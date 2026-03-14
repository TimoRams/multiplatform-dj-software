#pragma once

#include <QObject>
#include <QVector>
#include <QMutex>
#include <QColor>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

#include "TrackSegment.h"

class TrackData : public QObject
{
    Q_OBJECT

    Q_PROPERTY(double bpm           READ getBpm           NOTIFY bpmAnalyzed)
    Q_PROPERTY(bool   isBpmAnalyzed READ isBpmAnalyzed    NOTIFY bpmAnalyzed)
    Q_PROPERTY(qint64 firstBeatSample READ getFirstBeatSample NOTIFY bpmAnalyzed)
    Q_PROPERTY(QString detectedKey  READ getDetectedKey   NOTIFY keyAnalyzed)
    Q_PROPERTY(bool   isKeyAnalyzed READ isKeyAnalyzed    NOTIFY keyAnalyzed)

public:
    // Beat marker: one entry per beat in the track.
    // isDownbeat = true for beat 1 of each bar (every 4th beat, 4/4 time).
    // barNumber  = 1-based bar counter (bar 1 = first detected downbeat).
    struct BeatMarker {
        double positionSec = 0.0;  // exact, micro-snapped timestamp (seconds)
        bool   isDownbeat  = false;
        int    barNumber   = 0;
    };

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

    // Per-frame RGB waveform data for modern DJ-style rendering.
    // rms controls bar height, color encodes frequency balance (low/mid/high).
    struct RgbWaveformFrame {
        QColor color = QColor(255, 255, 255);
        float rms = 0.0f;
        float low = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
    };

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

    // Store BPM + first-beat anchor (rigid grid, backwards compat) AND the full
    // elastic beat-marker array (one BeatMarker per beat, with downbeat flags).
    void setBpmData(double bpm, qint64 firstBeatSample, double sampleRate,
                    std::vector<BeatMarker> beatGrid = {}) {
        {
            QMutexLocker locker(&m_mutex);
            m_bpm             = bpm;
            m_firstBeatSample = firstBeatSample;
            m_sampleRate      = sampleRate;
            m_isBpmAnalyzed   = (bpm > 0.0);
            m_beatGrid        = std::move(beatGrid);
        }
        emit bpmAnalyzed();
    }

    // Returns the elastic beat-marker array.
    // Empty if the analyzer ran before this feature was added.
    std::vector<BeatMarker> getBeatGrid() const {
        QMutexLocker locker(&m_mutex);
        return m_beatGrid;
    }

    // ── Manual beat-grid correction ──────────────────────────────────────────
    // Rebuilds the entire BeatMarker array so that newAnchorSec is beat 1 / bar 1
    // (i = 0, isDownbeat = true).  The grid is extended both backward (i < 0) and
    // forward (i ≥ 0) to cover [0, trackLengthSec], then sorted by positionSec.
    // Emits beatgridChanged() when done.
    void shiftBeatgridToDownbeat(double newAnchorSec, double trackLengthSec) {
        double bpm;
        {
            QMutexLocker locker(&m_mutex);
            bpm = m_bpm;
        }
        if (bpm <= 0.0 || trackLengthSec <= 0.0) return;

        const double beatDur = 60.0 / bpm;
        std::vector<BeatMarker> grid;
        grid.reserve(static_cast<size_t>(trackLengthSec / beatDur) + 4);

        // ── Backward pass (i = -1, -2, …) until we go past the track start ──
        for (int i = -1; ; --i) {
            double pos = newAnchorSec + i * beatDur;
            if (pos < -beatDur * 0.5) break;   // a little past 0 is fine; clamp below
            if (pos < 0.0) pos = 0.0;
            // Rebase: how many beats before the anchor?  Use (-i) as the offset.
            // "beat index" from anchor perspective is i (negative).
            // To assign barNumber we normalise to a non-negative beat counter:
            // beatIdx = i  (negative), barNumber = floor(beatIdx/4).  We correct
            // this after the sort when we renumber from the first marker.
            grid.push_back(BeatMarker{ pos, false, i });
            if (pos <= 0.0) break;
        }

        // ── Forward pass (i = 0, 1, 2, …) ──────────────────────────────────
        for (int i = 0; ; ++i) {
            double pos = newAnchorSec + i * beatDur;
            if (pos > trackLengthSec + beatDur * 0.5) break;
            if (pos > trackLengthSec) pos = trackLengthSec;
            grid.push_back(BeatMarker{ pos, false, i });
            if (pos >= trackLengthSec) break;
        }

        // ── Sort by time, remove duplicates at the boundary ─────────────────
        std::sort(grid.begin(), grid.end(),
            [](const BeatMarker& a, const BeatMarker& b){
                return a.positionSec < b.positionSec;
            });
        grid.erase(std::unique(grid.begin(), grid.end(),
            [](const BeatMarker& a, const BeatMarker& b){
                return std::abs(a.positionSec - b.positionSec) < 0.001;
            }), grid.end());

        // ── Renumber: find the anchor beat (closest to newAnchorSec), assign
        //    sequential beat index from there, set isDownbeat every 4 beats ──
        // First, find the index of the anchor entry.
        int anchorIdx = 0;
        double minDist = std::numeric_limits<double>::max();
        for (int k = 0; k < static_cast<int>(grid.size()); ++k) {
            double d = std::abs(grid[k].positionSec - newAnchorSec);
            if (d < minDist) { minDist = d; anchorIdx = k; }
        }
        for (int k = 0; k < static_cast<int>(grid.size()); ++k) {
            int beatIdx  = k - anchorIdx;          // 0 at anchor, negative before it
            // Modulo that always returns 0..3 even for negative beatIdx:
            int mod4 = ((beatIdx % 4) + 4) % 4;
            grid[k].isDownbeat = (mod4 == 0);
            grid[k].barNumber  = beatIdx / 4 + 1;  // 1-based, may be ≤ 0 before anchor
        }

        {
            QMutexLocker locker(&m_mutex);
            m_firstBeatSample = static_cast<qint64>(std::llround(newAnchorSec * m_sampleRate));
            m_beatGrid = std::move(grid);
        }
        emit beatgridChanged();
    }

    double getBpm() const {
        QMutexLocker locker(&m_mutex);
        return m_bpm;
    }

    // Update only the BPM value (used by manual x2 / ÷2 correction).
    // Does NOT rebuild the beat grid — the caller is responsible for that.
    void setBpm(double bpm) {
        QMutexLocker locker(&m_mutex);
        m_bpm = bpm;
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
        {
            QMutexLocker locker(&m_mutex);
            m_detectedKey  = camelotKey;
            m_isKeyAnalyzed = !camelotKey.isEmpty();
        }
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

    void setSegmentsData(std::vector<TrackSegment> segments) {
        {
            QMutexLocker locker(&m_mutex);
            m_segments = std::move(segments);
        }
        emit segmentsAnalyzed();
    }

    std::vector<TrackSegment> getSegments() const {
        QMutexLocker locker(&m_mutex);
        return m_segments;
    }

    QVector<FrequencyData> getWaveformData() const {
        QMutexLocker locker(&m_mutex);
        return m_data;
    }

    void clearWaveformData() {
        {
            QMutexLocker locker(&m_mutex);
            m_data.clear();
            m_rgbData.clear();
            m_totalExpected = 0;
            m_globalMaxPeak = 0.001f;
        }
        emit dataCleared();
    }

    void clear() {
        {
            QMutexLocker locker(&m_mutex);
            m_data.clear();
            m_rgbData.clear();
            m_totalExpected = 0;
            m_globalMaxPeak = 0.001f;
            m_bpm = 0.0;
            m_firstBeatSample = 0;
            m_isBpmAnalyzed = false;
            m_detectedKey.clear();
            m_isKeyAnalyzed = false;
            m_beatGrid.clear();
            m_segments.clear();
        }
        emit dataCleared();
    }

    void setRgbWaveformData(QVector<RgbWaveformFrame>&& frames) {
        {
            QMutexLocker locker(&m_mutex);
            m_rgbData = std::move(frames);
        }
        emit rgbWaveformUpdated();
    }

    void appendRgbWaveformData(const QVector<RgbWaveformFrame>& frames) {
        if (frames.isEmpty())
            return;
        {
            QMutexLocker locker(&m_mutex);
            m_rgbData.append(frames);
        }
        emit rgbWaveformUpdated();
    }

    QVector<RgbWaveformFrame> getRgbWaveformData() const {
        QMutexLocker locker(&m_mutex);
        return m_rgbData;
    }

    void appendData(const QVector<FrequencyData>& newData) {
        {
            QMutexLocker locker(&m_mutex);
            m_data.append(newData);
        }
        emit dataUpdated();
    }

    // Atomically replace the entire waveform with the final-polish version.
    // Called at the end of Pass 2; the renderer seamlessly switches to it
    // on the next timer tick without any flicker.
    void replaceAllData(QVector<FrequencyData>&& finalData, float finalGlobalMaxPeak) {
        {
            QMutexLocker locker(&m_mutex);
            m_data = std::move(finalData);
            m_globalMaxPeak = finalGlobalMaxPeak;
        }
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
    void rgbWaveformUpdated();
    void bpmAnalyzed();
    void keyAnalyzed();
    void beatgridChanged();  // emitted after a manual grid shift
    void segmentsAnalyzed();

private:
    QVector<FrequencyData> m_data;
    QVector<RgbWaveformFrame> m_rgbData;
    int m_totalExpected;
    float m_globalMaxPeak;

    double  m_bpm;
    qint64  m_firstBeatSample;
    double  m_sampleRate;
    bool    m_isBpmAnalyzed;
    std::vector<BeatMarker> m_beatGrid;  // elastic beat markers (positionSec + downbeat flag)
    std::vector<TrackSegment> m_segments;

    QString m_detectedKey;
    bool    m_isKeyAnalyzed;

    mutable QMutex m_mutex;
};
