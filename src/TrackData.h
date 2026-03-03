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
    // Per time-window bin: two values per frequency band.
    //   rms  = envelope-smoothed energy (forms the waveform body)
    //   peak = absolute maximum in the bin (captures transients)
    // Three bands from the Linkwitz-Riley crossover:
    //   low  (<200 Hz)    bass
    //   mid  (200-4k Hz)  mids
    //   high (>4k Hz)     highs
    // fullPeak / fullRms cover the full-band signal.
    struct WaveformBin {
        float fullPeak = 0.0f;
        float fullRms  = 0.0f;
        float lowPeak  = 0.0f;
        float lowRms   = 0.0f;
        float midPeak  = 0.0f;
        float midRms   = 0.0f;
        float highPeak = 0.0f;
        float highRms  = 0.0f;
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
