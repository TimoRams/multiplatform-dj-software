#include "ScratchController.hpp"

#include <algorithm>
#include <cmath>

namespace engine::scratch {

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

ScratchReleaseDisposition ScratchController::releaseScratch() noexcept
{
    m_touching.store(false, std::memory_order_relaxed);

    const double speed = m_smoothedSpeed.load(std::memory_order_relaxed);
    const double deckSpeed = m_wasPlayingBeforeScratch.load(std::memory_order_relaxed)
        ? m_normalPlaybackSpeed.load(std::memory_order_relaxed)
        : 0.0;
    const double threshold = std::max(m_config.inertiaStopThreshold, m_config.minScratchSpeed);
    const bool deckMoving = std::abs(deckSpeed) > threshold;
    const bool sameDirection = deckMoving && speed * deckSpeed > 0.0;

    if (!m_inertiaEnabled.load(std::memory_order_relaxed) || std::abs(speed) <= threshold) {
        stopScratch();
        return ScratchReleaseDisposition::HandoffNow;
    }

    // A same-direction platter that has already reached (or fallen below) the
    // deck rate must not be accelerated back up by the scratch engine.
    if (sameDirection && std::abs(speed) <= std::abs(deckSpeed) + threshold) {
        stopScratch();
        return ScratchReleaseDisposition::HandoffNow;
    }

    m_releaseTargetSpeed.store(sameDirection ? deckSpeed : 0.0, std::memory_order_relaxed);
    m_inertiaSpeed.store(speed, std::memory_order_relaxed);
    m_inertiaActive.store(true, std::memory_order_relaxed);
    m_active.store(true, std::memory_order_relaxed);
    return sameDirection ? ScratchReleaseDisposition::CoastToDeckRate
                         : ScratchReleaseDisposition::CoastToStop;
}

void ScratchController::submitHandDelta(double deltaTrackSec, double dtSec) noexcept
{
    if (dtSec < 1e-6 || !m_active.load(std::memory_order_relaxed))
        return;

    double raw = deltaTrackSec / dtSec;
    raw = std::clamp(raw, -m_config.maxScratchSpeed, m_config.maxScratchSpeed);

    const double target = m_handPositionSec.load(std::memory_order_relaxed) + deltaTrackSec;
    const double oldVelocity = m_smoothedSpeed.load(std::memory_order_relaxed);

    // UI/MIDI deltas are already integrated into position — keep rate close to
    // the instantaneous delta/dt so audio integration does not drift far ahead
    // of the hand and snap back on the next drag event.
    double velocity = raw;
    if ((oldVelocity * raw) < 0.0 && std::abs(raw) > 0.04)
        velocity = oldVelocity + (raw - oldVelocity) * 0.45;
    else if (std::abs(raw) < m_config.slowSpeedThreshold)
        velocity = oldVelocity + (raw - oldVelocity) * 0.72;
    velocity = std::clamp(velocity, -m_config.maxScratchSpeed, m_config.maxScratchSpeed);

    m_handPositionSec.store(target, std::memory_order_relaxed);
    m_rawSpeed.store(raw, std::memory_order_relaxed);
    m_smoothedSpeed.store(velocity, std::memory_order_relaxed);
    m_lastMoveNs.store(nowNs(), std::memory_order_relaxed);
}

void ScratchController::submitReleaseDelta(double deltaTrackSec, double dtSec) noexcept
{
    if (dtSec < 1e-6 || !m_inertiaActive.load(std::memory_order_relaxed))
        return;

    const double measured = std::clamp(deltaTrackSec / dtSec,
                                       -m_config.maxScratchSpeed,
                                       m_config.maxScratchSpeed);
    const double target = m_releaseTargetSpeed.load(std::memory_order_relaxed);
    const double threshold = std::max(m_config.inertiaStopThreshold, m_config.minScratchSpeed);
    const double current = m_inertiaSpeed.load(std::memory_order_relaxed);

    if (std::abs(target) > threshold) {
        const bool sameDirection = measured * target > 0.0;
        if (!sameDirection || std::abs(measured) <= std::abs(target) + threshold) {
            // The physical wheel has reached the deck speed. Finish at the
            // normal path instead of following its slower residual spin.
            m_inertiaSpeed.store(target, std::memory_order_relaxed);
            m_inertiaActive.store(false, std::memory_order_relaxed);
            m_active.store(false, std::memory_order_relaxed);
            return;
        }
    } else if (measured * current <= 0.0 || std::abs(measured) <= threshold) {
        // Reverse throws coast only until they stop, then normal playback owns
        // the direction again.
        m_inertiaSpeed.store(0.0, std::memory_order_relaxed);
        m_inertiaActive.store(false, std::memory_order_relaxed);
        m_active.store(false, std::memory_order_relaxed);
        return;
    }

    const double smoothed = current + (measured - current) * 0.55;
    m_inertiaSpeed.store(smoothed, std::memory_order_relaxed);
    m_rawSpeed.store(measured, std::memory_order_relaxed);
    m_smoothedSpeed.store(smoothed, std::memory_order_relaxed);
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

    return finalRate;
}

} // namespace engine::scratch
