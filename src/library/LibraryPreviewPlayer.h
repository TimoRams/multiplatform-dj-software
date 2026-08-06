#pragma once

#include <QObject>
#include <QString>
#include "app/ControlClock.h"
#include "audio/AudioEngine.h"
#include "audio/cache/AudioCacheHandle.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <juce_audio_basics/juce_audio_basics.h>

class AudioPageCache;
class CachedPlaybackAudioSource;

class LibraryPreviewPlayer : public QObject, public IAuxAudioEndpoint
{
    Q_OBJECT
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY currentPathChanged)
    Q_PROPERTY(double durationSec READ durationSec NOTIFY durationChanged)
    Q_PROPERTY(double positionSec READ positionSec NOTIFY positionChanged)
    Q_PROPERTY(double progress READ progress NOTIFY positionChanged)

public:
    explicit LibraryPreviewPlayer(ControlClock& controlClock,
                                  AudioPageCache& cache,
                                  QObject* parent = nullptr);
    ~LibraryPreviewPlayer() override;

    [[nodiscard]] bool isPlaying() const { return m_playing.load(std::memory_order_relaxed); }
    [[nodiscard]] QString currentPath() const;
    [[nodiscard]] double durationSec() const { return m_durationSec.load(std::memory_order_relaxed); }
    [[nodiscard]] double positionSec() const { return m_positionSec.load(std::memory_order_relaxed); }
    [[nodiscard]] double progress() const;

    Q_INVOKABLE void preview(const QString& filePath);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void togglePreview(const QString& filePath);
    Q_INVOKABLE void seekSeconds(double positionSec);
    Q_INVOKABLE void seekProgress(double progress);

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate);
    void releaseResources();
    void mixIntoOutputs(juce::AudioBuffer<float>& masterBuf,
                        juce::AudioBuffer<float>& scratch,
                        int startSample,
                        int numSamples) noexcept;
    void prepareAuxAudio(int maximumBlockSize, double sampleRate) override;
    void releaseAuxAudio() override;
    void mixAuxAudio(juce::AudioBuffer<float>& masterBuffer,
                     juce::AudioBuffer<float>& scratchBuffer,
                     int numberOfSamples) noexcept override;

signals:
    void playingChanged();
    void currentPathChanged();
    void durationChanged();
    void positionChanged();

private:
    void finishPreviewLocked();
    void waitForAudioReaders() const noexcept;
    void startPositionTimer();
    void stopPositionTimer();
    void pollPosition();
    double previewStartSeconds(double trackLengthSec) const;

    AudioPageCache& m_cache;
    AudioCacheHandle m_cacheHandle;
    std::unique_ptr<CachedPlaybackAudioSource> m_readerSource;
    std::atomic<CachedPlaybackAudioSource*> m_audioReader { nullptr };
    mutable std::atomic<std::uint32_t> m_activeAudioReaders { 0 };
    std::atomic<std::uint64_t> m_loadGeneration { 0 };

    mutable std::mutex m_mutex;
    ControlClock::Registration m_clockRegistration;
    bool m_positionPollingEnabled = false;
    QString m_currentPath;
    std::atomic<bool> m_playing { false };
    std::atomic<double> m_durationSec { 0.0 };
    std::atomic<double> m_positionSec { 0.0 };
    std::atomic<bool> m_prepared { false };

    static constexpr double kPreviewGain = 0.72;
};
