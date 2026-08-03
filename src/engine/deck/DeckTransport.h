#pragma once

#include "audio/cache/AudioCacheHandle.h"

#include <QElapsedTimer>

#include <atomic>
#include <cstdint>

class DeckAudioGraph;

struct DeckTransportSnapshot {
    bool hasTrack = false;
    bool playing = false;
    bool audioRunning = false;
    bool reverse = false;
    bool slipEnabled = false;
    bool preRollActive = false;
    bool atTrackEnd = false;
    double audiblePositionSeconds = 0.0;
    double backgroundPositionSeconds = 0.0;
    double trackLengthSeconds = 0.0;
    double playbackRate = 1.0;
    double preRollPositionSeconds = 0.0;
    double sourceSampleRate = 44100.0;
    std::uint64_t trackGeneration = 0;
    std::uint64_t stateGeneration = 0;
};

class DeckTransport final {
public:
    struct TrackConfiguration {
        std::uint64_t trackGeneration = 0;
        double sampleRate = 0.0;
        double lengthSeconds = 0.0;
    };

    struct LoopRegion {
        bool active = false;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
    };

    struct ControlUpdate {
        bool positionChanged = false;
        bool audioRunning = false;
        bool enteredPreRoll = false;
        bool leftPreRoll = false;
        bool reachedTrackEnd = false;
    };

    explicit DeckTransport(DeckAudioGraph& audioGraph) noexcept;
    ~DeckTransport() = default;
    DeckTransport(const DeckTransport&) = delete;
    DeckTransport& operator=(const DeckTransport&) = delete;

    bool installPreparedTrack(AudioCacheHandle cacheHandle, const TrackConfiguration& configuration);
    void clearTrack(std::uint64_t invalidThroughGeneration) noexcept;

    bool setPlaying(bool playing) noexcept;
    bool setReverse(bool enabled) noexcept;
    bool setSlipEnabled(bool enabled) noexcept;
    void returnToSlipPosition() noexcept;
    bool setPlaybackRate(double rate) noexcept;
    bool setJogNudgeRatio(double ratio) noexcept;
    bool seekToSeconds(double seconds) noexcept;
    bool seekNormalized(double progress) noexcept;
    void freezeAt(double seconds) noexcept;
    void ensureAudioRunning(bool blockedByScratch = false) noexcept;
    void setPreRollPosition(double seconds) noexcept;
    void beginPreRoll(double seconds) noexcept;
    void cancelPreRoll() noexcept;
    ControlUpdate updateControlState(const LoopRegion& loop, bool scratchActive,
                                     bool cuePreviewActive) noexcept;

    void setLoopRegion(const LoopRegion& loop) noexcept;
    void setPlaybackReadPositionSamples(std::int64_t samplePosition) noexcept;
    void setAudioReverseOverride(bool enabled) noexcept;
    void setKeylockEnabled(bool enabled) noexcept;
    void startAudio() noexcept;
    void startAudioPreservingScratchPosition() noexcept;
    void stopAudio() noexcept;
    void seekAudioToSeconds(double seconds) noexcept;
    void adoptScratchHandoffPosition(double seconds) noexcept;

    [[nodiscard]] DeckTransportSnapshot snapshot() const noexcept;
    [[nodiscard]] double positionSeconds(bool scratchActive = false) const noexcept;
    [[nodiscard]] double visualPositionSeconds(double latencyCompensationSeconds) const noexcept;
    [[nodiscard]] double playheadPositionAtomic() const noexcept;
    [[nodiscard]] bool audioRunning() const noexcept;
    [[nodiscard]] bool hasTrack() const noexcept { return m_hasTrack; }
    [[nodiscard]] double audioPositionSeconds() const noexcept;
    [[nodiscard]] bool playRequested() const noexcept { return m_playRequested; }
    [[nodiscard]] bool reverse() const noexcept { return m_reverse; }
    [[nodiscard]] bool slipEnabled() const noexcept { return m_slipEnabled; }
    [[nodiscard]] bool preRollActive() const noexcept { return m_preRollActive; }
    [[nodiscard]] bool slipDiverted(bool loopActive) const noexcept;
    [[nodiscard]] double trackLengthSeconds() const noexcept { return m_trackLengthSeconds; }
    [[nodiscard]] double sourceSampleRate() const noexcept { return m_sourceSampleRate; }
    [[nodiscard]] double playbackRate() const noexcept { return m_playbackRate * m_jogNudgeRatio; }
    [[nodiscard]] int keylockLatencySamples() const noexcept;
    [[nodiscard]] std::uint64_t trackGeneration() const noexcept { return m_trackGeneration; }

    void publishScratchPosition(double seconds) noexcept;
    // Control-thread mirror of the audio-owned scratch cursor. Does not write
    // back to the atomic sink, so it cannot race the callback with a stale value.
    void adoptScratchRenderedPosition(double seconds) noexcept;
    void setHeldPosition(double seconds) noexcept;
    [[nodiscard]] double heldPosition() const noexcept { return m_heldPositionSeconds; }
    [[nodiscard]] std::atomic<double>& audioPlayheadSink() noexcept { return m_audioPlayhead; }
    void armVisualSeekSettle() noexcept;
    void setVisualAnchor(double seconds, bool valid) noexcept;

private:
    void publishSnapshot() noexcept;
    void setSnapAnchor(double seconds, bool valid) noexcept;
    void reconcileVisualAnchor(double authoritativePositionSeconds) noexcept;
    void startPreRoll(double seconds) noexcept;

    DeckAudioGraph& m_audioGraph;
    bool m_hasTrack = false;
    bool m_playRequested = false;
    bool m_reverse = false;
    bool m_slipEnabled = false;
    bool m_preRollActive = false;
    bool m_atTrackEnd = false;
    double m_audiblePositionSeconds = 0.0;
    double m_backgroundPositionSeconds = 0.0;
    double m_heldPositionSeconds = 0.0;
    double m_trackLengthSeconds = 0.0;
    double m_sourceSampleRate = 44100.0;
    double m_playbackRate = 1.0;
    double m_jogNudgeRatio = 1.0;
    double m_preRollStartSeconds = 0.0;
    double m_snapPositionSeconds = 0.0;
    double m_snapPlaybackRate = 1.0;
    bool m_snapValid = false;
    std::uint64_t m_trackGeneration = 0;
    std::uint64_t m_stateGeneration = 0;
    QElapsedTimer m_snapClock;
    QElapsedTimer m_preRollClock;
    QElapsedTimer m_visualSeekSettleClock;
    std::atomic<double> m_audioPlayhead {0.0};

    std::atomic<std::uint64_t> m_publishedSequence {0};
    std::atomic<std::uint64_t> m_publishedTrackGeneration {0};
    std::atomic<std::uint64_t> m_publishedStateGeneration {0};
    std::atomic<std::uint32_t> m_publishedFlags {0};
    std::atomic<double> m_publishedAudible {0.0};
    std::atomic<double> m_publishedBackground {0.0};
    std::atomic<double> m_publishedLength {0.0};
    std::atomic<double> m_publishedRate {1.0};
    std::atomic<double> m_publishedPreRoll {0.0};
    std::atomic<double> m_publishedSourceRate {44100.0};
};
