#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

class LibraryPreviewPlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY currentPathChanged)
    Q_PROPERTY(double durationSec READ durationSec NOTIFY durationChanged)
    Q_PROPERTY(double positionSec READ positionSec NOTIFY positionChanged)
    Q_PROPERTY(double progress READ progress NOTIFY positionChanged)

public:
    explicit LibraryPreviewPlayer(QObject* parent = nullptr);
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
                        int numSamples);

signals:
    void playingChanged();
    void currentPathChanged();
    void durationChanged();
    void positionChanged();

private:
    void beginPreviewLocked(const QString& filePath);
    void finishPreviewLocked();
    void publishTransportStateLocked();
    void startPositionTimer();
    void stopPositionTimer();
    void pollPosition();
    double previewStartSeconds(double trackLengthSec) const;

    juce::AudioFormatManager m_formatManager;
    std::shared_ptr<juce::AudioFormatReader> m_reader;
    std::unique_ptr<juce::AudioFormatReaderSource> m_readerSource;
    juce::AudioTransportSource m_transport;

    mutable std::mutex m_mutex;
    QTimer m_positionTimer;
    QString m_currentPath;
    std::atomic<bool> m_playing { false };
    std::atomic<double> m_durationSec { 0.0 };
    std::atomic<double> m_positionSec { 0.0 };
    double m_sampleRate = 44100.0;
    int m_blockSize = 512;
    bool m_prepared = false;

    static constexpr double kPreviewGain = 0.72;
};
