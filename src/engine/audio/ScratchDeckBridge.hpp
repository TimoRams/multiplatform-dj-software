#pragma once

#include "ScratchResampler.hpp"
#include "../scratch/ScratchController.hpp"
#include "../scratch/VirtualTurntable.hpp"

#include "HermiteResamplingAudioSource.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>

namespace engine::audio {

// Normal playback: proven Hermite varispeed (keylock off) or transport pass-through (keylock on).
// Scratch playback: Mixxx-style PD controller + dedicated scratch resampler.
class ScratchDeckBridge : public juce::AudioSource {
public:
    explicit ScratchDeckBridge(juce::AudioSource* inputSource, bool deleteInputWhenDeleted = false);

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    void beginScratch(double anchorSeconds, double trackSampleRate, double trackLengthSeconds);
    void endScratch(bool allowInertia);
    void engageScratchDuringInertia() noexcept;
    void syncTargetFromPlatter(const engine::scratch::VirtualTurntable& platter) noexcept;
    void addTargetDeltaSeconds(double deltaSeconds, double trackSampleRate) noexcept;
    void setAbsoluteTargetSeconds(double seconds, double trackSampleRate) noexcept;

    void configureTrack(double trackSampleRate, double trackLengthSeconds) noexcept;
    void syncReadPositionSeconds(double positionSeconds, double trackSampleRate) noexcept;
    void exitScratchMode(double positionSeconds, double trackSampleRate) noexcept;

    void setDeckTempoRatio(double ratio) noexcept;
    void setKeylockPassthrough(bool enabled) noexcept {
        m_keylockPassthrough.store(enabled, std::memory_order_relaxed);
    }
    void setLoopRangeSeconds(double loopInSec, double loopOutSec, bool active,
                             double trackSampleRate) noexcept;
    void setReverse(bool reverse) noexcept;

    void tickControlThread(double dtSeconds) noexcept;

    [[nodiscard]] bool isScratching() const noexcept;
    [[nodiscard]] bool isInertiaActive() const noexcept;
    [[nodiscard]] double scratchRate() const noexcept;
    [[nodiscard]] double readPositionSeconds(double trackSampleRate) const noexcept;
    [[nodiscard]] double targetPositionSeconds(double trackSampleRate) const noexcept;

    void syncReadToTarget(double trackSampleRate) noexcept;
    void snapHermiteToDeckTempo() noexcept;

    [[nodiscard]] engine::scratch::VirtualTurntable& platter() noexcept { return m_platter; }
    [[nodiscard]] const engine::scratch::VirtualTurntable& platter() const noexcept { return m_platter; }

    void armScalerCrossfade() noexcept { m_crossfadeRemaining.store(kCrossfadeSamples, std::memory_order_relaxed); }

    // Positionable deck reader used for scratch pulls (bypasses transport clock).
    void setScratchInputSource(juce::AudioSource* source) noexcept { m_scratchInput = source; }

    // Blocks audio output while DjEngine swaps transport reader sources.
    void beginTransportSwap() noexcept { m_transportSwapInProgress.store(true, std::memory_order_release); }
    void endTransportSwap() noexcept { m_transportSwapInProgress.store(false, std::memory_order_release); }

private:
    void applyDeckTempoToHermite() noexcept;
    double activePlaybackRate(double trackSampleRate) noexcept;
    void applyNormalPathCrossfade(const juce::AudioSourceChannelInfo& info) noexcept;
    [[nodiscard]] bool isScratchPathActive() const noexcept;

    juce::OptionalScopedPointer<juce::AudioSource> m_transport;
    juce::AudioSource* m_scratchInput = nullptr;
    std::unique_ptr<HermiteResamplingAudioSource> m_hermite;

    engine::scratch::ScratchController m_controller;
    engine::scratch::VirtualTurntable m_platter;
    ScratchResampler m_scratchResampler;

    std::atomic<double> m_deckTempoRatio { 1.0 };
    std::atomic<bool> m_keylockPassthrough { false };
    std::atomic<bool> m_reverse { false };
    std::atomic<double> m_trackSampleRate { 44100.0 };
    std::atomic<double> m_trackLengthSeconds { 0.0 };

    double m_outputSampleRate = 44100.0;
    int m_blockSize = 512;
    bool m_useScratchScaler = false;
    bool m_prevScratchPath = false;

    std::atomic<int> m_crossfadeRemaining { 0 };
    static constexpr int kCrossfadeSamples = 256;

    bool m_loopActive = false;
    double m_loopInSample = 0.0;
    double m_loopOutSample = 0.0;

    std::atomic<bool> m_transportSwapInProgress { false };
};

} // namespace engine::audio
