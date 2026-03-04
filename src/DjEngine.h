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
    Q_PROPERTY(double tempoPercent READ getTempoPercent WRITE setTempoPercent NOTIFY tempoChanged)
    Q_PROPERTY(TrackData* trackData READ getTrackData CONSTANT)

    Q_PROPERTY(QString trackTitle   READ trackTitle   NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackArtist  READ trackArtist  NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackAlbum   READ trackAlbum   NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackKey     READ trackKey     NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString trackDuration READ trackDuration NOTIFY trackMetadataChanged)
    Q_PROPERTY(bool    hasTrack     READ hasTrack     NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString coverArtUrl  READ coverArtUrl  NOTIFY trackMetadataChanged)
    Q_PROPERTY(bool    hasCoverArt  READ hasCoverArt  NOTIFY trackMetadataChanged)

    // Mixer Properties
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(double trim READ trim WRITE setTrim NOTIFY trimChanged)
    Q_PROPERTY(double eqHigh READ eqHigh WRITE setEqHigh NOTIFY eqHighChanged)
    Q_PROPERTY(double eqMid READ eqMid WRITE setEqMid NOTIFY eqMidChanged)
    Q_PROPERTY(double eqLow READ eqLow WRITE setEqLow NOTIFY eqLowChanged)
    Q_PROPERTY(double filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool cueEnabled READ cueEnabled WRITE setCueEnabled NOTIFY cueEnabledChanged)

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
    double  getTempoPercent() const { return m_tempoPercent; }

    // Mixer Getters
    double volume() const { return m_volume; }
    double trim() const { return m_trim; }
    double eqHigh() const { return m_eqHigh; }
    double eqMid() const { return m_eqMid; }
    double eqLow() const { return m_eqLow; }
    double filter() const { return m_filter; }
    bool cueEnabled() const { return m_cueEnabled; }

    void setCoverArtProvider(CoverArtProvider* provider, const QString& deckId);

public slots:
    void loadTrack(const QString& rawPath);
    void togglePlay();
    void setPosition(float progress);
    void setTempoPercent(double percent);
    
    // Mixer Setters
    void setVolume(double value);
    void setTrim(double value);
    void setEqHigh(double value);
    void setEqMid(double value);
    void setEqLow(double value);
    void setFilter(double value);
    void setCueEnabled(bool value);

signals:
    void progressChanged();
    void playingChanged();
    void tempoChanged();
    void trackLoaded();
    void trackMetadataChanged();
    
    // Mixer Signals
    void volumeChanged();
    void trimChanged();
    void eqHighChanged();
    void eqMidChanged();
    void eqLowChanged();
    void filterChanged();
    void cueEnabledChanged();

private slots:
    void onTimer();

private:
    class MixerDspSource;

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transportSource;
    std::unique_ptr<juce::ResamplingAudioSource> resamplingSource;
    std::unique_ptr<MixerDspSource> mixerSource;
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

    // Tempo control: ±8% range
    double m_tempoPercent = 0.0;

    // Mixer state
    double m_volume = 0.8;
    double m_trim = 1.0;
    double m_eqHigh = 0.0;
    double m_eqMid = 0.0;
    double m_eqLow = 0.0;
    double m_filter = 0.0;
    bool m_cueEnabled = false;

    // Updates the JUCE transport source gain based on volume and trim
    void updateGain();

    // m_latencySeconds is computed once after device init (output latency + buffer size).
    // m_snapPosition + m_snapClock enable sub-frame interpolation in getVisualPosition().
    float          m_latencySeconds  = 0.0f;
    double         m_snapPosition    = 0.0;
    QElapsedTimer  m_snapClock;
    bool           m_snapValid       = false;
};
