#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QElapsedTimer>
#include <atomic>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "TrackData.h"
#include "WaveformAnalyzer.h"

class CoverArtProvider;

class DjEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float progress READ getProgress NOTIFY progressChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(TrackData* trackData READ getTrackData CONSTANT)

    Q_PROPERTY(QString trackTitle   READ trackTitle   NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackArtist  READ trackArtist  NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackAlbum   READ trackAlbum   NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackKey     READ trackKey     NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackDuration READ trackDuration NOTIFY trackMetadataChanged)
    Q_PROPERTY(bool    hasTrack     READ hasTrack     NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString coverArtUrl  READ coverArtUrl  NOTIFY trackMetadataChanged)
    Q_PROPERTY(bool    hasCoverArt  READ hasCoverArt  NOTIFY trackMetadataChanged)

public:
    explicit DjEngine(QObject* parent = nullptr);
    ~DjEngine() override;

    float getProgress() const;
    float getDuration() const;
    float getPosition() const;
    // Latency-compensated position in seconds, used by the waveform renderer.
    float getVisualPosition() const;
    bool isPlaying() const;
    TrackData* getTrackData() const;

    QString trackTitle()    const { return m_trackTitle; }
    QString trackArtist()   const { return m_trackArtist; }
    QString trackAlbum()    const { return m_trackAlbum; }
    QString trackKey()      const { return m_trackKey; }
    QString trackDuration() const { return m_trackDuration; }
    bool    hasTrack()      const { return m_hasTrack; }
    QString coverArtUrl()   const { return m_coverArtUrl; }
    bool    hasCoverArt()   const { return m_hasCoverArt; }

    void setCoverArtProvider(CoverArtProvider* provider, const QString& deckId);

public slots:
    void loadTrack(const QString& rawPath);
    void togglePlay();
    void setPosition(float progress);

signals:
    void progressChanged();
    void playingChanged();
    void trackLoaded();
    void trackMetadataChanged();

private slots:
    void onTimer();

private:
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transportSource;
    juce::AudioSourcePlayer sourcePlayer;

    QTimer timer;

    TrackData* m_trackData;
    WaveformAnalyzer* m_analyzer;

    QString m_trackTitle;
    QString m_trackArtist;
    QString m_trackAlbum;
    QString m_trackKey;
    QString m_trackDuration;
    bool    m_hasTrack = false;

    CoverArtProvider* m_coverProvider = nullptr;
    QString m_deckId;
    QString m_coverArtUrl;
    bool    m_hasCoverArt = false;

    // m_latencySeconds is computed once after device init (output latency + buffer size).
    // m_snapPosition + m_snapClock enable sub-frame interpolation in getVisualPosition().
    float          m_latencySeconds  = 0.0f;
    double         m_snapPosition    = 0.0;
    QElapsedTimer  m_snapClock;
    bool           m_snapValid       = false;
};
