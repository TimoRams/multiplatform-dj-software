#pragma once

#include <QObject>
#include <QVector>
#include <QHash>
#include <QMutex>
#include <QTimer>
#include <QElapsedTimer>
#include <QColor>
#include <QString>
#include <atomic>
#include <vector>
#include <memory>

#include "TransportLimits.h"
#include "TrackSegment.h"
#include "waveform/WaveformLineStore.h"
#include "waveform/WaveformLineBatch.h"
#include "waveform/WaveformNormalizationState.h"

namespace analysis { struct AnalysisResult; }
namespace waveform { struct PreparedWaveformLines; }

class TrackData : public QObject
{
    Q_OBJECT

    Q_PROPERTY(double bpm           READ getBpm           NOTIFY bpmAnalyzed)
    Q_PROPERTY(bool   isBpmAnalyzed READ isBpmAnalyzed    NOTIFY bpmAnalyzed)
    Q_PROPERTY(qint64 firstBeatSample READ getFirstBeatSample NOTIFY bpmAnalyzed)
    Q_PROPERTY(double sampleRate READ getSampleRate NOTIFY bpmAnalyzed)
    Q_PROPERTY(QString detectedKey  READ getDetectedKey   NOTIFY keyAnalyzed)
    Q_PROPERTY(bool   isKeyAnalyzed READ isKeyAnalyzed    NOTIFY keyAnalyzed)
    Q_PROPERTY(double analysisProgress READ analysisProgress NOTIFY analysisProgressChanged)
    Q_PROPERTY(bool   analyzing READ isAnalyzing NOTIFY analysisProgressChanged)

public:
    enum class BeatGridType {
        Unknown = 0,
        ConstantTempo,
        DynamicTempo
    };

    struct ConfidenceInfo {
        float bpmConfidence = 0.0f;
        float beatConfidence = 0.0f;
        float downbeatConfidence = 0.0f;
        float gridConfidence = 0.0f;
    };

    struct TempoNode {
        double positionSec = 0.0;
        double bpm = 0.0;
        float confidence = 0.0f;
    };

    struct BeatGridInfo {
        BeatGridType type = BeatGridType::Unknown;
        std::vector<TempoNode> tempoNodes;
        bool userModified = false;
        bool lockedByUser = false;
    };

    // Beat marker: one entry per beat in the track.
    // isDownbeat = true for beat 1 of each bar (every 4th beat, 4/4 time).
    // barNumber  = floor(beatIdx / 4) + 1; anchor downbeat (beatIdx 0) is bar 1,
    //              pre-roll continues with bar 0, -1, -2, …
    // beatInBar  = 1..4, beat position within the current bar.
    struct BeatMarker {
        double positionSec = 0.0;  // exact, micro-snapped timestamp (seconds)
        bool   isBeat      = true;
        bool   isDownbeat  = false;
        int    barIndex    = 0;    // 0-based bar counter, may be < 0 before anchor
        int    barNumber   = 0;
        int    beatInBar   = 1;    // 1=downbeat, 2, 3, 4
        float  confidence  = 0.0f;
        bool   userModified = false;
        bool   lockedByUser = false;
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
    };

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
    // Pre-downsampled overview used by deck overview paint() — O(kOverviewBins).
    static constexpr int kOverviewBins = 4096;

    // Max-fold downsample of full-res RGB frames for the deck overview waveform.
    static QVector<RgbWaveformFrame> downsampleOverview(
        const QVector<RgbWaveformFrame>& src, int maxBins = kOverviewBins);

    explicit TrackData(QObject* parent = nullptr);

    // Owner-thread boundary used by WaveformAnalyzer. The seed is captured
    // before the worker starts; only applyAnalysisResult mutates this QObject.
    [[nodiscard]] analysis::AnalysisResult createAnalysisSeed() const;
    bool applyAnalysisResult(const analysis::AnalysisResult& result);

    void setTotalExpected(int total) {
        assertOwnerThread();
        QMutexLocker locker(&m_mutex);
        m_totalExpected = total;
    }

    int getTotalExpected() const {
        QMutexLocker locker(&m_mutex);
        return m_totalExpected;
    }

    void setGlobalMaxPeak(float maxPeak) {
        assertOwnerThread();
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
                    std::vector<BeatMarker> beatGrid = {});

    void setBpmData(double bpm, qint64 firstBeatSample, double sampleRate,
                    std::vector<BeatMarker> beatGrid,
                    ConfidenceInfo confidence,
                    BeatGridInfo beatGridInfo);
    // Creates an unlocked, replaceable grid immediately from trusted tag BPM.
    // Cached or analyzed beat data replaces it later without changing the API.
    void ensureProvisionalBeatgrid(double trackLengthSec);

    // Returns the elastic beat-marker array.
    // Empty if the analyzer ran before this feature was added.
    std::vector<BeatMarker> getBeatGrid() const {
        QMutexLocker locker(&m_mutex);
        return m_beatGrid;
    }

    ConfidenceInfo getConfidenceInfo() const {
        QMutexLocker locker(&m_mutex);
        return m_confidence;
    }

    BeatGridInfo getBeatGridInfo() const {
        QMutexLocker locker(&m_mutex);
        return m_beatGridInfo;
    }

    bool beatgridLockedByUser() const {
        QMutexLocker locker(&m_mutex);
        return m_beatGridInfo.lockedByUser;
    }

    // Rebuilds the entire BeatMarker array so that newAnchorSec is beat 1 / bar 1.
    void shiftBeatgridToDownbeat(double newAnchorSec, double trackLengthSec);

    // Shift every beat marker by deltaSec (positive = later in the track).
    void nudgeBeatgrid(double deltaSec, double trackLengthSec);

    void setBeatgridLocked(bool locked);

    double getBpm() const {
        QMutexLocker locker(&m_mutex);
        return m_bpm;
    }

    // Update only the BPM value (used by manual x2 / ÷2 correction).
    // Does NOT rebuild the beat grid — the caller is responsible for that.
    void setBpm(double bpm);

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

    void setKeyData(const QString& camelotKey);

    QString getDetectedKey() const {
        QMutexLocker locker(&m_mutex);
        return m_detectedKey;
    }

    bool isKeyAnalyzed() const {
        QMutexLocker locker(&m_mutex);
        return m_isKeyAnalyzed;
    }

    double analysisProgress() const {
        return m_analysisProgress.load(std::memory_order_relaxed);
    }

    bool isAnalyzing() const {
        return m_analyzing.load(std::memory_order_relaxed);
    }

    // Called from the analyzer thread; throttles GUI notifications.
    void reportAnalysisProgress(double progress, bool active);

    void setSegmentsData(std::vector<TrackSegment> segments);

    std::vector<TrackSegment> getSegments() const {
        QMutexLocker locker(&m_mutex);
        return m_segments;
    }

    QVector<WaveformBin> getWaveformData() const {
        QMutexLocker locker(&m_mutex);
        if (m_waveformSnapshot) return *m_waveformSnapshot;
        return m_data;
    }

    void clearWaveformData();
    // Starts a new visual generation before asynchronous track preparation
    // completes. This prevents renderers from retaining tiles or overview data
    // belonging to the previously loaded track while the next track is queued.
    void beginVisualTrackLoad(std::uint64_t trackGeneration);
    void clear();

    void setPeakMipData(QVector<PeakFrame>&& data);

    int getPeakMipSize() const {
        QMutexLocker locker(&m_mutex);
        if (m_peakMipSnapshot) return m_peakMipSnapshot->size();
        return m_peakMip.size();
    }

    QVector<PeakFrame> getPeakMipSlice(int startIdx, int endIdx, int* outStartIdx = nullptr) const;

    QVector<PeakFrame> getPeakMipData() const {
        QMutexLocker locker(&m_mutex);
        if (m_peakMipSnapshot) return *m_peakMipSnapshot;
        return m_peakMip;
    }

    void setRgbWaveformData(QVector<RgbWaveformFrame>&& frames);
    void installCachedWaveform(QVector<WaveformBin>&& waveform,
                               float globalMaxPeak,
                               QVector<RgbWaveformFrame>&& rgb,
                               QVector<PeakFrame>&& peakMip,
                               std::shared_ptr<const waveform::PreparedWaveformLines> preparedLines,
                               QVector<RgbWaveformFrame>&& preparedOverview = {});
    void initializeCachedWaveformLines(int totalLines, int linesPerSecond,
                                       QVector<RgbWaveformFrame>&& preparedOverview);
    void applyCachedWaveformLineChunk(
        int firstLine, int totalLines, int linesPerSecond,
        std::shared_ptr<const std::vector<WaveformLine>> lines);
    void applyCachedWaveformLineBatch(
        int totalLines, int linesPerSecond, WaveformLineBatch chunks);
    void applyCachedWaveformLodBatch(WaveformLodBatch chunks);

    // Pre-downsampled overview (≤4096 bins) computed off the main thread.
    void setOverviewRgbData(QVector<RgbWaveformFrame>&& data);

    QVector<RgbWaveformFrame> getOverviewRgbData() const {
        QMutexLocker locker(&m_mutex);
        if (m_overviewSnapshot) return *m_overviewSnapshot;
        return m_overviewRgb;
    }
    std::shared_ptr<const QVector<RgbWaveformFrame>> getOverviewRgbSnapshot() const {
        QMutexLocker locker(&m_mutex);
        return m_overviewSnapshot;
    }
    std::shared_ptr<const QVector<RgbWaveformFrame>> getRgbWaveformSnapshot() const {
        QMutexLocker locker(&m_mutex);
        return m_rgbSnapshot;
    }
    std::shared_ptr<const QVector<PeakFrame>> getPeakMipSnapshot() const {
        QMutexLocker locker(&m_mutex);
        return m_peakMipSnapshot;
    }
    std::shared_ptr<const WaveformLineStoreSnapshot> getWaveformLineStoreSnapshot() const {
        QMutexLocker locker(&m_mutex);
        return m_waveformLineStore.snapshot();
    }

    void preallocateRgbWaveform(int numBins);

    // Write analyzed frames at a specific bin offset.
    void writeRgbWaveformRange(int fromBin, const QVector<RgbWaveformFrame>& data);

    void appendRgbWaveformData(const QVector<RgbWaveformFrame>& frames);

    QVector<RgbWaveformFrame> getRgbWaveformData() const {
        QMutexLocker locker(&m_mutex);
        if (m_rgbSnapshot) return *m_rgbSnapshot;
        return m_rgbData;
    }

    int getRgbWaveformSize() const {
        QMutexLocker locker(&m_mutex);
        if (m_rgbSnapshot) return m_rgbSnapshot->size();
        return m_rgbData.size();
    }

    // Progressive kProgressiveBins-bin overview updated incrementally during analysis.
    QVector<RgbWaveformFrame> getProgressiveOvrData(int* outProcessed = nullptr) const;

    QVector<RgbWaveformFrame> getRgbWaveformSlice(int startIndex, int endIndex, int* outStartIndex = nullptr) const;

    int fillRgbWaveformSlice(QVector<RgbWaveformFrame>& dst, int startIndex, int endIndex) const;

    int fillPeakMipSlice(QVector<PeakFrame>& dst, int startIdx, int endIdx) const;

    void appendData(const QVector<WaveformBin>& newData);
    void applyProgressiveWaveformChunk(int firstBin, int totalBins,
                                       const QVector<WaveformBin>& waveform,
                                       const QVector<RgbWaveformFrame>& rgb,
                                       bool publishLineStoreImmediately = true,
                                       WaveformNormalizationState normalizationState
                                           = WaveformNormalizationState::Preview);
    // The control clock batches worker chunks and flushes one immutable
    // replacement per touched render chunk.  Direct callers retain immediate
    // publication through the default argument above.
    void flushProgressiveWaveformLines();

    // Atomically replace the entire waveform with the final-polish version.
    void replaceAllData(QVector<WaveformBin>&& finalData, float finalGlobalMaxPeak);

    void reserve(int size);

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
    void beatgridChanged();
    void segmentsAnalyzed();
    void analysisProgressChanged();

private slots:
    void emitAnalysisProgress() { emit analysisProgressChanged(); }
    void flushRgbWaveformEmit();
    void scheduleRgbWaveformEmit();

private:
    QVector<WaveformBin> m_data;
    QVector<RgbWaveformFrame> m_rgbData;
    QVector<RgbWaveformFrame> m_overviewRgb;
    QVector<PeakFrame> m_peakMip;
    std::shared_ptr<const QVector<WaveformBin>> m_waveformSnapshot;
    std::shared_ptr<const QVector<RgbWaveformFrame>> m_rgbSnapshot;
    std::shared_ptr<const QVector<RgbWaveformFrame>> m_overviewSnapshot;
    std::shared_ptr<const QVector<PeakFrame>> m_peakMipSnapshot;
    WaveformLineStore m_waveformLineStore;
    std::uint64_t m_waveformLineGeneration = 0;
    int m_totalExpected;
    float m_globalMaxPeak;

    double  m_bpm;
    qint64  m_firstBeatSample;
    double  m_sampleRate;
    bool    m_isBpmAnalyzed;
    ConfidenceInfo m_confidence;
    BeatGridInfo m_beatGridInfo;
    std::vector<BeatMarker> m_beatGrid;
    std::vector<TrackSegment> m_segments;

    QString m_detectedKey;
    bool    m_isKeyAnalyzed;

    mutable QMutex m_mutex;
    bool m_rgbEmitPending = false;
    QElapsedTimer m_rgbEmitClock;
    std::atomic<double> m_analysisProgress{0.0};
    std::atomic<bool>   m_analyzing{false};
    std::atomic<int> m_lastEmittedProgressPct{-1};

    QVector<RgbWaveformFrame> m_progressiveOvr;
    int                       m_progressiveLastFrame = 0;
    // Readiness is separate from the RGB values: an all-zero frame is valid
    // silence and must not be mistaken for an unanalyzed frame.
    QVector<quint8>           m_progressiveRgbReady;
    QVector<quint8>           m_progressiveNormalizationStates;
    QVector<std::uint32_t>    m_progressiveDirtyLineChunks;
    // Progressive analysis is sparse: never allocate duration*analysisRate
    // arrays on the GUI thread. Only touched immutable render chunks are staged.
    QHash<std::uint32_t, std::shared_ptr<std::vector<WaveformLine>>>
        m_progressivePendingLineChunks;

    void _updateProgressiveOvr(int from, int to);
    void updateProgressiveOverviewFromChunkLocked(
        int firstBin, int totalBins, const QVector<RgbWaveformFrame>& rgb);
    void markProgressiveWaveformLinesDirtyLocked(int firstRgbFrame, int rgbFrameCount);
    void flushProgressiveWaveformLinesLocked();
    void publishProgressiveWaveformLinesLocked(int firstRgbFrame, int rgbFrameCount);
    void stageProgressiveWaveformLinesLocked(
        int firstBin, const QVector<RgbWaveformFrame>& rgb,
        WaveformNormalizationState normalizationState);
    void alignSegmentsToBeatgridLocked();
    void rebuildWaveformLineStoreLocked(std::uint64_t trackGeneration = 0);
    void installPreparedWaveformLinesLocked(
        const std::shared_ptr<const waveform::PreparedWaveformLines>& prepared,
        std::uint64_t trackGeneration = 0);
    void assertOwnerThread() const;
};
