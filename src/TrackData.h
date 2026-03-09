#pragma once

#include <QObject>
#include <QVector>
#include <QMutex>

class TrackData : public QObject
{
    Q_OBJECT

    Q_PROPERTY(double bpm           READ getBpm           NOTIFY bpmAnalyzed)
    Q_PROPERTY(bool   isBpmAnalyzed READ isBpmAnalyzed    NOTIFY bpmAnalyzed)
    Q_PROPERTY(qint64 firstBeatSample READ getFirstBeatSample NOTIFY bpmAnalyzed)
    Q_PROPERTY(QString detectedKey  READ getDetectedKey   NOTIFY keyAnalyzed)
    Q_PROPERTY(bool   isKeyAnalyzed READ isKeyAnalyzed    NOTIFY keyAnalyzed)

public:
    // Per-block bin (≈ samplesPerBin samples): envelope per frequency band.
    //
    // Four bands from a parallel filterbank (Rekordbox-style, overlapping slopes):
    //   low     (LP @ 110 Hz, 6 dB/oct)         kick / subbass      → dark blue
    //   lowMid  (BP 150–160 Hz, 12+6 dB/oct)    bass body / warmth  → gold
    //   mid     (BP 180–800 Hz, 12+6 dB/oct)    snare, vocals       → orange
    //   high    (BP@2750 + HP@19kHz)             hi-hat, percussion  → white
    //
    // All values are globally normalised per-band and shaped with pow() contrast.
    // The analyzer operates in two passes:
    //   Pass 1 (raw analysis):  collects raw envelope values, tracks global maxima.
    //   Pass 2 (final output):  normalises against true global max, applies pow()
    //                           contrast + UI gain, atomically replaces data.
    struct WaveformBin {
        float low     = 0.0f;   // sub-bass / kick
        float lowMid  = 0.0f;   // bass body / warmth
        float mid     = 0.0f;   // snare / vocals
        float high    = 0.0f;   // hi-hat / percussion

        // Legacy aliases so existing code compiles during transition.
        // TODO: remove once all renderers are fully migrated to 4-band.
        float lowPeak       = 0.0f;
        float lowRms        = 0.0f;
        float midPeak       = 0.0f;
        float midRms        = 0.0f;
        float highPeak      = 0.0f;
        float highRms       = 0.0f;
        float transientDelta = 0.0f;
        float lowEnv        = 0.0f;
        float midEnv        = 0.0f;
        float highEnv       = 0.0f;
    };

    // Alias kept for renderer compatibility; will be removed in a future refactor.
    using FrequencyData = WaveformBin;

    explicit TrackData(QObject* parent = nullptr)
        : QObject(parent), m_totalExpected(0), m_globalMaxPeak(0.001f),
          m_bpm(0.0), m_firstBeatSample(0), m_sampleRate(44100.0),
          m_isBpmAnalyzed(false), m_isKeyAnalyzed(false) {}

    void setTotalExpected(int total) {
        QMutexLocker locker(&m_mutex);
        m_totalExpected = total;
    }

    int getTotalExpected() const {
        QMutexLocker locker(&m_mutex);
        return m_totalExpected;
    }

    void setGlobalMaxPeak(float maxPeak) {
        QMutexLocker locker(&m_mutex);
        m_globalMaxPeak = maxPeak;
    }

    float getGlobalMaxPeak() const {
        QMutexLocker locker(&m_mutex);
        return m_globalMaxPeak;
    }

    void setBpmData(double bpm, qint64 firstBeatSample, double sampleRate) {
        QMutexLocker locker(&m_mutex);
        m_bpm = bpm;
        m_firstBeatSample = firstBeatSample;
        m_sampleRate = sampleRate;
        m_isBpmAnalyzed = (bpm > 0.0);
        emit bpmAnalyzed();
    }

    double getBpm() const {
        QMutexLocker locker(&m_mutex);
        return m_bpm;
    }

    qint64 getFirstBeatSample() const {
        QMutexLocker locker(&m_mutex);
        return m_firstBeatSample;
    }

    double getSampleRate() const {
        QMutexLocker locker(&m_mutex);
        return m_sampleRate;
    }

    bool isBpmAnalyzed() const {
        QMutexLocker locker(&m_mutex);
        return m_isBpmAnalyzed;
    }

    void setKeyData(const QString& camelotKey) {
        QMutexLocker locker(&m_mutex);
        m_detectedKey  = camelotKey;
        m_isKeyAnalyzed = !camelotKey.isEmpty();
        emit keyAnalyzed();
    }

    QString getDetectedKey() const {
        QMutexLocker locker(&m_mutex);
        return m_detectedKey;
    }

    bool isKeyAnalyzed() const {
        QMutexLocker locker(&m_mutex);
        return m_isKeyAnalyzed;
    }

    QVector<FrequencyData> getWaveformData() const {
        QMutexLocker locker(&m_mutex);
        return m_data;
    }

    void clear() {
        QMutexLocker locker(&m_mutex);
        m_data.clear();
        m_totalExpected = 0;
        m_globalMaxPeak = 0.001f;
        m_bpm = 0.0;
        m_firstBeatSample = 0;
        m_isBpmAnalyzed = false;
        m_detectedKey.clear();
        m_isKeyAnalyzed = false;
        emit dataCleared();
    }

    void appendData(const QVector<FrequencyData>& newData) {
        QMutexLocker locker(&m_mutex);
        m_data.append(newData);
        emit dataUpdated();
    }

    // Atomically replace the entire waveform with the final-polish version.
    // Called at the end of Pass 2; the renderer seamlessly switches to it
    // on the next timer tick without any flicker.
    void replaceAllData(QVector<FrequencyData>&& finalData, float finalGlobalMaxPeak) {
        QMutexLocker locker(&m_mutex);
        m_data = std::move(finalData);
        m_globalMaxPeak = finalGlobalMaxPeak;
        emit dataUpdated();
    }

    void reserve(int size) {
        QMutexLocker locker(&m_mutex);
        m_data.reserve(size);
    }

    int size() const {
        QMutexLocker locker(&m_mutex);
        return m_data.size();
    }

signals:
    void dataUpdated();
    void dataCleared();
    void bpmAnalyzed();
    void keyAnalyzed();

private:
    QVector<FrequencyData> m_data;
    int m_totalExpected;
    float m_globalMaxPeak;

    double  m_bpm;
    qint64  m_firstBeatSample;
    double  m_sampleRate;
    bool    m_isBpmAnalyzed;

    QString m_detectedKey;
    bool    m_isKeyAnalyzed;

    mutable QMutex m_mutex;
};
