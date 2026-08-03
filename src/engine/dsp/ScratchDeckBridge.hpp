#pragma once

#include "ScratchResampler.hpp"
#include "../scratch/ScratchController.hpp"
#include "../scratch/VirtualTurntable.hpp"

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cstdint>
#include <memory>

class HermiteResamplingAudioSource;

namespace engine::audio {

// Normal playback: proven Hermite varispeed (keylock off) or transport pass-through (keylock on).
// Scratch playback: velocity-based virtual turntable + dedicated scratch resampler.
class ScratchDeckBridge : public juce::AudioSource {
public:
    explicit ScratchDeckBridge(juce::AudioSource* inputSource, bool deleteInputWhenDeleted = false);
    ~ScratchDeckBridge() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    void beginScratch(double anchorSeconds,
                      double trackSampleRate,
                      double trackLengthSeconds,
                      bool wasPlayingBeforeScratch,
                      double normalPlaybackSpeed);
    [[nodiscard]] engine::scratch::ScratchReleaseDisposition endScratch(bool allowInertia);
    void engageScratchDuringInertia() noexcept;
    void addTargetDeltaSeconds(double deltaSeconds, double trackSampleRate) noexcept;
    void submitHandDeltaSeconds(double deltaSeconds, double dtSeconds) noexcept;
    void submitReleaseDeltaSeconds(double deltaSeconds, double dtSeconds) noexcept;
    void syncScratchReadPosition(double displaySec, double trackSampleRate) noexcept;
    void publishScratchDisplay(double displaySec) noexcept;

    void configureTrack(double trackSampleRate, double trackLengthSeconds) noexcept;
    void syncReadPositionSeconds(double positionSeconds, double trackSampleRate) noexcept;
    void prepareNormalPlaybackHandoff(double positionSeconds, double trackSampleRate) noexcept;
    void exitScratchMode(double positionSeconds, double trackSampleRate) noexcept;

    void setDeckTempoRatio(double ratio) noexcept;
    void setJogNudgeRatio(double ratio) noexcept;
    void setKeylockPassthrough(bool enabled) noexcept {
        m_keylockPassthrough.store(enabled, std::memory_order_relaxed);
    }
    void setLoopRangeSeconds(double loopInSec, double loopOutSec, bool active,
                             double trackSampleRate) noexcept;
    void setReverse(bool reverse) noexcept;

    [[nodiscard]] bool isScratching() const noexcept;
    [[nodiscard]] bool isInertiaActive() const noexcept;
    [[nodiscard]] double scratchRate() const noexcept;
    [[nodiscard]] double readPositionSeconds(double trackSampleRate) const noexcept;
    [[nodiscard]] double displayPositionSeconds() const noexcept;

    void snapHermiteToDeckTempo() noexcept;

    [[nodiscard]] engine::scratch::VirtualTurntable& platter() noexcept { return m_platter; }
    [[nodiscard]] const engine::scratch::VirtualTurntable& platter() const noexcept { return m_platter; }

    void armScalerCrossfade() noexcept { m_crossfadeRemaining.store(kCrossfadeSamples, std::memory_order_relaxed); }

    void setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle) noexcept;
    [[nodiscard]] ScratchCacheStats scratchCacheStats() const noexcept { return m_scratchResampler.cacheStats(); }

    // Audio thread publishes scratch playhead here (seconds) for lock-free UI reads.
    void setAudioPlayheadSink(std::atomic<double>* sink) noexcept { m_audioPlayheadSink = sink; }

    // Blocks audio output while DjEngine swaps transport reader sources.
    void beginTransportSwap() noexcept { m_transportSwapInProgress.store(true, std::memory_order_release); }
    void endTransportSwap() noexcept { m_transportSwapInProgress.store(false, std::memory_order_release); }

private:
    void applyDeckTempoToHermite() noexcept;
    [[nodiscard]] double effectiveDeckTempoRatio() const noexcept;
    void consumePendingAudioCommands() noexcept;
    double activePlaybackRate(double trackSampleRate, int bufferSize) noexcept;
    void applyNormalPathCrossfade(const juce::AudioSourceChannelInfo& info) noexcept;
    [[nodiscard]] bool isScratchPathActive() const noexcept;

    juce::OptionalScopedPointer<juce::AudioSource> m_transport;
    juce::PositionableAudioSource* m_positionableTransportSource = nullptr;
    std::unique_ptr<HermiteResamplingAudioSource> m_hermite;

    engine::scratch::ScratchController m_controller;
    engine::scratch::VirtualTurntable m_platter;
    ScratchResampler m_scratchResampler;

    std::atomic<double> m_deckTempoRatio { 1.0 };
    std::atomic<double> m_jogNudgeRatio { 1.0 };
    std::atomic<bool> m_keylockPassthrough { false };
    std::atomic<bool> m_reverse { false };
    std::atomic<double> m_trackSampleRate { 44100.0 };
    std::atomic<double> m_trackLengthSeconds { 0.0 };
    std::atomic<double> m_scratchDisplaySec { 0.0 };
    std::atomic<double> m_audioScratchReadPositionSamples { 0.0 };

    double m_outputSampleRate = 44100.0;
    int m_blockSize = 512;
    std::atomic<bool> m_useScratchScaler { false };
    bool m_prevScratchPath = false;

    std::atomic<int> m_crossfadeRemaining { 0 };
    static constexpr int kCrossfadeSamples = 384;

    std::atomic<bool> m_loopActive { false };
    std::atomic<double> m_loopInSample { 0.0 };
    std::atomic<double> m_loopOutSample { 0.0 };
    std::atomic<std::uint64_t> m_loopCommandGeneration { 0 };
    std::uint64_t m_appliedLoopCommandGeneration = 0;

    std::atomic<double> m_startPositionSeconds { 0.0 };
    std::atomic<double> m_startSampleRate { 44100.0 };
    std::atomic<double> m_startLengthSeconds { 0.0 };
    std::atomic<std::uint64_t> m_startCommandGeneration { 0 };
    std::uint64_t m_appliedStartCommandGeneration = 0;

    std::atomic<double> m_readerSyncPositionSeconds { 0.0 };
    std::atomic<double> m_readerSyncSampleRate { 44100.0 };
    std::atomic<std::uint64_t> m_readerSyncGeneration { 0 };
    std::uint64_t m_appliedReaderSyncGeneration = 0;

    std::atomic<double> m_handoffPositionSeconds { 0.0 };
    std::atomic<double> m_handoffSampleRate { 44100.0 };
    std::atomic<std::uint64_t> m_handoffCommandGeneration { 0 };
    std::uint64_t m_appliedHandoffCommandGeneration = 0;

    std::atomic<bool> m_transportSwapInProgress { false };
    std::atomic<double>* m_audioPlayheadSink = nullptr;
};

} // namespace engine::audio
