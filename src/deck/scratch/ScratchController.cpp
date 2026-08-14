#include "ScratchController.h"

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

    m_wasPlayingBeforeScratch.store(wasPlayingBeforeScratch, std::memory_order_relaxed);
    m_releaseAllowsInertia.store(true, std::memory_order_relaxed);
    setNormalPlaybackSpeed(normalPlaybackSpeed);
    m_handPositionSec.store(startSec, std::memory_order_relaxed);
    m_rawSpeed.store(0.0, std::memory_order_relaxed);
    m_smoothedSpeed.store(0.0, std::memory_order_relaxed);
    m_commandedHandSpeed.store(0.0, std::memory_order_relaxed);
    m_inertiaSpeed.store(0.0, std::memory_order_relaxed);
    m_releaseTargetSpeed.store(0.0, std::memory_order_relaxed);
    m_readPosition.store(audioSamplePos, std::memory_order_relaxed);
    m_lastMoveNs.store(nowNs(), std::memory_order_relaxed);
    m_releaseEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_appliedReleaseSpeedGeneration.store(
        m_releaseSpeedGeneration.load(std::memory_order_acquire),
        std::memory_order_release);
    // Publish the phase last so the callback cannot observe a new grab with
    // velocity/read-position state left over from the previous release.
    m_phase.store(ScratchPhase::TouchTracking, std::memory_order_release);
}

void ScratchController::stopScratch() noexcept
{
    m_rawSpeed.store(0.0, std::memory_order_relaxed);
    m_smoothedSpeed.store(0.0, std::memory_order_relaxed);
    m_commandedHandSpeed.store(0.0, std::memory_order_relaxed);
    m_inertiaSpeed.store(0.0, std::memory_order_relaxed);
    m_releaseTargetSpeed.store(0.0, std::memory_order_relaxed);
    m_phase.store(ScratchPhase::Idle, std::memory_order_release);
}

bool ScratchController::requestRelease(bool allowInertia) noexcept
{
    m_releaseAllowsInertia.store(allowInertia, std::memory_order_relaxed);
    auto expected = ScratchPhase::TouchTracking;
    if (m_phase.compare_exchange_strong(expected,
                                        ScratchPhase::ReleasePending,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        return true;
    }
    return expected == ScratchPhase::ReleasePending
        || expected == ScratchPhase::CoastToDeckRate
        || expected == ScratchPhase::CoastToStop
        || expected == ScratchPhase::HandoffPending;
}

void ScratchController::setTouching(bool touching) noexcept
{
    if (!touching) {
        (void) requestRelease(m_inertiaEnabled.load(std::memory_order_relaxed));
        return;
    }

    const auto current = phase();
    if (current != ScratchPhase::Idle)
        m_phase.store(ScratchPhase::TouchTracking, std::memory_order_release);
}

ScratchReleaseDisposition ScratchController::releaseScratch() noexcept
{
    const bool allowInertia = m_inertiaEnabled.load(std::memory_order_relaxed);
    if (!requestRelease(allowInertia))
        return ScratchReleaseDisposition::HandoffNow;
    return releaseScratchWithSpeed(
        m_smoothedSpeed.load(std::memory_order_relaxed),
        allowInertia);
}

ScratchReleaseDisposition ScratchController::releaseScratchWithSpeed(
    double measuredNormalizedSpeed,
    bool allowInertia) noexcept
{
    return releaseScratchWithSpeed(
        measuredNormalizedSpeed,
        allowInertia,
        m_normalPlaybackSpeed.load(std::memory_order_relaxed),
        m_wasPlayingBeforeScratch.load(std::memory_order_relaxed));
}

ScratchReleaseDisposition ScratchController::releaseScratchWithSpeed(
    double measuredNormalizedSpeed,
    bool allowInertia,
    double signedDeckSpeed,
    bool wasPlaying) noexcept
{
    const auto current = phase();
    if (current == ScratchPhase::CoastToDeckRate)
        return ScratchReleaseDisposition::CoastToDeckRate;
    if (current == ScratchPhase::CoastToStop)
        return ScratchReleaseDisposition::CoastToStop;
    if (current != ScratchPhase::ReleasePending)
        return ScratchReleaseDisposition::HandoffNow;

    const double speed = std::clamp(measuredNormalizedSpeed,
                                    -m_config.maxScratchSpeed,
                                    m_config.maxScratchSpeed);
    const double clampedDeckSpeed = std::clamp(signedDeckSpeed,
                                               -m_config.maxScratchSpeed,
                                               m_config.maxScratchSpeed);
    const double deckSpeed = wasPlaying ? clampedDeckSpeed : 0.0;
    const double threshold = std::max(m_config.inertiaStopThreshold, m_config.minScratchSpeed);
    const bool deckMoving = std::abs(deckSpeed) > threshold;
    const bool sameDirection = deckMoving && speed * deckSpeed > 0.0;
    const bool inertiaAllowed = allowInertia
        && m_inertiaEnabled.load(std::memory_order_relaxed);

    ScratchPhase nextPhase = ScratchPhase::HandoffPending;
    ScratchReleaseDisposition disposition = ScratchReleaseDisposition::HandoffNow;
    if (!inertiaAllowed || std::abs(speed) <= threshold) {
        m_inertiaSpeed.store(deckSpeed, std::memory_order_relaxed);
        m_releaseTargetSpeed.store(deckSpeed, std::memory_order_relaxed);
    } else if (sameDirection && std::abs(speed) <= std::abs(deckSpeed) + threshold) {
        // A same-direction platter that has already reached (or fallen below)
        // deck rate must not be accelerated back up by the scratch engine.
        m_inertiaSpeed.store(deckSpeed, std::memory_order_relaxed);
        m_releaseTargetSpeed.store(deckSpeed, std::memory_order_relaxed);
    } else {
        m_releaseTargetSpeed.store(sameDirection ? deckSpeed : 0.0,
                                   std::memory_order_relaxed);
        m_inertiaSpeed.store(speed, std::memory_order_relaxed);
        nextPhase = sameDirection
            ? ScratchPhase::CoastToDeckRate
            : ScratchPhase::CoastToStop;
        disposition = sameDirection
            ? ScratchReleaseDisposition::CoastToDeckRate
            : ScratchReleaseDisposition::CoastToStop;
    }

    auto expected = ScratchPhase::ReleasePending;
    if (!m_phase.compare_exchange_strong(expected,
                                         nextPhase,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        // A re-grab owns the controller now. The bridge's release generation
        // will observe its cancellation and must not hand off the new grab.
        return ScratchReleaseDisposition::HandoffNow;
    }
    return disposition;
}

ScratchReleaseDisposition ScratchController::retargetRelease(
    bool playbackIntent,
    double signedDeckSpeed) noexcept
{
    auto current = phase();
    if (current != ScratchPhase::CoastToDeckRate
        && current != ScratchPhase::CoastToStop
        && current != ScratchPhase::HandoffPending)
        return ScratchReleaseDisposition::CoastToStop;

    const double threshold = std::max(m_config.inertiaStopThreshold,
                                      m_config.minScratchSpeed);
    const double inertia = m_inertiaSpeed.load(std::memory_order_relaxed);
    const double deckSpeed = playbackIntent
        ? std::clamp(signedDeckSpeed, -m_config.maxScratchSpeed,
                     m_config.maxScratchSpeed)
        : 0.0;
    const bool sameDirection = std::abs(deckSpeed) > threshold
        && inertia * deckSpeed > 0.0;

    ScratchPhase next = ScratchPhase::CoastToStop;
    ScratchReleaseDisposition disposition = ScratchReleaseDisposition::CoastToStop;
    double target = 0.0;

    if (playbackIntent && sameDirection) {
        target = deckSpeed;
        if (std::abs(inertia) <= std::abs(deckSpeed) + threshold) {
            next = ScratchPhase::HandoffPending;
            disposition = ScratchReleaseDisposition::HandoffNow;
        } else {
            next = ScratchPhase::CoastToDeckRate;
            disposition = ScratchReleaseDisposition::CoastToDeckRate;
        }
    } else if (std::abs(inertia) <= threshold) {
        next = ScratchPhase::HandoffPending;
        disposition = ScratchReleaseDisposition::HandoffNow;
    }

    m_releaseTargetSpeed.store(target, std::memory_order_relaxed);
    auto expected = current;
    if (!m_phase.compare_exchange_strong(expected, next,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        // A fresh platter grab supersedes this transport-intent change.
        return ScratchReleaseDisposition::HandoffNow;
    }
    return disposition;
}

bool ScratchController::completeHandoff() noexcept
{
    auto expected = ScratchPhase::HandoffPending;
    // Do not clear velocity atomics after the CAS: startScratch publishes its
    // phase last, so a post-CAS clear could erase a concurrent re-grab's freshly
    // initialized state. Idle never consumes the old values and the next grab
    // overwrites all of them before publishing TouchTracking.
    return m_phase.compare_exchange_strong(expected,
                                           ScratchPhase::Idle,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

void ScratchController::submitHandDelta(double deltaTrackSec, double dtSec) noexcept
{
    if (dtSec < 1e-6 || phase() != ScratchPhase::TouchTracking)
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
    m_commandedHandSpeed.store(velocity, std::memory_order_relaxed);
    m_lastMoveNs.store(nowNs(), std::memory_order_relaxed);
}

double ScratchController::commandedHandSpeed() const noexcept
{
    const double commanded = m_commandedHandSpeed.load(std::memory_order_relaxed);
    const double idleMs = timeSinceLastMoveMs();
    if (idleMs <= m_config.commandVelocityHoldMs)
        return commanded;

    const double tauMs = std::max(1.0, m_config.commandVelocityDecayTauMs);
    const double gain = std::exp(-(idleMs - m_config.commandVelocityHoldMs) / tauMs);
    const double decayed = commanded * gain;
    return std::abs(decayed) < m_config.minScratchSpeed ? 0.0 : decayed;
}

void ScratchController::submitReleaseDelta(double deltaTrackSec, double dtSec) noexcept
{
    if (dtSec < 1e-6)
        return;

    submitReleaseSpeed(deltaTrackSec / dtSec);
}

void ScratchController::submitReleaseSpeed(double measuredNormalizedSpeed) noexcept
{
    const auto releaseEpoch = m_releaseEpoch.load(std::memory_order_acquire);
    const auto current = phase();
    if (current != ScratchPhase::ReleasePending
        && current != ScratchPhase::CoastToDeckRate
        && current != ScratchPhase::CoastToStop) {
        return;
    }

    const double measured = std::clamp(measuredNormalizedSpeed,
                                       -m_config.maxScratchSpeed,
                                       m_config.maxScratchSpeed);
    m_submittedReleaseEpoch.store(releaseEpoch, std::memory_order_relaxed);
    m_submittedReleaseSpeed.store(measured, std::memory_order_relaxed);
    m_releaseSpeedGeneration.fetch_add(1, std::memory_order_release);
}

double ScratchController::processAudioBlock(int bufferSize,
                                            double outputSampleRate,
                                            double trackSampleRate) noexcept
{
    if (!m_enabled.load(std::memory_order_relaxed))
        return 0.0;

    const double oneX = oneXResamplerRate(outputSampleRate, trackSampleRate);
    double finalNormalized = 0.0;
    auto currentPhase = phase();

    if (currentPhase == ScratchPhase::TouchTracking
        || currentPhase == ScratchPhase::ReleasePending) {
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
    } else if (currentPhase == ScratchPhase::CoastToDeckRate
               || currentPhase == ScratchPhase::CoastToStop) {
        double inertia = m_inertiaSpeed.load(std::memory_order_relaxed);
        const double target = m_releaseTargetSpeed.load(std::memory_order_relaxed);
        const double threshold = std::max(m_config.inertiaStopThreshold,
                                          m_config.minScratchSpeed);

        const auto speedGeneration = m_releaseSpeedGeneration.load(std::memory_order_acquire);
        const auto appliedGeneration = m_appliedReleaseSpeedGeneration.load(
            std::memory_order_relaxed);
        if (speedGeneration != appliedGeneration) {
            const auto submittedEpoch = m_submittedReleaseEpoch.load(std::memory_order_relaxed);
            const auto currentEpoch = m_releaseEpoch.load(std::memory_order_acquire);
            if (submittedEpoch == currentEpoch) {
                const double measured = m_submittedReleaseSpeed.load(std::memory_order_relaxed);
                const bool reachedHandoff = currentPhase == ScratchPhase::CoastToDeckRate
                    ? measured * target <= 0.0
                        || std::abs(measured) <= std::abs(target) + threshold
                    : measured * inertia <= 0.0 || std::abs(measured) <= threshold;

                if (reachedHandoff) {
                    auto expected = currentPhase;
                    if (m_phase.compare_exchange_strong(
                            expected,
                            ScratchPhase::HandoffPending,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        inertia = target;
                        currentPhase = ScratchPhase::HandoffPending;
                    } else {
                        currentPhase = expected;
                    }
                } else {
                    inertia += (measured - inertia) * 0.55;
                }
            }
            m_appliedReleaseSpeedGeneration.store(speedGeneration, std::memory_order_release);
        }

        if (currentPhase == ScratchPhase::HandoffPending) {
            m_inertiaSpeed.store(inertia, std::memory_order_relaxed);
            finalNormalized = inertia;
        } else if (currentPhase == ScratchPhase::CoastToDeckRate
                   || currentPhase == ScratchPhase::CoastToStop) {
            const double blockSec = static_cast<double>(std::max(1, bufferSize))
                                  / std::max(1.0, outputSampleRate);
            const double alpha = 1.0 - std::exp(
                -blockSec / std::max(0.001, m_config.releaseReturnTauSec));
            inertia += (target - inertia) * std::clamp(alpha, 0.0, 1.0);

            if (std::abs(inertia - target) < threshold) {
                auto expected = currentPhase;
                if (m_phase.compare_exchange_strong(
                        expected,
                        ScratchPhase::HandoffPending,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    inertia = target;
                    currentPhase = ScratchPhase::HandoffPending;
                } else {
                    currentPhase = expected;
                }
            }
            if (currentPhase == ScratchPhase::CoastToDeckRate
                || currentPhase == ScratchPhase::CoastToStop
                || currentPhase == ScratchPhase::HandoffPending) {
                m_inertiaSpeed.store(inertia, std::memory_order_relaxed);
                finalNormalized = inertia;
            }
        }
    } else if (currentPhase == ScratchPhase::HandoffPending) {
        finalNormalized = m_releaseTargetSpeed.load(std::memory_order_relaxed);
    }

    const double finalRate = finalNormalized * oneX;

    const double readPos = m_readPosition.load(std::memory_order_relaxed);
    m_readPosition.store(readPos + finalRate * static_cast<double>(std::max(1, bufferSize)),
                         std::memory_order_relaxed);

    return finalRate;
}

} // namespace engine::scratch
