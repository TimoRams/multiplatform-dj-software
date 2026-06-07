#include "ScratchController.hpp"

#include <algorithm>

namespace engine::scratch {

void ScratchController::resetControllerState(double audioSamplePos, double targetSamplePos) noexcept
{
    m_previousAudioSamplePos = audioSamplePos;
    m_scratchStartTargetPos = targetSamplePos;
    m_audioDeltaSum = 0.0;
    m_targetDeltaLast = 0.0;
    m_previousError = 0.0;
    m_filteredError = 0.0;
    m_moveDelay = 0.0;
    m_scratchPosSampleTime = 0.0;
    m_callsSinceInterval = 0;
    m_timeSinceTargetMove = 0.0;
    m_rate.store(0.0, std::memory_order_relaxed);
    m_readPosition.store(audioSamplePos, std::memory_order_relaxed);
}

void ScratchController::startScratch(double audioSamplePos, double targetSamplePos) noexcept
{
    m_isScratching.store(true, std::memory_order_relaxed);
    m_inertiaActive.store(false, std::memory_order_relaxed);
    m_targetSamplePos.store(targetSamplePos, std::memory_order_relaxed);
    resetControllerState(audioSamplePos, targetSamplePos);
    ++m_targetMoveGeneration;
}

void ScratchController::setTrackSampleRate(double sampleRate) noexcept
{
    m_trackSampleRate.store(std::max(1.0, sampleRate), std::memory_order_relaxed);
}

void ScratchController::stopScratch() noexcept
{
    m_isScratching.store(false, std::memory_order_relaxed);
    m_inertiaActive.store(false, std::memory_order_relaxed);
    m_rate.store(0.0, std::memory_order_relaxed);
}

void ScratchController::releaseScratch() noexcept
{
    const double currentRate = m_rate.load(std::memory_order_relaxed);
    if (m_inertiaEnabled.load(std::memory_order_relaxed)
            && std::abs(currentRate) > m_config.throwThreshold) {
        m_inertiaActive.store(true, std::memory_order_relaxed);
        m_isScratching.store(false, std::memory_order_relaxed);
        return;
    }

    stopScratch();
}

void ScratchController::setTargetSamplePosition(double targetSamples) noexcept
{
    m_targetSamplePos.store(targetSamples, std::memory_order_relaxed);
    notifyTargetMoved();
}

void ScratchController::addTargetSampleDelta(double deltaSamples) noexcept
{
    if (deltaSamples == 0.0)
        return;

    const double next = m_targetSamplePos.load(std::memory_order_relaxed) + deltaSamples;
    m_targetSamplePos.store(next, std::memory_order_relaxed);
    notifyTargetMoved();
}

void ScratchController::notifyTargetMoved() noexcept
{
    m_moveDelay = 0.0;
    m_timeSinceTargetMove = 0.0;
    ++m_targetMoveGeneration;
}

double ScratchController::correctSampleDeltaForLoop(double sampleDelta,
                                                    bool loopActive,
                                                    double loopInSample,
                                                    double loopOutSample) const noexcept
{
    if (!loopActive || loopOutSample <= loopInSample + 1.0)
        return sampleDelta;

    const double loopLen = loopOutSample - loopInSample;
    if (loopLen <= 0.0)
        return sampleDelta;

    const double wraps = std::trunc(sampleDelta / loopLen);
    return sampleDelta - wraps * loopLen;
}

void ScratchController::runPdTick(double targetDeltaNormalized) noexcept
{
    const double error = targetDeltaNormalized - m_audioDeltaSum;
    m_filteredError += m_config.filterFactor * (error - m_filteredError);
    const double dTerm = m_filteredError - m_previousError;
    double nextRate = m_config.p * m_filteredError + m_config.d * dTerm;
    nextRate /= static_cast<double>(std::max(1, m_callsPerDt));
    m_rate.store(nextRate, std::memory_order_relaxed);
    clampRate(false);
    m_previousError = m_filteredError;
    m_targetDeltaLast = targetDeltaNormalized;
}

void ScratchController::clampRate(bool snapSmallToZero) noexcept
{
    double r = m_rate.load(std::memory_order_relaxed);
    r = std::clamp(r, -m_config.maxScratchRate, m_config.maxScratchRate);
    if (snapSmallToZero && std::abs(r) < m_config.minScratchRate)
        r = 0.0;
    m_rate.store(r, std::memory_order_relaxed);
}

double ScratchController::processAudioBlock(double currentAudioSamplePos,
                                            int bufferSize,
                                            double outputSampleRate,
                                            double baseSampleRateRatio,
                                            bool loopActive,
                                            double loopInSample,
                                            double loopOutSample) noexcept
{
    if (!m_enabled.load(std::memory_order_relaxed))
        return 0.0;

    if (m_inertiaActive.load(std::memory_order_relaxed)) {
        m_readPosition.store(currentAudioSamplePos, std::memory_order_relaxed);
        return m_rate.load(std::memory_order_relaxed);
    }

    if (!m_isScratching.load(std::memory_order_relaxed))
        return 0.0;

    m_bufferSize = std::max(1, bufferSize);
    m_dt = static_cast<double>(m_bufferSize)
         / std::max(1.0, outputSampleRate);

    double sampleDelta = currentAudioSamplePos - m_previousAudioSamplePos;
    sampleDelta = correctSampleDeltaForLoop(sampleDelta, loopActive, loopInSample, loopOutSample);

    const double norm = static_cast<double>(m_bufferSize) * std::max(1e-9, baseSampleRateRatio);
    m_audioDeltaSum += sampleDelta / norm;
    m_previousAudioSamplePos = currentAudioSamplePos;
    m_readPosition.store(currentAudioSamplePos, std::memory_order_relaxed);

    m_timeSinceTargetMove += m_dt;

    const bool targetJustMoved = (m_targetMoveGeneration != m_lastSeenTargetGeneration);
    if (targetJustMoved) {
        m_lastSeenTargetGeneration = m_targetMoveGeneration;
        m_moveDelay = 0.0;
        m_timeSinceTargetMove = 0.0;
        // Run PD on the next sample of this block — don't wait a full interval.
        m_scratchPosSampleTime = m_config.sampleIntervalSeconds;
    }

    m_scratchPosSampleTime += m_dt;
    ++m_callsSinceInterval;

    const double interval = m_config.sampleIntervalSeconds;
    if (m_scratchPosSampleTime >= interval) {
        const double targetNow = m_targetSamplePos.load(std::memory_order_relaxed);
        const double targetDeltaNorm = (targetNow - m_scratchStartTargetPos) / norm;
        m_callsPerDt = std::max(1, m_callsSinceInterval);
        m_callsSinceInterval = 0;
        m_scratchPosSampleTime = 0.0;

        if (m_timeSinceTargetMove < m_config.moveDelayMaxSeconds) {
            runPdTick(targetDeltaNorm);
        } else {
            // Platter stopped — pull rate toward zero via zero target delta growth
            const double decayedTarget = m_targetDeltaLast
                + (targetDeltaNorm - m_targetDeltaLast) * 0.35;
            runPdTick(decayedTarget);
            double r = m_rate.load(std::memory_order_relaxed);
            r *= 0.82;
            m_rate.store(r, std::memory_order_relaxed);
            clampRate(true);
        }
    } else if (m_timeSinceTargetMove < m_config.moveDelayMaxSeconds) {
        // Assume delayed UI event — hold previous rate
    } else {
        double r = m_rate.load(std::memory_order_relaxed);
        r *= 0.90;
        m_rate.store(r, std::memory_order_relaxed);
        clampRate(true);
    }

    return m_rate.load(std::memory_order_relaxed);
}

void ScratchController::tickInertia(double dtSeconds) noexcept
{
    if (!m_inertiaActive.load(std::memory_order_relaxed))
        return;

    double r = m_rate.load(std::memory_order_relaxed);
    const double tau = std::max(0.05, m_config.timeToStopSeconds);
    const double alpha = 1.0 - std::exp(-dtSeconds / tau);
    r += (0.0 - r) * alpha;
    m_rate.store(r, std::memory_order_relaxed);

    const double trackSr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double readPos = m_readPosition.load(std::memory_order_relaxed);
    m_readPosition.store(readPos + r * dtSeconds * trackSr, std::memory_order_relaxed);

    if (std::abs(r) < m_config.minScratchRate * 4.0) {
        m_rate.store(0.0, std::memory_order_relaxed);
        m_inertiaActive.store(false, std::memory_order_relaxed);
    }
}

} // namespace engine::scratch
