#pragma once

#include <limits>
#include "audio/AudioRouting.h"
#include "audio/internal/ScratchResampler.h"
#include "deck/scratch/ScratchController.h"
#include "deck/scratch/VirtualTurntable.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

class HermiteResamplingAudioSource;
class CachedPlaybackAudioSource;

namespace engine::audio {

enum class RenderMode : std::uint8_t {
    Direct,
    Keylock,
    Scratch
};

enum class ScratchReleasePhase : std::uint8_t {
    Idle,
    ReleasePending,
    CoastToDeck,
    CoastToStop,
    HandoffPending,
    TailSuppression
};

struct ScratchReleaseSnapshot {
    std::uint64_t generation = 0;
    ScratchReleasePhase phase = ScratchReleasePhase::Idle;
    engine::scratch::ScratchReleaseDisposition disposition =
        engine::scratch::ScratchReleaseDisposition::HandoffNow;
    double finalCursorSeconds = 0.0;
};

// Normal playback: proven Hermite varispeed (keylock off) or transport pass-through (keylock on).
// Scratch playback: velocity-based virtual turntable + dedicated scratch resampler.
class RenderModeRouter : public juce::AudioSource {
public:
    explicit RenderModeRouter(juce::AudioSource* inputSource, bool deleteInputWhenDeleted = false);
    ~RenderModeRouter() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    void beginScratch(double anchorSeconds,
                      double trackSampleRate,
                      double trackLengthSeconds,
                      bool wasPlayingBeforeScratch,
                      double normalPlaybackSpeed);
    [[nodiscard]] engine::scratch::ScratchReleaseDisposition endScratch(bool allowInertia);
    [[nodiscard]] std::uint64_t requestScratchRelease(double normalizedReleaseSpeed,
                                                      bool allowInertia,
                                                      bool playbackIntent) noexcept;
    void submitScratchReleaseSpeed(double normalizedReleaseSpeed) noexcept;
    void engageScratchDuringInertia() noexcept;
    void addTargetDeltaSeconds(double deltaSeconds, double trackSampleRate) noexcept;
    // measuredRate: hand speed in playback rates as measured by the input, or
    // a non-finite value when the input cannot measure it better than the
    // per-event quotient. See ScratchController::submitHandDelta.
    void submitHandDeltaSeconds(double deltaSeconds,
                                double dtSeconds,
                                double measuredRate
                                    = std::numeric_limits<double>::quiet_NaN()) noexcept;
    void submitReleaseDeltaSeconds(double deltaSeconds, double dtSeconds) noexcept;
    void syncScratchReadPosition(double displaySec, double trackSampleRate) noexcept;
    void publishScratchDisplay(double displaySec) noexcept;

    void configureTrack(double trackSampleRate, double trackLengthSeconds) noexcept;
    void syncReadPositionSeconds(double positionSeconds, double trackSampleRate) noexcept;
    void prepareNormalPlaybackHandoff(double positionSeconds, double trackSampleRate) noexcept;
    void prepareNormalPlaybackHandoffFromScratchCursor(double trackSampleRate) noexcept;
    void exitScratchMode(double positionSeconds, double trackSampleRate) noexcept;

    void setDeckTempoRatio(double ratio) noexcept;
    void setJogNudgeRatio(double ratio) noexcept;
    void setNormalPlaybackEnabled(bool enabled) noexcept;
    void setKeylockEnabled(bool enabled) noexcept {
        m_keylockPassthrough.store(enabled, std::memory_order_relaxed);
    }
    void setLoopRangeSeconds(double loopInSec, double loopOutSec, bool active,
                             double trackSampleRate) noexcept;
    void setReverse(bool reverse) noexcept;

    [[nodiscard]] bool isScratching() const noexcept;
    [[nodiscard]] RenderMode activeRenderMode() const noexcept
    { return m_activeRenderMode.load(std::memory_order_acquire); }
    [[nodiscard]] bool isInertiaActive() const noexcept;
    [[nodiscard]] double scratchRate() const noexcept;
    [[nodiscard]] double readPositionSeconds(double trackSampleRate) const noexcept;
    [[nodiscard]] double displayPositionSeconds() const noexcept;
    [[nodiscard]] bool normalPlaybackHandoffPending() const noexcept;
    [[nodiscard]] ScratchReleaseSnapshot scratchReleaseSnapshot() const noexcept;
    [[nodiscard]] bool scratchReleaseComplete(std::uint64_t generation) const noexcept;

    void snapHermiteToDeckTempo() noexcept;

    [[nodiscard]] engine::scratch::VirtualTurntable& platter() noexcept { return m_platter; }
    [[nodiscard]] const engine::scratch::VirtualTurntable& platter() const noexcept { return m_platter; }

    void armScalerCrossfade() noexcept { m_crossfadeRemaining.store(kCrossfadeSamples, std::memory_order_relaxed); }

    void setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle) noexcept;
    void setPlaybackSource(CachedPlaybackAudioSource* source) noexcept {
        m_playbackSource.store(source, std::memory_order_release);
    }
    [[nodiscard]] ScratchCacheStats scratchCacheStats() const noexcept { return m_scratchResampler.cacheStats(); }

    // Audio thread publishes scratch playhead here (seconds) for lock-free UI reads.
    void setAudioPlayheadSink(std::atomic<double>* sink) noexcept { m_audioPlayheadSink = sink; }

    // Blocks audio output while DjEngine swaps transport reader sources.
    void beginTransportSwap() noexcept;
    void endTransportSwap() noexcept {
        m_transportSwapInProgress.store(false, std::memory_order_seq_cst);
    }

private:
    void applyDeckTempoToHermite() noexcept;
    [[nodiscard]] double effectiveDeckTempoRatio() const noexcept;
    void consumePendingAudioCommands() noexcept;
    void consumeScratchReleaseCommand() noexcept;
    void finishReleaseDecisionAfterTrackingBlock() noexcept;
    void finishCoastHandoffAfterScratchBlock(double trackSampleRate) noexcept;
    void completeActiveReleaseHandoff(double trackSampleRate) noexcept;
    [[nodiscard]] bool applyReaderHandoff(double positionSeconds,
                                          double trackSampleRate,
                                          bool releaseOwned = false) noexcept;
    void applyNormalPlaybackHandoff(double positionSeconds,
                                    double trackSampleRate,
                                    std::uint64_t generation) noexcept;
    void completeCursorHandoffAfterScratchBlock(double trackSampleRate) noexcept;
    double activePlaybackRate(double trackSampleRate, int bufferSize) noexcept;
    void applyNormalPathCrossfade(const juce::AudioSourceChannelInfo& info) noexcept;
    void captureScratchTail(const juce::AudioSourceChannelInfo& info) noexcept;
    void applyScratchExitTail(const juce::AudioSourceChannelInfo& info) noexcept;
    void captureNormalTail(const juce::AudioSourceChannelInfo& info) noexcept;
    void applyNormalStopTail(const juce::AudioSourceChannelInfo& info) noexcept;
    void publishScratchCursor(double readPositionSamples, double trackSampleRate) noexcept;
    void publishReleaseSnapshot(std::uint64_t generation,
                                ScratchReleasePhase phase,
                                engine::scratch::ScratchReleaseDisposition disposition,
                                double finalCursorSeconds) noexcept;
    [[nodiscard]] bool isScratchPathActive() const noexcept;
    [[nodiscard]] double signedDeckTempoRatio() const noexcept;

    juce::OptionalScopedPointer<juce::AudioSource> m_transport;
    juce::PositionableAudioSource* m_positionableTransportSource = nullptr;
    std::atomic<CachedPlaybackAudioSource*> m_playbackSource { nullptr };
    std::unique_ptr<HermiteResamplingAudioSource> m_hermite;

    engine::scratch::ScratchController m_controller;
    engine::scratch::VirtualTurntable m_platter;
    ScratchResampler m_scratchResampler;

    std::atomic<double> m_deckTempoRatio { 1.0 };
    std::atomic<double> m_jogNudgeRatio { 1.0 };
    std::atomic<bool> m_normalPlaybackEnabled { false };
    std::atomic<std::uint64_t> m_transportIntentGeneration { 0 };
    std::uint64_t m_appliedTransportIntentGeneration = 0;
    std::atomic<bool> m_keylockPassthrough { false };
    std::atomic<RenderMode> m_activeRenderMode { RenderMode::Direct };
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
    static constexpr int kCrossfadeSamples =
        AudioRoutingConstants::kScratchCrossfadeMaxSamples;

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
    std::atomic<bool> m_handoffFromScratchCursor { false };
    std::atomic<std::uint64_t> m_handoffCommandGeneration { 0 };
    std::atomic<std::uint64_t> m_cancelledHandoffGeneration { 0 };
    std::uint64_t m_appliedHandoffCommandGeneration = 0;
    std::atomic<std::uint64_t> m_completedHandoffCommandGeneration { 0 };
    bool m_cursorHandoffPending = false;
    std::uint64_t m_cursorHandoffGeneration = 0;
    double m_cursorHandoffSampleRate = 44100.0;

    // Control-thread release publication. The odd/even sequence makes the
    // multi-field command coherent without making the audio callback spin.
    std::atomic<std::uint64_t> m_releaseCommandSequence { 0 };
    std::atomic<std::uint64_t> m_releaseCommandGeneration { 0 };
    std::atomic<double> m_releaseCommandSpeed { 0.0 };
    std::atomic<double> m_releaseCommandDeckRate { 1.0 };
    std::atomic<double> m_releaseCommandSampleRate { 44100.0 };
    std::atomic<bool> m_releaseCommandPlaybackIntent { false };
    std::atomic<bool> m_releaseCommandAllowInertia { true };
    std::atomic<bool> m_releaseCommandKeylock { false };
    std::atomic<bool> m_releaseCommandReverse { false };
    std::atomic<bool> m_releaseCommandLoop { false };
    std::atomic<std::uint64_t> m_releaseGenerationCounter { 0 };
    std::atomic<std::uint64_t> m_requestedReleaseGeneration { 0 };
    std::atomic<std::uint64_t> m_cancelledReleaseGeneration { 0 };
    std::atomic<std::uint64_t> m_completedReleaseGeneration { 0 };
    std::atomic<double> m_latestReleaseSpeed { 0.0 };

    struct AudioReleaseCommand {
        std::uint64_t generation = 0;
        double speed = 0.0;
        double deckRate = 1.0;
        double sampleRate = 44100.0;
        bool playbackIntent = false;
        bool allowInertia = true;
        bool keylock = false;
        bool reverse = false;
        bool loop = false;
    };
    AudioReleaseCommand m_audioReleaseCommand;
    std::uint64_t m_appliedReleaseGeneration = 0;
    ScratchReleasePhase m_audioReleasePhase = ScratchReleasePhase::Idle;
    engine::scratch::ScratchReleaseDisposition m_audioReleaseDisposition =
        engine::scratch::ScratchReleaseDisposition::HandoffNow;

    // Audio-thread acknowledgement snapshot, also protected by an odd/even
    // sequence so the facade never observes a new generation with an old cursor.
    std::atomic<std::uint64_t> m_releaseAckSequence { 0 };
    std::atomic<std::uint64_t> m_releaseAckGeneration { 0 };
    std::atomic<std::uint8_t> m_releaseAckPhase {
        static_cast<std::uint8_t>(ScratchReleasePhase::Idle) };
    std::atomic<std::uint8_t> m_releaseAckDisposition {
        static_cast<std::uint8_t>(engine::scratch::ScratchReleaseDisposition::HandoffNow) };
    std::atomic<double> m_releaseAckCursorSeconds { 0.0 };

    std::array<float, 2> m_lastScratchOutput { 0.0f, 0.0f };
    bool m_lastScratchOutputValid = false;
    bool m_scratchExitTailPending = false;
    std::uint64_t m_tailReleaseGeneration = 0;
    std::array<float, 2> m_lastNormalOutput { 0.0f, 0.0f };
    bool m_lastNormalOutputValid = false;
    bool m_normalPlaybackWasEnabled = false;

    std::atomic<bool> m_transportSwapInProgress { false };
    std::atomic<unsigned int> m_audioCallbacksActive { 0 };
    std::atomic<double>* m_audioPlayheadSink = nullptr;
};

} // namespace engine::audio
