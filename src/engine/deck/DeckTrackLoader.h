#pragma once

#include "TrackData.h"
#include "WaveformCache.h"
#include "audio/cache/AudioCacheHandle.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

enum class TrackLoadError {
    None,
    EmptyPath,
    FileNotFound,
    UnsupportedFormat,
    DecoderCreationFailed,
    Cancelled,
    Superseded,
    Shutdown
};

enum class TrackLoadState {
    Idle,
    Queued,
    Loading,
    Preparing,
    Ready,
    Failed,
    CancelRequested,
    Cancelled,
    ShuttingDown
};

struct TrackMetadataSnapshot {
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString comment;
    QString key;
    QString year;
    QString trackNumber;
    double tagBpm = 0.0;
    double durationSec = 0.0;
    double sampleRate = 0.0;
    std::int64_t lengthInSamples = 0;
    unsigned int channelCount = 0;
    std::int64_t fileSize = 0;
};

struct TrackLoadResult {
    std::uint64_t generation = 0;
    QString canonicalPath;
    TrackMetadataSnapshot metadata;
    AudioCacheHandle cacheHandle;
    WaveformCache::Payload waveformCache;
    QVector<TrackData::RgbWaveformFrame> instantOverview;
    int instantOverviewExpected = 0;
    bool waveformCacheLoaded = false;
    QByteArray coverBytes;
    QImage coverImage;
    double autoCueSec = -1.0;
    TrackLoadError error = TrackLoadError::None;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept { return error == TrackLoadError::None; }
};

class DeckTrackLoader
{
public:
    using CompletionCallback = std::function<void(TrackLoadResult)>;

    DeckTrackLoader(AudioPageCache& audioPageCache, int waveformPointsPerSecond);
    ~DeckTrackLoader();

    DeckTrackLoader(const DeckTrackLoader&) = delete;
    DeckTrackLoader& operator=(const DeckTrackLoader&) = delete;

    std::uint64_t loadTrack(QString path, CompletionCallback completion);
    void requestCancel() noexcept;
    void shutdownAndJoin() noexcept;

    [[nodiscard]] std::uint64_t currentGeneration() const noexcept;
    [[nodiscard]] TrackLoadState state() const noexcept;

private:
    struct Request {
        QString path;
        std::uint64_t generation = 0;
        CompletionCallback completion;
    };

    void workerLoop();
    TrackLoadResult prepare(const Request& request);
    [[nodiscard]] bool isCurrent(std::uint64_t generation) const noexcept;
    void publishState(std::uint64_t generation, TrackLoadState state) noexcept;

    const int m_waveformPointsPerSecond;
    AudioPageCache& m_audioPageCache;
    juce::AudioFormatManager m_formatManager;
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<TrackLoadState> m_state{TrackLoadState::Idle};
    std::atomic<bool> m_shuttingDown{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::optional<Request> m_pending;
    std::thread m_worker;
};
