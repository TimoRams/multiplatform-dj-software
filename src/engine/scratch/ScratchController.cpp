#include "ScratchController.hpp"

#include <QDebug>
#include <algorithm>
#include <cmath>

namespace engine::scratch {

namespace {
constexpr bool kScratchDebugLog = false;
constexpr int kScratchDebugIntervalBlocks = 50;
} // namespace

uint64_t ScratchController::nowNs() noexcept
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
}

void ScratchController::setTrackSampleRate(double sampleRate) noexcept
{
    m_trackSampleRate.store(std::max(1.0, sampleRate), std::memory_order_relaxed);
}

double ScratchController::timeSinceLastMoveMs() const noexcept
{
    const uint64_t last = m_lastMoveNs.load(std::memory_order_relaxed);
    if (last == 0)
        return 0.0;
    const uint64_t now = nowNs();
    return static_cast<double>(now - last) * 1e-6;
}

double ScratchController::oneXResamplerRate(double outputSampleRate, double trackSampleRate) const noexcept
{
    const double outSr = std::max(1.0, outputSampleRate);
    const double trackSr = std::max(1.0, trackSampleRate);
    return trackSr / outSr;
}

void ScratchController::startScratch(double audioSamplePos,
                                     bool wasPlayingBeforeScratch,
                                     double normalPlaybackSpeed) noexcept
{
    const double trackSr = m_trackSampleRate.load(std::memory_order_relaxed);
    const double startSec = audioSamplePos / std::max(1.0, trackSr);

    m_active.store(true, std::memory_order_relaxed);
    m_touching.store(true, std::memory_order_relaxed);
    m_inertiaActive.store(false, std::memory_order_relaxed);
    m_wasPlayingBeforeScratch.store(wasPlayingBeforeScratch, std::memory_order_relaxed);
    setNormalPlaybackSpeed(normalPlaybackSpeed);
    m_handPositionSec.store(startSec, std::memory_order_relaxed);
    m_filteredPositionSec.store(startSec, std::memory_order_relaxed);
    m_rawSpeed.store(0.0, std::memory_order_relaxed);
    m_smoothedSpeed.store(0.0, std::memory_order_relaxed);
    m_inertiaSpeed.store(0.0, std::memory_order_relaxed);
    m_releaseTargetSpeed.store(0.0, std::memory_order_relaxed);
    m_readPosition.store(audioSamplePos, std::memory_order_relaxed);
    m_lastMoveNs.store(nowNs(), std::memory_order_relaxed);
}

void ScratchController::stopScratch() noexcept
{
    m_active.store(false, std::memory_order_relaxed);
    m_touching.store(false, std::memory_order_relaxed);
    m_inertiaActive.store(false, std::memory_order_relaxed);
    m_rawSpeed.store(0.0, std::memory_order_relaxed);
    m_smoothedSpeed.store(0.0, std::memory_order_relaxed);
    m_inertiaSpeed.store(0.0, std::memory_order_relaxed);
    m_releaseTargetSpeed.store(0.0, std::memory_order_relaxed);
}

void ScratchController::releaseScratch() noexcept
{
    m_touching.store(false, std::memory_order_relaxed);

    const double speed = m_smoothedSpeed.load(std::memory_order_relaxed);
    const double target = m_wasPlayingBeforeScratch.load(std::memory_order_relaxed)
        ? m_normalPlaybackSpeed.load(std::memory_order_relaxed)
        : 0.0;
    m_releaseTargetSpeed.store(target, std::memory_order_relaxed);

    if (m_inertiaEnabled.load(std::memory_order_relaxed)
            && std::abs(speed - target) > m_config.throwThreshold) {
        m_inertiaSpeed.store(speed, std::memory_order_relaxed);
        m_inertiaActive.store(true, std::memory_order_relaxed);
        return;
    }

    m_inertiaActive.store(false, std::memory_order_relaxed);
    m_active.store(false, std::memory_order_relaxed);
}

void ScratchController::submitHandDelta(double deltaTrackSec, double dtSec) noexcept
{
    if (dtSec < 1e-6 || !m_active.load(std::memory_order_relaxed))
        return;

    double raw = deltaTrackSec / dtSec;
    raw = std::clamp(raw, -m_config.maxScratchSpeed, m_config.maxScratchSpeed);

    const double safeDt = std::clamp(dtSec, 0.001, 0.100);
    const double target = m_handPositionSec.load(std::memory_order_relaxed) + deltaTrackSec;
    const double oldPosition = m_filteredPositionSec.load(std::memory_order_relaxed);
    const double oldVelocity = m_smoothedSpeed.load(std::memory_order_relaxed);
    const double predicted = oldPosition + oldVelocity * safeDt;
    const double residual = target - predicted;

    const double filteredPosition = predicted + m_config.alpha * residual;
    double filteredVelocity = oldVelocity + m_config.beta * residual / safeDt;
    filteredVelocity = filteredVelocity * (1.0 - m_config.rawVelocityMix)
                     + raw * m_config.rawVelocityMix;
    filteredVelocity = std::clamp(filteredVelocity, -m_config.maxScratchSpeed, m_config.maxScratchSpeed);

    m_handPositionSec.store(target, std::memory_order_relaxed);
    m_filteredPositionSec.store(filteredPosition, std::memory_order_relaxed);
    m_rawSpeed.store(raw, std::memory_order_relaxed);
    m_smoothedSpeed.store(filteredVelocity, std::memory_order_relaxed);
    m_lastMoveNs.store(nowNs(), std::memory_order_relaxed);
}

double ScratchController::processAudioBlock(int bufferSize,
                                            double outputSampleRate,
                                            double trackSampleRate) noexcept
{
    if (!m_enabled.load(std::memory_order_relaxed))
        return 0.0;

    const double oneX = oneXResamplerRate(outputSampleRate, trackSampleRate);
    double finalNormalized = 0.0;

    if (m_touching.load(std::memory_order_relaxed)) {
        finalNormalized = m_smoothedSpeed.load(std::memory_order_relaxed);

        const double idleMs = timeSinceLastMoveMs();
        if (idleMs > m_config.noMoveDecayMs) {
            const double blockSec = static_cast<double>(std::max(1, bufferSize))
                                  / std::max(1.0, outputSampleRate);
            const double decay = std::exp(-blockSec / std::max(0.001, m_config.noMoveDecayTauSec));
            finalNormalized *= decay;
            if (std::abs(finalNormalized) < m_config.minScratchSpeed)
                finalNormalized = 0.0;
            m_smoothedSpeed.store(finalNormalized, std::memory_order_relaxed);
        }
    } else if (m_inertiaActive.load(std::memory_order_relaxed)) {
        double inertia = m_inertiaSpeed.load(std::memory_order_relaxed);
        const double target = m_releaseTargetSpeed.load(std::memory_order_relaxed);
        const double blockSec = static_cast<double>(std::max(1, bufferSize))
                              / std::max(1.0, outputSampleRate);
        const double alpha = 1.0 - std::exp(-blockSec / std::max(0.001, m_config.releaseReturnTauSec));
        inertia += (target - inertia) * std::clamp(alpha, 0.0, 1.0);

        if (std::abs(inertia - target) < m_config.inertiaStopThreshold) {
            inertia = target;
            m_inertiaActive.store(false, std::memory_order_relaxed);
            m_active.store(false, std::memory_order_relaxed);
        }
        m_inertiaSpeed.store(inertia, std::memory_order_relaxed);
        finalNormalized = inertia;
    } else if (m_active.load(std::memory_order_relaxed)) {
        finalNormalized = 0.0;
        m_active.store(false, std::memory_order_relaxed);
    }

    const double finalRate = finalNormalized * oneX;

    const double readPos = m_readPosition.load(std::memory_order_relaxed);
    m_readPosition.store(readPos + finalRate * static_cast<double>(std::max(1, bufferSize)),
                         std::memory_order_relaxed);

    if constexpr (kScratchDebugLog) {
        if (++m_debugBlockCounter >= kScratchDebugIntervalBlocks) {
            m_debugBlockCounter = 0;
            qDebug() << "scratch"
                     << "touching" << m_touching.load(std::memory_order_relaxed)
                     << "active" << m_active.load(std::memory_order_relaxed)
                     << "inertia" << m_inertiaActive.load(std::memory_order_relaxed)
                     << "raw" << m_rawSpeed.load(std::memory_order_relaxed)
                     << "smoothed" << m_smoothedSpeed.load(std::memory_order_relaxed)
                     << "finalNorm" << finalNormalized
                     << "finalRate" << finalRate
                     << "releaseTarget" << m_releaseTargetSpeed.load(std::memory_order_relaxed)
                     << "wasPlaying" << m_wasPlayingBeforeScratch.load(std::memory_order_relaxed)
                     << "idleMs" << timeSinceLastMoveMs();
        }
    }

    return finalRate;
}

} // namespace engine::scratch
