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
    Q_PROPERTY(double sampleRate READ getSampleRate NOTIFY bpmAnalyzed)
    Q_PROPERTY(QString detectedKey  READ getDetectedKey   NOTIFY keyAnalyzed)
    Q_PROPERTY(bool   isKeyAnalyzed READ isKeyAnalyzed    NOTIFY keyAnalyzed)

public:
    // Beat marker: one entry per beat in the track.
    // isDownbeat = true for beat 1 of each bar (every 4th beat, 4/4 time).
    // barNumber  = 1-based bar counter (bar 1 = first detected downbeat).
    // beatInBar  = 1..4, beat position within the current bar.
    struct BeatMarker {
        double positionSec = 0.0;  // exact, micro-snapped timestamp (seconds)
        bool   isDownbeat  = false;
        int    barNumber   = 0;
        int    beatInBar   = 1;    // 1=downbeat, 2, 3, 4
    };

    // Per-block bin (≈ samplesPerBin samples): envelope per frequency band.
    //
    // Four bands from a parallel filterbank (DJ-style, overlapping slopes):
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
        float rms    = 0.0f;
        float low    = 0.0f;
        float lowMid = 0.0f;
        float mid    = 0.0f;
        float high   = 0.0f;
    };

    // High-resolution signed min/max peak pair for oscillation rendering.
    // Stored at PEAK_POINTS_PER_SECOND resolution (4× the analysis rate).
    // Values quantized to [-127, +127] representing normalized amplitude [-1, +1].
    // Used by the renderer to show actual audio oscillations at high zoom.
    struct PeakFrame {
        qint8 minSample = 0;
        qint8 maxSample = 0;
    };
    // 8× the analysis rate (1200 pps). At 48 kHz, 1 bin covers 5 samples.
    // At max zoom (40 px/pt) each bin spans 40/8 = 5 px → sub-pixel with Catmull-Rom.
    static constexpr int PEAK_POINTS_PER_SECOND = 9600;

    // Fixed bin count for the progressive downsampled overview (built incrementally
    // during analysis). Renderers stay O(kProgressiveBins) instead of O(track_length).
    static constexpr int kProgressiveBins = 2048;

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

        // Extend backward into pre-roll (up to 8 bars, capped at 32 s before beat 1).
        // Positions below -preRollSec are discarded; no clamping to 0.
        const double preRollSec = std::min(8.0 * 4.0 * beatDur, 32.0);
        // ── Backward pass (i = -1, -2, …) until we exceed the pre-roll zone ──
        for (int i = -1; ; --i) {
            double pos = newAnchorSec + i * beatDur;
            if (pos < -(preRollSec + beatDur * 0.5)) break;
            if (pos < -preRollSec) pos = -preRollSec;
            grid.push_back(BeatMarker{ pos, false, i });
            if (pos <= -preRollSec) break;
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
            grid[k].beatInBar  = mod4 + 1;          // 1=downbeat, 2, 3, 4
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
            m_peakMip.clear();
            m_progressiveOvr.clear();
            m_progressiveLastFrame = 0;
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
            m_overviewRgb.clear();
            m_peakMip.clear();
            m_progressiveOvr.clear();
            m_progressiveLastFrame = 0;
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

    void setPeakMipData(QVector<PeakFrame>&& data) {
        {
            QMutexLocker locker(&m_mutex);
            m_peakMip = std::move(data);
        }
        emit peakMipUpdated();
    }

    int getPeakMipSize() const {
        QMutexLocker locker(&m_mutex);
        return m_peakMip.size();
    }

    QVector<PeakFrame> getPeakMipSlice(int startIdx, int endIdx, int* outStartIdx = nullptr) const {
        QMutexLocker locker(&m_mutex);
        if (outStartIdx)
            *outStartIdx = 0;
        if (m_peakMip.isEmpty())
            return {};
        const int lo = std::max(0, startIdx);
        const int hi = std::min(endIdx, static_cast<int>(m_peakMip.size()));
        if (lo >= hi)
            return {};
        QVector<PeakFrame> slice;
        slice.reserve(hi - lo);
        for (int i = lo; i < hi; ++i)
            slice.push_back(m_peakMip[i]);
        if (outStartIdx)
            *outStartIdx = lo;
        return slice;
    }

    QVector<PeakFrame> getPeakMipData() const {
        QMutexLocker locker(&m_mutex);
        return m_peakMip;
    }

    void setRgbWaveformData(QVector<RgbWaveformFrame>&& frames) {
        {
            QMutexLocker locker(&m_mutex);
            m_rgbData = std::move(frames);
            // Clear progressive overview — this is an atomic full-data replacement
            // (end of Pass 2 or cache load); setOverviewRgbData() follows shortly.
            m_progressiveOvr.clear();
            m_progressiveLastFrame = 0;
        }
        emit rgbWaveformUpdated();
    }

    // Pre-downsampled overview (≤4096 bins) computed off the main thread.
    // When available, RgbWaveformItem uses this instead of the full RGB data
    // so paint() stays O(4096) regardless of track length.
    void setOverviewRgbData(QVector<RgbWaveformFrame>&& data) {
        {
            QMutexLocker locker(&m_mutex);
            m_overviewRgb = std::move(data);
            // Pre-computed overview supersedes the progressive one — release memory.
            m_progressiveOvr.clear();
            m_progressiveLastFrame = 0;
        }
        emit overviewRgbUpdated();
    }

    QVector<RgbWaveformFrame> getOverviewRgbData() const {
        QMutexLocker locker(&m_mutex);
        return m_overviewRgb;
    }

    // Pre-allocate the full RGB array with zero frames so the renderer can
    // index any position immediately (unanalyzed bins render as silence).
    void preallocateRgbWaveform(int numBins) {
        QMutexLocker locker(&m_mutex);
        m_rgbData.fill(RgbWaveformFrame{}, numBins);
    }

    // Write analyzed frames at a specific bin offset.
    // Called by the analyzer's priority and fill passes.
    void writeRgbWaveformRange(int fromBin, const QVector<RgbWaveformFrame>& data) {
        if (data.isEmpty()) return;
        {
            QMutexLocker locker(&m_mutex);
            const int avail = m_rgbData.size() - fromBin;
            if (avail <= 0) return;
            const int n = std::min(static_cast<int>(data.size()), avail);
            for (int i = 0; i < n; ++i)
                m_rgbData[fromBin + i] = data[i];
            _updateProgressiveOvr(fromBin, fromBin + n);
        }
        emit rgbWaveformUpdated();
    }

    void appendRgbWaveformData(const QVector<RgbWaveformFrame>& frames) {
        if (frames.isEmpty())
            return;
        {
            QMutexLocker locker(&m_mutex);
            const int oldSize = m_rgbData.size();
            m_rgbData.append(frames);
            _updateProgressiveOvr(oldSize, m_rgbData.size());
        }
        emit rgbWaveformUpdated();
    }

    QVector<RgbWaveformFrame> getRgbWaveformData() const {
        QMutexLocker locker(&m_mutex);
        return m_rgbData;
    }

    int getRgbWaveformSize() const {
        QMutexLocker locker(&m_mutex);
        return m_rgbData.size();
    }

    // Progressive kProgressiveBins-bin overview updated incrementally during analysis.
    // Use as fallback when getOverviewRgbData() is empty (analysis not yet complete).
    // outProcessed receives the number of source frames folded into the overview.
    QVector<RgbWaveformFrame> getProgressiveOvrData(int* outProcessed = nullptr) const {
        QMutexLocker locker(&m_mutex);
        if (outProcessed) *outProcessed = m_progressiveLastFrame;
        return m_progressiveOvr;
    }

    QVector<RgbWaveformFrame> getRgbWaveformSlice(int startIndex, int endIndex, int* outStartIndex = nullptr) const {
        QMutexLocker locker(&m_mutex);

        if (outStartIndex)
            *outStartIndex = 0;

        if (m_rgbData.isEmpty())
            return {};

        const int lo = std::max(0, startIndex);
        const int hi = std::min(endIndex, static_cast<int>(m_rgbData.size()));
        if (lo >= hi)
            return {};

        QVector<RgbWaveformFrame> slice;
        slice.reserve(hi - lo);
        for (int i = lo; i < hi; ++i)
            slice.push_back(m_rgbData[i]);

        if (outStartIndex)
            *outStartIndex = lo;

        return slice;
    }

    // Fill variants: write into caller's pre-allocated buffer, reusing its capacity
    // to avoid a new heap allocation every frame. Returns the base index.
    int fillRgbWaveformSlice(QVector<RgbWaveformFrame>& dst, int startIndex, int endIndex) const {
        QMutexLocker locker(&m_mutex);
        dst.clear();
        if (m_rgbData.isEmpty()) return 0;
        const int lo = std::max(0, startIndex);
        const int hi = std::min(endIndex, static_cast<int>(m_rgbData.size()));
        if (lo >= hi) return lo;
        const int n = hi - lo;
        dst.resize(n);
        for (int i = 0; i < n; ++i)
            dst[i] = m_rgbData[lo + i];
        return lo;
    }

    int fillPeakMipSlice(QVector<PeakFrame>& dst, int startIdx, int endIdx) const {
        QMutexLocker locker(&m_mutex);
        dst.clear();
        if (m_peakMip.isEmpty()) return 0;
        const int lo = std::max(0, startIdx);
        const int hi = std::min(endIdx, static_cast<int>(m_peakMip.size()));
        if (lo >= hi) return lo;
        const int n = hi - lo;
        dst.resize(n);
        for (int i = 0; i < n; ++i)
            dst[i] = m_peakMip[lo + i];
        return lo;
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
    void overviewRgbUpdated();
    void peakMipUpdated();
    void bpmAnalyzed();
    void keyAnalyzed();
    void beatgridChanged();  // emitted after a manual grid shift
    void segmentsAnalyzed();

private:
    QVector<FrequencyData> m_data;
    QVector<RgbWaveformFrame> m_rgbData;
    QVector<RgbWaveformFrame> m_overviewRgb;
    QVector<PeakFrame> m_peakMip;
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

    QVector<RgbWaveformFrame> m_progressiveOvr;   // kProgressiveBins bins, max-folded
    int                       m_progressiveLastFrame = 0;

    // Fold m_rgbData[from..to) into m_progressiveOvr using max() per band.
    // Must be called with m_mutex already held.
    void _updateProgressiveOvr(int from, int to) {
        if (m_totalExpected <= 0 || to <= from) return;
        if (m_progressiveOvr.size() != kProgressiveBins) {
            m_progressiveOvr.fill(RgbWaveformFrame{}, kProgressiveBins);
            m_progressiveLastFrame = 0;
        }
        const int dataSize = static_cast<int>(m_rgbData.size());
        const int end = std::min(to, dataSize);
        for (int i = from; i < end; ++i) {
            const int bin = static_cast<int>(
                (static_cast<int64_t>(i) * kProgressiveBins) / m_totalExpected);
            if (bin < 0 || bin >= kProgressiveBins) continue;
            auto& b = m_progressiveOvr[bin];
            const auto& f = m_rgbData[i];
            if (f.rms    > b.rms)    b.rms    = f.rms;
            if (f.low    > b.low)    b.low    = f.low;
            if (f.lowMid > b.lowMid) b.lowMid = f.lowMid;
            if (f.mid    > b.mid)    b.mid    = f.mid;
            if (f.high   > b.high)   b.high   = f.high;
        }
        m_progressiveLastFrame = end;
    }
};
