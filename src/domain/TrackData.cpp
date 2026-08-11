#include "TrackData.h"
#include "analysis/AnalysisResult.h"
#include "waveform/WaveformLineBuilder.h"
#include "waveform/WaveformVisualStyle.h"

#include <QMetaObject>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr int kRgbFramesPerCanonicalLine = 1;
constexpr int kNormalizationRangeBins = 128;
constexpr int kPeakFramesPerCanonicalLine = TrackData::PEAK_POINTS_PER_SECOND
    / static_cast<int>(WaveformLineStore::kCanonicalLinesPerSecond);
static_assert(kPeakFramesPerCanonicalLine > 0);

WaveformLine makeCanonicalWaveformLine(const QVector<TrackData::RgbWaveformFrame>& rgb,
                                       const QVector<TrackData::PeakFrame>& peaks,
                                       int lineIndex,
                                       WaveformNormalizationState normalizationState)
{
    const int rgbBegin = lineIndex * kRgbFramesPerCanonicalLine;
    const int rgbEnd = std::min(rgbBegin + kRgbFramesPerCanonicalLine,
                                static_cast<int>(rgb.size()));
    float rms = 0.0f;
    float low = 0.0f, lowMid = 0.0f, mid = 0.0f, high = 0.0f;
    for (int i = rgbBegin; i < rgbEnd; ++i) {
        const auto& frame = rgb[i];
        rms = std::max(rms, frame.rms);
        low = std::max(low, frame.low);
        lowMid = std::max(lowMid, frame.lowMid);
        mid = std::max(mid, frame.mid);
        high = std::max(high, frame.high);
    }

    float minimum = -rms;
    float maximum = rms;
    if (!peaks.isEmpty()) {
        const int peakBegin = lineIndex * kPeakFramesPerCanonicalLine;
        const int peakEnd = std::min(peakBegin + kPeakFramesPerCanonicalLine,
                                     static_cast<int>(peaks.size()));
        for (int i = peakBegin; i < peakEnd; ++i) {
            minimum = std::min(minimum, peaks[i].minSample / 127.0f);
            maximum = std::max(maximum, peaks[i].maxSample / 127.0f);
        }
    }

    WaveformLine line;
    line.minimum = static_cast<std::int16_t>(std::lround(
        std::clamp(minimum, -1.0f, 0.0f) * 32767.0f));
    line.maximum = static_cast<std::int16_t>(std::lround(
        std::clamp(maximum, 0.0f, 1.0f) * 32767.0f));
    const auto color = waveform_visual::color({low, lowMid, mid, high, rms});
    line.red = color[0];
    line.green = color[1];
    line.blue = color[2];
    line.flags = waveform_line_flags::kAvailable;
    if (normalizationState == WaveformNormalizationState::Final)
        line.flags |= waveform_line_flags::kFinal;
    return line;
}

} // namespace

TrackData::TrackData(QObject* parent)
    : QObject(parent)
    , m_totalExpected(0)
    , m_globalMaxPeak(0.001f)
    , m_bpm(0.0)
    , m_firstBeatSample(0)
    , m_sampleRate(44100.0)
    , m_isBpmAnalyzed(false)
    , m_isKeyAnalyzed(false)
{
}

void TrackData::assertOwnerThread() const
{
    Q_ASSERT_X(QThread::currentThread() == thread(), "TrackData",
               "TrackData mutations are restricted to the QObject owner thread");
}

analysis::AnalysisResult TrackData::createAnalysisSeed() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    QMutexLocker locker(&m_mutex);
    analysis::AnalysisResult seed;
    seed.totalExpected = m_totalExpected;
    seed.globalMaxPeak = m_globalMaxPeak;
    seed.waveform = m_waveformSnapshot ? m_waveformSnapshot
        : std::make_shared<const QVector<WaveformBin>>(m_data);
    seed.rgbWaveform = m_rgbSnapshot ? m_rgbSnapshot
        : std::make_shared<const QVector<RgbWaveformFrame>>(m_rgbData);
    seed.overviewWaveform = m_overviewSnapshot ? m_overviewSnapshot
        : std::make_shared<const QVector<RgbWaveformFrame>>(m_overviewRgb);
    seed.peakMip = m_peakMipSnapshot ? m_peakMipSnapshot
        : std::make_shared<const QVector<PeakFrame>>(m_peakMip);
    seed.bpm = m_bpm;
    seed.firstBeatSample = m_firstBeatSample;
    seed.sampleRate = m_sampleRate;
    seed.confidence = m_confidence;
    seed.beatGrid = m_beatGridInfo;
    seed.beats = m_beatGrid;
    seed.phrases = m_segments;
    seed.detectedKey = m_detectedKey;
    return seed;
}

bool TrackData::applyAnalysisResult(const analysis::AnalysisResult& result)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!result.validated || !result.complete || !result.error.isEmpty())
        return false;
    {
        QMutexLocker locker(&m_mutex);
        m_totalExpected = result.totalExpected;
        m_globalMaxPeak = result.globalMaxPeak;
        m_waveformSnapshot = result.waveform;
        m_rgbSnapshot = result.rgbWaveform;
        m_overviewSnapshot = result.overviewWaveform;
        m_peakMipSnapshot = result.peakMip;
        if (result.preparedWaveformLines)
            installPreparedWaveformLinesLocked(result.preparedWaveformLines,
                                               result.identity.trackGeneration);
        else
            rebuildWaveformLineStoreLocked(result.identity.trackGeneration);
        m_data.clear(); m_rgbData.clear(); m_overviewRgb.clear(); m_peakMip.clear();
        m_progressiveRgbReady.clear();
        m_progressiveNormalizationStates.clear();
        m_progressiveDirtyLineChunks.clear();
        m_progressivePendingLineChunks.clear();
        m_bpm = result.bpm;
        m_firstBeatSample = result.firstBeatSample;
        m_sampleRate = result.sampleRate;
        m_isBpmAnalyzed = result.bpm > 0.0;
        m_confidence = result.confidence;
        if (!m_beatGridInfo.lockedByUser) {
            m_beatGridInfo = result.beatGrid;
            m_beatGrid = result.beats;
        }
        m_segments = result.phrases;
        m_detectedKey = result.detectedKey;
        m_isKeyAnalyzed = !m_detectedKey.isEmpty();
    }
    emit dataUpdated();
    emit rgbWaveformUpdated();
    emit overviewRgbUpdated();
    emit peakMipUpdated();
    emit bpmAnalyzed();
    emit beatgridChanged();
    emit segmentsAnalyzed();
    emit keyAnalyzed();
    return true;
}

void TrackData::rebuildWaveformLineStoreLocked(std::uint64_t trackGeneration)
{
    const auto rgb = m_rgbSnapshot ? m_rgbSnapshot
        : std::make_shared<const QVector<RgbWaveformFrame>>(m_rgbData);
    const auto peaks = m_peakMipSnapshot ? m_peakMipSnapshot
        : std::make_shared<const QVector<PeakFrame>>(m_peakMip);
    if (!rgb || rgb->isEmpty())
        return;

    // The render-store generation is intentionally monotonic even when a loader
    // generation is reused after cancellation.  A scene-graph snapshot must
    // never mistake a new timeline for an older immutable one.
    trackGeneration = std::max(trackGeneration, m_waveformLineGeneration + 1);
    m_waveformLineGeneration = trackGeneration;

    constexpr int rgbPerLine = kRgbFramesPerCanonicalLine;
    const int totalLines = (rgb->size() + rgbPerLine - 1) / rgbPerLine;
    m_waveformLineStore.reset(trackGeneration, static_cast<std::uint32_t>(totalLines));

    for (int first = 0, chunkIndex = 0; first < totalLines;
         first += static_cast<int>(WaveformLineStore::kChunkSize), ++chunkIndex) {
        const int count = std::min(static_cast<int>(WaveformLineStore::kChunkSize), totalLines - first);
        auto lines = std::make_shared<std::vector<WaveformLine>>(static_cast<size_t>(count));
        const QVector<PeakFrame> emptyPeaks;
        const auto& peakData = peaks ? *peaks : emptyPeaks;
        for (int local = 0; local < count; ++local)
            (*lines)[static_cast<size_t>(local)] = makeCanonicalWaveformLine(
                *rgb, peakData, first + local,
                WaveformNormalizationState::Final);
        const auto published = m_waveformLineStore.publish({
            trackGeneration, static_cast<std::uint32_t>(chunkIndex), static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(totalLines), std::move(lines)});
        Q_ASSERT(published == WaveformLineStore::PublishResult::Accepted);
    }
}

void TrackData::installPreparedWaveformLinesLocked(
    const std::shared_ptr<const waveform::PreparedWaveformLines>& prepared,
    std::uint64_t trackGeneration)
{
    if (!prepared || prepared->totalLineCount == 0 || prepared->chunks.empty()) {
        rebuildWaveformLineStoreLocked(trackGeneration);
        return;
    }

    trackGeneration = std::max(trackGeneration, m_waveformLineGeneration + 1);
    m_waveformLineGeneration = trackGeneration;
    m_waveformLineStore.reset(trackGeneration, prepared->totalLineCount);
    std::uint32_t first = 0;
    for (std::uint32_t chunkIndex = 0; chunkIndex < prepared->chunks.size(); ++chunkIndex) {
        const auto& lines = prepared->chunks[chunkIndex];
        if (!lines || lines->empty() || first >= prepared->totalLineCount) {
            rebuildWaveformLineStoreLocked(trackGeneration + 1);
            return;
        }
        const auto count = static_cast<std::uint32_t>(lines->size());
        const auto published = m_waveformLineStore.publish({
            trackGeneration, chunkIndex, first, count,
            prepared->totalLineCount, lines});
        if (published != WaveformLineStore::PublishResult::Accepted) {
            rebuildWaveformLineStoreLocked(trackGeneration + 1);
            return;
        }
        first += count;
    }
    if (first != prepared->totalLineCount)
        rebuildWaveformLineStoreLocked(trackGeneration + 1);
}

QVector<TrackData::RgbWaveformFrame> TrackData::downsampleOverview(
    const QVector<RgbWaveformFrame>& src, int maxBins)
{
    if (src.isEmpty())
        return {};
    const int total = src.size();
    const int factor = std::max(1, (total + maxBins - 1) / maxBins);
    QVector<RgbWaveformFrame> out;
    out.reserve((total + factor - 1) / factor);
    for (int i = 0; i < total; i += factor) {
        const int end = std::min(i + factor, total);
        float rms = 0.0f, low = 0.0f, lowMid = 0.0f, mid = 0.0f, high = 0.0f;
        for (int j = i; j < end; ++j) {
            const auto& f = src[j];
            rms    = std::max(rms,    f.rms);
            low    = std::max(low,    f.low);
            lowMid = std::max(lowMid, f.lowMid);
            mid    = std::max(mid,    f.mid);
            high   = std::max(high,   f.high);
        }
        RgbWaveformFrame bin;
        bin.rms = rms; bin.low = low; bin.lowMid = lowMid;
        bin.mid = mid; bin.high = high;
        out.push_back(bin);
    }
    return out;
}

void TrackData::setBpmData(double bpm, qint64 firstBeatSample, double sampleRate,
                           std::vector<BeatMarker> beatGrid)
{
    assertOwnerThread();
    setBpmData(bpm, firstBeatSample, sampleRate, std::move(beatGrid), ConfidenceInfo{}, BeatGridInfo{});
}

void TrackData::setBpmData(double bpm, qint64 firstBeatSample, double sampleRate,
                           std::vector<BeatMarker> beatGrid,
                           ConfidenceInfo confidence,
                           BeatGridInfo beatGridInfo)
{
    bool beatgridUpdated = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_beatGridInfo.lockedByUser && !beatGrid.empty()) {
            beatGrid.clear();
            beatGridInfo = m_beatGridInfo;
        }
        m_bpm             = bpm;
        m_firstBeatSample = firstBeatSample;
        m_sampleRate      = sampleRate;
        m_isBpmAnalyzed   = (bpm > 0.0);
        m_confidence      = confidence;
        if (!beatGrid.empty()) {
            m_beatGrid = std::move(beatGrid);
            beatgridUpdated = true;
        }
        if (beatGridInfo.type != BeatGridType::Unknown || !beatGridInfo.tempoNodes.empty()
            || beatGridInfo.userModified || beatGridInfo.lockedByUser) {
            m_beatGridInfo = std::move(beatGridInfo);
        }
    }
    emit bpmAnalyzed();
    if (beatgridUpdated)
        emit beatgridChanged();
}

void TrackData::ensureProvisionalBeatgrid(double trackLengthSec)
{
    assertOwnerThread();
    double bpm = 0.0;
    double sampleRate = 0.0;
    qint64 firstBeatSample = 0;
    float confidence = 0.35f;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_beatGrid.empty() || m_bpm <= 0.0 || m_sampleRate <= 0.0
            || trackLengthSec <= 0.0) {
            return;
        }
        bpm = m_bpm;
        sampleRate = m_sampleRate;
        firstBeatSample = m_firstBeatSample;
        confidence = std::max(confidence, m_confidence.bpmConfidence);
    }

    const double beatDuration = 60.0 / bpm;
    const double anchor = static_cast<double>(firstBeatSample) / sampleRate;
    const int firstIndex = static_cast<int>(
        std::floor((-TransportLimits::kPreRollSeconds - anchor) / beatDuration));
    const int lastIndex = static_cast<int>(
        std::ceil((trackLengthSec - anchor) / beatDuration));
    std::vector<BeatMarker> grid;
    grid.reserve(static_cast<size_t>(std::max(0, lastIndex - firstIndex + 1)));
    for (int beatIndex = firstIndex; beatIndex <= lastIndex; ++beatIndex) {
        const double position = anchor + static_cast<double>(beatIndex) * beatDuration;
        if (position < -TransportLimits::kPreRollSeconds - 0.001
            || position > trackLengthSec + 0.001) {
            continue;
        }
        const int beatInBarZeroBased = ((beatIndex % 4) + 4) % 4;
        BeatMarker marker;
        marker.positionSec = position;
        marker.isDownbeat = beatInBarZeroBased == 0;
        marker.barIndex = static_cast<int>(
            std::floor(static_cast<double>(beatIndex) / 4.0));
        marker.barNumber = marker.barIndex + 1;
        marker.beatInBar = beatInBarZeroBased + 1;
        marker.confidence = confidence;
        grid.push_back(marker);
    }

    {
        QMutexLocker locker(&m_mutex);
        if (!m_beatGrid.empty() || std::abs(m_bpm - bpm) > 1.0e-9)
            return;
        m_beatGrid = std::move(grid);
        m_beatGridInfo.type = BeatGridType::ConstantTempo;
        m_beatGridInfo.tempoNodes = {{anchor, bpm, confidence}};
        m_beatGridInfo.userModified = false;
        m_beatGridInfo.lockedByUser = false;
    }
    emit beatgridChanged();
}

void TrackData::shiftBeatgridToDownbeat(double newAnchorSec, double trackLengthSec)
{
    assertOwnerThread();
    double bpm;
    {
        QMutexLocker locker(&m_mutex);
        bpm = m_bpm;
    }
    if (bpm <= 0.0 || trackLengthSec <= 0.0) return;

    const double beatDur = 60.0 / bpm;
    std::vector<BeatMarker> grid;
    grid.reserve(static_cast<size_t>(trackLengthSec / beatDur) + 4);

    const double preRollSec = TransportLimits::kPreRollSeconds;
    for (int i = -1; ; --i) {
        double pos = newAnchorSec + i * beatDur;
        if (pos < -(preRollSec + beatDur * 0.5)) break;
        if (pos < -preRollSec) pos = -preRollSec;
        BeatMarker marker;
        marker.positionSec = pos;
        marker.barIndex = i / 4;
        marker.barNumber = marker.barIndex + 1;
        marker.confidence = 1.0f;
        marker.userModified = true;
        marker.lockedByUser = true;
        grid.push_back(marker);
        if (pos <= -preRollSec) break;
    }

    for (int i = 0; ; ++i) {
        double pos = newAnchorSec + i * beatDur;
        if (pos > trackLengthSec + beatDur * 0.5) break;
        if (pos > trackLengthSec) pos = trackLengthSec;
        BeatMarker marker;
        marker.positionSec = pos;
        marker.barIndex = i / 4;
        marker.barNumber = marker.barIndex + 1;
        marker.confidence = 1.0f;
        marker.userModified = true;
        marker.lockedByUser = true;
        grid.push_back(marker);
        if (pos >= trackLengthSec) break;
    }

    std::sort(grid.begin(), grid.end(),
        [](const BeatMarker& a, const BeatMarker& b){
            return a.positionSec < b.positionSec;
        });
    grid.erase(std::unique(grid.begin(), grid.end(),
        [](const BeatMarker& a, const BeatMarker& b){
            return std::abs(a.positionSec - b.positionSec) < 0.001;
        }), grid.end());

    for (int k = 0; k < static_cast<int>(grid.size()); ++k) {
        const int beatIdx = static_cast<int>(
            std::llround((grid[k].positionSec - newAnchorSec) / beatDur));
        const int mod4 = ((beatIdx % 4) + 4) % 4;
        grid[k].isDownbeat = (mod4 == 0);
        grid[k].barIndex = static_cast<int>(
            std::floor(static_cast<double>(beatIdx) / 4.0));
        grid[k].barNumber = grid[k].barIndex + 1;
        grid[k].beatInBar = mod4 + 1;
        grid[k].confidence = 1.0f;
        grid[k].userModified = true;
        grid[k].lockedByUser = true;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_firstBeatSample = static_cast<qint64>(std::llround(newAnchorSec * m_sampleRate));
        m_beatGrid = std::move(grid);
        m_beatGridInfo.type = BeatGridType::ConstantTempo;
        m_beatGridInfo.userModified = true;
        m_beatGridInfo.lockedByUser = true;
        alignSegmentsToBeatgridLocked();
    }
    emit beatgridChanged();
    emit segmentsAnalyzed();
}

void TrackData::nudgeBeatgrid(double deltaSec, double trackLengthSec)
{
    assertOwnerThread();
    if (std::abs(deltaSec) < 1e-9 || trackLengthSec <= 0.0)
        return;

    {
        QMutexLocker locker(&m_mutex);
        if (m_beatGrid.empty())
            return;

        const double preRollSec = TransportLimits::kPreRollSeconds;
        for (auto& marker : m_beatGrid) {
            marker.positionSec = std::clamp(marker.positionSec + deltaSec,
                                            -(preRollSec + 0.001),
                                            trackLengthSec + 0.001);
            marker.userModified = true;
            marker.lockedByUser = true;
        }

        std::sort(m_beatGrid.begin(), m_beatGrid.end(),
                  [](const BeatMarker& a, const BeatMarker& b) {
                      return a.positionSec < b.positionSec;
                  });

        const qint64 anchorSample = static_cast<qint64>(
            std::llround(std::max(0.0, m_beatGrid.front().positionSec) * m_sampleRate));
        m_firstBeatSample = anchorSample;
        m_beatGridInfo.userModified = true;
        m_beatGridInfo.lockedByUser = true;
        alignSegmentsToBeatgridLocked();
    }
    emit beatgridChanged();
    emit segmentsAnalyzed();
}

void TrackData::setBeatgridLocked(bool locked)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        m_beatGridInfo.lockedByUser = locked;
        for (auto& marker : m_beatGrid)
            marker.lockedByUser = locked;
    }
    emit beatgridChanged();
}

void TrackData::setBpm(double bpm)
{
    assertOwnerThread();
    bool changed = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_bpm != bpm) {
            m_bpm = bpm;
            changed = true;
        }
    }
    if (changed)
        emit bpmAnalyzed();
}

void TrackData::setKeyData(const QString& camelotKey)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        m_detectedKey  = camelotKey;
        m_isKeyAnalyzed = !camelotKey.isEmpty();
    }
    emit keyAnalyzed();
}

void TrackData::reportAnalysisProgress(double progress, bool active)
{
    assertOwnerThread();
    m_analysisProgress.store(std::clamp(progress, 0.0, 1.0), std::memory_order_relaxed);
    m_analyzing.store(active, std::memory_order_relaxed);

    const int pct = static_cast<int>(std::round(progress * 100.0));
    if (!active) {
        m_lastEmittedProgressPct.store(-1, std::memory_order_relaxed);
        QMetaObject::invokeMethod(this, "emitAnalysisProgress", Qt::QueuedConnection);
        return;
    }
    if (pct == m_lastEmittedProgressPct.exchange(pct, std::memory_order_relaxed))
        return;
    QMetaObject::invokeMethod(this, "emitAnalysisProgress", Qt::QueuedConnection);
}

void TrackData::setSegmentsData(std::vector<TrackSegment> segments)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        m_segments = std::move(segments);
        if (m_beatGridInfo.lockedByUser)
            alignSegmentsToBeatgridLocked();
    }
    emit segmentsAnalyzed();
}

void TrackData::clearWaveformData()
{
    assertOwnerThread();
    bool keepPreview = false;
    {
        QMutexLocker locker(&m_mutex);
        keepPreview = (m_overviewSnapshot && !m_overviewSnapshot->isEmpty())
            || !m_overviewRgb.isEmpty();
        m_data.clear();
        m_rgbData.clear();
        m_peakMip.clear();
        m_waveformSnapshot.reset();
        m_rgbSnapshot.reset();
        m_peakMipSnapshot.reset();
        m_waveformLineStore.reset(++m_waveformLineGeneration, 0);
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
        m_progressiveRgbReady.clear();
        m_progressiveNormalizationStates.clear();
        m_progressiveDirtyLineChunks.clear();
        m_progressivePendingLineChunks.clear();
        if (!keepPreview)
            m_totalExpected = 0;
        m_globalMaxPeak = 0.001f;
    }
    if (keepPreview)
        emit overviewRgbUpdated();
    else
        emit dataCleared();
}

void TrackData::beginVisualTrackLoad(std::uint64_t trackGeneration)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        m_data.clear();
        m_rgbData.clear();
        m_overviewRgb.clear();
        m_peakMip.clear();
        m_waveformSnapshot.reset();
        m_rgbSnapshot.reset();
        m_overviewSnapshot.reset();
        m_peakMipSnapshot.reset();
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
        m_progressiveRgbReady.clear();
        m_progressiveNormalizationStates.clear();
        m_progressiveDirtyLineChunks.clear();
        m_progressivePendingLineChunks.clear();
        m_totalExpected = 0;
        m_globalMaxPeak = 0.001f;

        // Loader generations and renderer generations share only the identity
        // boundary. Preserve monotonicity even if a direct/test caller supplies
        // an older loader generation.
        m_waveformLineGeneration = std::max(
            trackGeneration, m_waveformLineGeneration + 1);
        m_waveformLineStore.reset(m_waveformLineGeneration, 0);
    }
    emit dataCleared();
}

void TrackData::clear()
{
    assertOwnerThread();
    reportAnalysisProgress(0.0, false);
    {
        QMutexLocker locker(&m_mutex);
        m_data.clear();
        m_rgbData.clear();
        m_overviewRgb.clear();
        m_peakMip.clear();
        m_waveformSnapshot.reset();
        m_rgbSnapshot.reset();
        m_overviewSnapshot.reset();
        m_peakMipSnapshot.reset();
        m_waveformLineStore.reset(++m_waveformLineGeneration, 0);
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
        m_progressiveRgbReady.clear();
        m_progressiveNormalizationStates.clear();
        m_progressiveDirtyLineChunks.clear();
        m_progressivePendingLineChunks.clear();
        m_totalExpected = 0;
        m_globalMaxPeak = 0.001f;
        m_bpm = 0.0;
        m_firstBeatSample = 0;
        m_isBpmAnalyzed = false;
        m_confidence = {};
        m_beatGridInfo = {};
        m_detectedKey.clear();
        m_isKeyAnalyzed = false;
        m_beatGrid.clear();
        m_segments.clear();
    }
    emit dataCleared();
}

void TrackData::setPeakMipData(QVector<PeakFrame>&& data)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        m_peakMipSnapshot = std::make_shared<const QVector<PeakFrame>>(std::move(data));
        m_peakMip.clear();
        rebuildWaveformLineStoreLocked();
    }
    emit peakMipUpdated();
}

QVector<TrackData::PeakFrame> TrackData::getPeakMipSlice(int startIdx, int endIdx, int* outStartIdx) const
{
    QMutexLocker locker(&m_mutex);
    if (outStartIdx)
        *outStartIdx = 0;
    const auto* source = m_peakMipSnapshot ? m_peakMipSnapshot.get() : &m_peakMip;
    if (source->isEmpty())
        return {};
    const int lo = std::max(0, startIdx);
    const int hi = std::min(endIdx, static_cast<int>(source->size()));
    if (lo >= hi)
        return {};
    QVector<PeakFrame> slice;
    slice.reserve(hi - lo);
    for (int i = lo; i < hi; ++i)
        slice.push_back((*source)[i]);
    if (outStartIdx)
        *outStartIdx = lo;
    return slice;
}

void TrackData::setRgbWaveformData(QVector<RgbWaveformFrame>&& frames)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        auto overview = downsampleOverview(frames);
        m_rgbSnapshot = std::make_shared<const QVector<RgbWaveformFrame>>(std::move(frames));
        m_overviewSnapshot = std::make_shared<const QVector<RgbWaveformFrame>>(std::move(overview));
        m_rgbData.clear();
        m_overviewRgb.clear();
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
        m_progressiveRgbReady.clear();
        m_progressiveNormalizationStates.clear();
        m_progressiveDirtyLineChunks.clear();
        m_progressivePendingLineChunks.clear();
        rebuildWaveformLineStoreLocked();
    }
    emit rgbWaveformUpdated();
    emit overviewRgbUpdated();
}

void TrackData::installCachedWaveform(
    QVector<WaveformBin>&& waveform,
    float globalMaxPeak,
    QVector<RgbWaveformFrame>&& rgb,
    QVector<PeakFrame>&& peakMip,
    std::shared_ptr<const waveform::PreparedWaveformLines> preparedLines,
    QVector<RgbWaveformFrame>&& preparedOverview)
{
    assertOwnerThread();
    // Production cache loads prepare this on the loader thread. Keep a fallback
    // for direct/test callers, but never rescan a long cached timeline in the UI.
    if (preparedOverview.isEmpty())
        preparedOverview = downsampleOverview(rgb);
    {
        QMutexLocker locker(&m_mutex);
        m_totalExpected = rgb.size();
        m_globalMaxPeak = std::max(0.001f, globalMaxPeak);
        m_waveformSnapshot = std::make_shared<const QVector<WaveformBin>>(std::move(waveform));
        m_rgbSnapshot = std::make_shared<const QVector<RgbWaveformFrame>>(std::move(rgb));
        m_overviewSnapshot = std::make_shared<const QVector<RgbWaveformFrame>>(
            std::move(preparedOverview));
        m_peakMipSnapshot = std::make_shared<const QVector<PeakFrame>>(std::move(peakMip));
        m_data.clear();
        m_rgbData.clear();
        m_overviewRgb.clear();
        m_peakMip.clear();
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
        m_progressiveRgbReady.clear();
        m_progressiveNormalizationStates.clear();
        m_progressiveDirtyLineChunks.clear();
        installPreparedWaveformLinesLocked(preparedLines);
    }
    emit dataUpdated();
    emit rgbWaveformUpdated();
    emit overviewRgbUpdated();
    emit peakMipUpdated();
}

void TrackData::initializeCachedWaveformLines(
    int totalLines,
    int linesPerSecond,
    QVector<RgbWaveformFrame>&& preparedOverview)
{
    assertOwnerThread();
    if (totalLines <= 0 || linesPerSecond <= 0)
        return;
    {
        QMutexLocker locker(&m_mutex);
        m_totalExpected = totalLines;
        m_data.clear();
        m_rgbData.clear();
        m_peakMip.clear();
        m_waveformSnapshot.reset();
        m_rgbSnapshot.reset();
        m_peakMipSnapshot.reset();
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
        m_progressivePendingLineChunks.clear();
        m_progressiveDirtyLineChunks.clear();
        m_overviewSnapshot = std::make_shared<const QVector<RgbWaveformFrame>>(
            std::move(preparedOverview));
        m_overviewRgb.clear();
        m_waveformLineStore.reset(
            ++m_waveformLineGeneration,
            static_cast<std::uint32_t>(totalLines),
            static_cast<std::uint32_t>(linesPerSecond));
    }
    emit overviewRgbUpdated();
    emit dataUpdated();
}

void TrackData::applyCachedWaveformLineChunk(
    int firstLine,
    int totalLines,
    int linesPerSecond,
    std::shared_ptr<const std::vector<WaveformLine>> lines)
{
    WaveformLineBatch batch;
    batch.push_back({firstLine, std::move(lines)});
    applyCachedWaveformLineBatch(totalLines, linesPerSecond, std::move(batch));
}

void TrackData::applyCachedWaveformLineBatch(
    int totalLines,
    int linesPerSecond,
    WaveformLineBatch chunks)
{
    assertOwnerThread();
    if (totalLines <= 0 || linesPerSecond <= 0 || chunks.empty())
        return;
    bool accepted = false;
    {
        QMutexLocker locker(&m_mutex);
        auto snapshot = m_waveformLineStore.snapshot();
        if (!snapshot || snapshot->totalLineCount != static_cast<std::uint32_t>(totalLines)
            || snapshot->linesPerSecond != static_cast<std::uint32_t>(linesPerSecond)) {
            m_totalExpected = totalLines;
            m_waveformLineStore.reset(
                ++m_waveformLineGeneration,
                static_cast<std::uint32_t>(totalLines),
                static_cast<std::uint32_t>(linesPerSecond));
            snapshot = m_waveformLineStore.snapshot();
        }
        const auto chunkSize = static_cast<int>(snapshot->chunkSize);
        if (chunkSize <= 0)
            return;

        // Validate the complete viewport batch before publishing its first
        // member. Readers hold the same TrackData mutex, so they can observe
        // either the old snapshot or the complete guarded window, never a
        // partially filled sequence of adjacent cache chunks.
        for (const auto& chunk : chunks) {
            if (chunk.firstLine < 0 || chunk.firstLine >= totalLines
                || chunk.firstLine % chunkSize != 0
                || !chunk.lines || chunk.lines->empty()
                || static_cast<int>(chunk.lines->size())
                    != std::min(chunkSize, totalLines - chunk.firstLine)) {
                return;
            }
        }
        for (auto& chunk : chunks) {
            const auto count = static_cast<int>(chunk.lines->size());
            const auto chunkIndex = static_cast<std::uint32_t>(
                chunk.firstLine / chunkSize);
            const auto published = m_waveformLineStore.publish({
                snapshot->trackGeneration,
                chunkIndex,
                static_cast<std::uint32_t>(chunk.firstLine),
                static_cast<std::uint32_t>(count),
                static_cast<std::uint32_t>(totalLines),
                std::move(chunk.lines)});
            accepted = accepted
                || published != WaveformLineStore::PublishResult::Rejected;
        }
    }
    if (accepted) {
        emit dataUpdated();
        scheduleRgbWaveformEmit();
    }
}

void TrackData::applyCachedWaveformLodBatch(WaveformLodBatch chunks)
{
    assertOwnerThread();
    if (chunks.empty())
        return;
    bool accepted = false;
    {
        QMutexLocker locker(&m_mutex);
        accepted = m_waveformLineStore.publishLodBatch(std::move(chunks));
    }
    if (accepted)
        emit dataUpdated();
}

void TrackData::setOverviewRgbData(QVector<RgbWaveformFrame>&& data)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        m_overviewSnapshot = std::make_shared<const QVector<RgbWaveformFrame>>(std::move(data));
        m_overviewRgb.clear();
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
    }
    emit overviewRgbUpdated();
}

void TrackData::preallocateRgbWaveform(int numBins)
{
    assertOwnerThread();
    QMutexLocker locker(&m_mutex);
    m_rgbSnapshot.reset();
    m_rgbData.fill(RgbWaveformFrame{}, numBins);
    m_progressiveRgbReady.fill(0, numBins);
    m_progressiveDirtyLineChunks.clear();
}

void TrackData::writeRgbWaveformRange(int fromBin, const QVector<RgbWaveformFrame>& data)
{
    assertOwnerThread();
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
    scheduleRgbWaveformEmit();
}

void TrackData::appendRgbWaveformData(const QVector<RgbWaveformFrame>& frames)
{
    assertOwnerThread();
    if (frames.isEmpty())
        return;
    {
        QMutexLocker locker(&m_mutex);
        const int oldSize = m_rgbData.size();
        m_rgbData.append(frames);
        _updateProgressiveOvr(oldSize, m_rgbData.size());
    }
    scheduleRgbWaveformEmit();
}

QVector<TrackData::RgbWaveformFrame> TrackData::getProgressiveOvrData(int* outProcessed) const
{
    QMutexLocker locker(&m_mutex);
    if (outProcessed) *outProcessed = m_progressiveLastFrame;
    return m_progressiveOvr;
}

QVector<TrackData::RgbWaveformFrame> TrackData::getRgbWaveformSlice(
    int startIndex, int endIndex, int* outStartIndex) const
{
    QMutexLocker locker(&m_mutex);

    if (outStartIndex)
        *outStartIndex = 0;

    const auto* source = m_rgbSnapshot ? m_rgbSnapshot.get() : &m_rgbData;
    if (source->isEmpty())
        return {};

    const int lo = std::max(0, startIndex);
    const int hi = std::min(endIndex, static_cast<int>(source->size()));
    if (lo >= hi)
        return {};

    QVector<RgbWaveformFrame> slice;
    slice.reserve(hi - lo);
    for (int i = lo; i < hi; ++i)
        slice.push_back((*source)[i]);

    if (outStartIndex)
        *outStartIndex = lo;

    return slice;
}

int TrackData::fillRgbWaveformSlice(QVector<RgbWaveformFrame>& dst, int startIndex, int endIndex) const
{
    QMutexLocker locker(&m_mutex);
    dst.clear();
    const auto* source = m_rgbSnapshot ? m_rgbSnapshot.get() : &m_rgbData;
    if (source->isEmpty()) return 0;
    const int lo = std::max(0, startIndex);
    const int hi = std::min(endIndex, static_cast<int>(source->size()));
    if (lo >= hi) return lo;
    const int n = hi - lo;
    dst.resize(n);
    for (int i = 0; i < n; ++i)
        dst[i] = (*source)[lo + i];
    return lo;
}

int TrackData::fillPeakMipSlice(QVector<PeakFrame>& dst, int startIdx, int endIdx) const
{
    QMutexLocker locker(&m_mutex);
    dst.clear();
    const auto* source = m_peakMipSnapshot ? m_peakMipSnapshot.get() : &m_peakMip;
    if (source->isEmpty()) return 0;
    const int lo = std::max(0, startIdx);
    const int hi = std::min(endIdx, static_cast<int>(source->size()));
    if (lo >= hi) return lo;
    const int n = hi - lo;
    dst.resize(n);
    for (int i = 0; i < n; ++i)
        dst[i] = (*source)[lo + i];
    return lo;
}

void TrackData::appendData(const QVector<WaveformBin>& newData)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        m_waveformSnapshot.reset();
        m_data.append(newData);
    }
    emit dataUpdated();
}

void TrackData::applyProgressiveWaveformChunk(int firstBin, int totalBins,
                                              const QVector<WaveformBin>& waveform,
                                              const QVector<RgbWaveformFrame>& rgb,
                                              bool publishLineStoreImmediately,
                                              WaveformNormalizationState normalizationState)
{
    assertOwnerThread();
    (void)waveform; // Legacy progressive amplitudes; renderers consume sparse RGB line chunks.
    if (totalBins <= 0 || firstBin < 0 || (waveform.isEmpty() && rgb.isEmpty()))
        return;
    bool acceptedUpdate = false;
    {
        QMutexLocker locker(&m_mutex);
        const int totalLines = (totalBins + kRgbFramesPerCanonicalLine - 1)
            / kRgbFramesPerCanonicalLine;
        const auto lineSnapshot = m_waveformLineStore.snapshot();
        if (m_totalExpected != totalBins || !lineSnapshot
            || lineSnapshot->totalLineCount != static_cast<std::uint32_t>(totalLines)) {
            m_totalExpected = totalBins;
            // Full duration-sized vectors used to be allocated and zeroed here
            // on the GUI thread. For long sets that meant millions of objects in
            // one frame. Progressive rendering now lives only in sparse line
            // chunks plus the fixed-size overview until the immutable final
            // analysis snapshot arrives from the worker.
            m_data.clear();
            m_rgbData.clear();
            m_progressiveRgbReady.clear();
            m_progressiveNormalizationStates.fill(
                static_cast<quint8>(WaveformNormalizationState::Preview),
                (totalBins + kNormalizationRangeBins - 1)
                    / kNormalizationRangeBins);
            m_progressiveDirtyLineChunks.clear();
            m_progressivePendingLineChunks.clear();
            m_waveformSnapshot.reset();
            m_rgbSnapshot.reset();
            m_waveformLineStore.reset(++m_waveformLineGeneration,
                                      static_cast<std::uint32_t>(totalLines));
        }
        const int rgbCount = std::min(static_cast<int>(rgb.size()), totalBins - firstBin);
        if (rgbCount > 0) {
            const QVector<RgbWaveformFrame> boundedRgb = rgbCount == rgb.size()
                ? rgb : rgb.mid(0, rgbCount);
            if (m_progressiveNormalizationStates.isEmpty()) {
                m_progressiveNormalizationStates.fill(
                    static_cast<quint8>(WaveformNormalizationState::Preview),
                    (totalBins + kNormalizationRangeBins - 1)
                        / kNormalizationRangeBins);
            }
            int local = 0;
            while (local < boundedRgb.size()) {
                const int global = firstBin + local;
                const int range = global / kNormalizationRangeBins;
                const int rangeEnd = std::min(
                    firstBin + static_cast<int>(boundedRgb.size()),
                    (range + 1) * kNormalizationRangeBins);
                const int count = rangeEnd - global;
                const bool alreadyFinal = range >= 0
                    && range < m_progressiveNormalizationStates.size()
                    && m_progressiveNormalizationStates[range]
                        == static_cast<quint8>(WaveformNormalizationState::Final);
                if (!(normalizationState == WaveformNormalizationState::Preview
                      && alreadyFinal)) {
                    const auto segment = boundedRgb.mid(local, count);
                    stageProgressiveWaveformLinesLocked(
                        global, segment, normalizationState);
                    updateProgressiveOverviewFromChunkLocked(
                        global, totalBins, segment);
                    acceptedUpdate = true;
                    if (normalizationState == WaveformNormalizationState::Final
                        && range >= 0
                        && range < m_progressiveNormalizationStates.size()) {
                        m_progressiveNormalizationStates[range] =
                            static_cast<quint8>(WaveformNormalizationState::Final);
                    }
                }
                local += count;
            }
            if (acceptedUpdate && publishLineStoreImmediately)
                flushProgressiveWaveformLinesLocked();
        }
    }
    if (acceptedUpdate)
        emit dataUpdated();
    if (acceptedUpdate && !rgb.isEmpty())
        scheduleRgbWaveformEmit();
}

void TrackData::flushProgressiveWaveformLines()
{
    assertOwnerThread();
    QMutexLocker locker(&m_mutex);
    flushProgressiveWaveformLinesLocked();
}

void TrackData::markProgressiveWaveformLinesDirtyLocked(int firstRgbFrame, int rgbFrameCount)
{
    const auto snapshot = m_waveformLineStore.snapshot();
    if (rgbFrameCount <= 0 || !snapshot || snapshot->chunkSize == 0
        || snapshot->totalLineCount == 0) {
        return;
    }
    const int firstLine = firstRgbFrame / kRgbFramesPerCanonicalLine;
    const int lastLine = std::min(static_cast<int>(snapshot->totalLineCount) - 1,
        (firstRgbFrame + rgbFrameCount - 1) / kRgbFramesPerCanonicalLine);
    const std::uint32_t firstChunk = static_cast<std::uint32_t>(firstLine)
        / snapshot->chunkSize;
    const std::uint32_t lastChunk = static_cast<std::uint32_t>(lastLine)
        / snapshot->chunkSize;
    for (std::uint32_t chunk = firstChunk; chunk <= lastChunk; ++chunk) {
        if (!m_progressiveDirtyLineChunks.contains(chunk))
            m_progressiveDirtyLineChunks.push_back(chunk);
    }
}

void TrackData::flushProgressiveWaveformLinesLocked()
{
    if (m_progressivePendingLineChunks.isEmpty())
        return;

    auto pending = std::move(m_progressivePendingLineChunks);
    m_progressivePendingLineChunks.clear();
    m_progressiveDirtyLineChunks.clear();
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        const auto snapshot = m_waveformLineStore.snapshot();
        const std::uint32_t chunkIndex = it.key();
        const auto& lines = it.value();
        if (!snapshot || !snapshot->chunks || !lines
            || chunkIndex >= snapshot->chunks->size()) {
            continue;
        }
        const std::uint32_t first = chunkIndex * snapshot->chunkSize;
        const std::uint32_t count = std::min(snapshot->chunkSize,
                                             snapshot->totalLineCount - first);
        const auto published = m_waveformLineStore.publish({
            snapshot->trackGeneration, chunkIndex, first, count,
            snapshot->totalLineCount, lines});
        Q_ASSERT(published != WaveformLineStore::PublishResult::Rejected);
    }
}

void TrackData::stageProgressiveWaveformLinesLocked(
    int firstBin, const QVector<RgbWaveformFrame>& rgb,
    WaveformNormalizationState normalizationState)
{
    if (rgb.isEmpty())
        return;
    const auto snapshot = m_waveformLineStore.snapshot();
    if (!snapshot || !snapshot->chunks || snapshot->chunkSize == 0
        || snapshot->totalLineCount == 0) {
        return;
    }

    const int firstLine = firstBin / kRgbFramesPerCanonicalLine;
    const int lastLine = std::min(static_cast<int>(snapshot->totalLineCount) - 1,
        (firstBin + static_cast<int>(rgb.size()) - 1) / kRgbFramesPerCanonicalLine);
    const std::uint32_t firstChunk = static_cast<std::uint32_t>(firstLine)
        / snapshot->chunkSize;
    const std::uint32_t lastChunk = static_cast<std::uint32_t>(lastLine)
        / snapshot->chunkSize;
    const QVector<PeakFrame> emptyPeaks;

    for (std::uint32_t chunkIndex = firstChunk; chunkIndex <= lastChunk; ++chunkIndex) {
        auto lines = m_progressivePendingLineChunks.value(chunkIndex);
        const std::uint32_t chunkFirst = chunkIndex * snapshot->chunkSize;
        const std::uint32_t chunkCount = std::min(snapshot->chunkSize,
                                                  snapshot->totalLineCount - chunkFirst);
        if (!lines) {
            lines = std::make_shared<std::vector<WaveformLine>>(chunkCount);
            if (const auto previous = snapshot->chunkAt(chunkIndex);
                previous && previous->lines && previous->lines->size() == lines->size()) {
                *lines = *previous->lines;
            }
            m_progressivePendingLineChunks.insert(chunkIndex, lines);
        }

        const int begin = std::max(firstLine, static_cast<int>(chunkFirst));
        const int end = std::min(lastLine + 1, static_cast<int>(chunkFirst + chunkCount));
        for (int globalLine = begin; globalLine < end; ++globalLine) {
            const int sourceLine = globalLine - firstLine;
            (*lines)[static_cast<size_t>(globalLine - static_cast<int>(chunkFirst))]
                = makeCanonicalWaveformLine(rgb, emptyPeaks, sourceLine,
                                            normalizationState);
        }
    }
}

void TrackData::updateProgressiveOverviewFromChunkLocked(
    int firstBin, int totalBins, const QVector<RgbWaveformFrame>& rgb)
{
    if (totalBins <= 0 || rgb.isEmpty())
        return;
    if (m_progressiveOvr.size() != kProgressiveBins) {
        m_progressiveOvr.fill(RgbWaveformFrame{}, kProgressiveBins);
        m_progressiveLastFrame = 0;
    }
    for (int local = 0; local < rgb.size(); ++local) {
        const int global = firstBin + local;
        if (global < 0 || global >= totalBins)
            continue;
        const int bin = static_cast<int>(
            (static_cast<int64_t>(global) * kProgressiveBins) / totalBins);
        if (bin < 0 || bin >= kProgressiveBins)
            continue;
        auto& target = m_progressiveOvr[bin];
        const auto& frame = rgb[local];
        target.rms = std::max(target.rms, frame.rms);
        target.low = std::max(target.low, frame.low);
        target.lowMid = std::max(target.lowMid, frame.lowMid);
        target.mid = std::max(target.mid, frame.mid);
        target.high = std::max(target.high, frame.high);
    }
    m_progressiveLastFrame = std::max(m_progressiveLastFrame,
                                      std::min(totalBins,
                                               firstBin + static_cast<int>(rgb.size())));
}

void TrackData::publishProgressiveWaveformLinesLocked(int firstRgbFrame, int rgbFrameCount)
{
    if (rgbFrameCount <= 0 || m_rgbData.isEmpty()
        || m_progressiveRgbReady.size() != m_rgbData.size()) {
        return;
    }

    const auto initial = m_waveformLineStore.snapshot();
    if (!initial || initial->totalLineCount == 0 || initial->chunkSize == 0)
        return;

    const int firstLine = firstRgbFrame / kRgbFramesPerCanonicalLine;
    const int lastLine = std::min(static_cast<int>(initial->totalLineCount) - 1,
        (firstRgbFrame + rgbFrameCount - 1) / kRgbFramesPerCanonicalLine);
    if (firstLine > lastLine)
        return;

    const std::uint32_t firstChunk = static_cast<std::uint32_t>(firstLine)
        / initial->chunkSize;
    const std::uint32_t lastChunk = static_cast<std::uint32_t>(lastLine)
        / initial->chunkSize;
    const QVector<PeakFrame> emptyPeaks;

    for (std::uint32_t chunkIndex = firstChunk; chunkIndex <= lastChunk; ++chunkIndex) {
        const auto snapshot = m_waveformLineStore.snapshot();
        const std::uint32_t first = chunkIndex * snapshot->chunkSize;
        const std::uint32_t count = std::min(snapshot->chunkSize,
            snapshot->totalLineCount - first);
        auto lines = std::make_shared<std::vector<WaveformLine>>(static_cast<size_t>(count));
        if (const auto previous = snapshot->chunkAt(chunkIndex); previous && previous->lines
            && previous->lines->size() == lines->size()) {
            *lines = *previous->lines;
        }

        const int lineBegin = std::max(firstLine, static_cast<int>(first));
        const int lineEnd = std::min(lastLine + 1, static_cast<int>(first + count));
        for (int lineIndex = lineBegin; lineIndex < lineEnd; ++lineIndex) {
            const int rgbBegin = lineIndex * kRgbFramesPerCanonicalLine;
            const int rgbEnd = std::min(rgbBegin + kRgbFramesPerCanonicalLine,
                                        static_cast<int>(m_progressiveRgbReady.size()));
            const bool ready = std::all_of(m_progressiveRgbReady.cbegin() + rgbBegin,
                                           m_progressiveRgbReady.cbegin() + rgbEnd,
                                           [](quint8 value) { return value != 0; });
            (*lines)[static_cast<size_t>(lineIndex - static_cast<int>(first))]
                = ready ? makeCanonicalWaveformLine(
                              m_rgbData, m_peakMip, lineIndex,
                              WaveformNormalizationState::Preview)
                        : WaveformLine{};
        }

        const auto published = m_waveformLineStore.publish({
            snapshot->trackGeneration, chunkIndex, first, count,
            snapshot->totalLineCount, std::move(lines)});
        Q_ASSERT(published != WaveformLineStore::PublishResult::Rejected);
    }
}

void TrackData::replaceAllData(QVector<WaveformBin>&& finalData, float finalGlobalMaxPeak)
{
    assertOwnerThread();
    {
        QMutexLocker locker(&m_mutex);
        m_waveformSnapshot = std::make_shared<const QVector<WaveformBin>>(std::move(finalData));
        m_data.clear();
        m_globalMaxPeak = finalGlobalMaxPeak;
    }
    emit dataUpdated();
}

void TrackData::reserve(int size)
{
    assertOwnerThread();
    QMutexLocker locker(&m_mutex);
    m_data.reserve(size);
}

void TrackData::flushRgbWaveformEmit()
{
    constexpr qint64 kMinIntervalMs = 90;
    const qint64 since = m_rgbEmitClock.isValid()
        ? m_rgbEmitClock.elapsed() : kMinIntervalMs;

    if (since < kMinIntervalMs) {
        QTimer::singleShot(static_cast<int>(kMinIntervalMs - since), this,
                           [this]() { flushRgbWaveformEmit(); });
        return;
    }

    m_rgbEmitPending = false;
    m_rgbEmitClock.restart();
    emit rgbWaveformUpdated();
}

void TrackData::scheduleRgbWaveformEmit()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "scheduleRgbWaveformEmit", Qt::QueuedConnection);
        return;
    }
    if (m_rgbEmitPending)
        return;
    m_rgbEmitPending = true;
    QMetaObject::invokeMethod(this, "flushRgbWaveformEmit", Qt::QueuedConnection);
}

void TrackData::_updateProgressiveOvr(int from, int to)
{
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

void TrackData::alignSegmentsToBeatgridLocked()
{
    if (m_segments.empty() || m_beatGrid.size() < 2)
        return;

    std::vector<double> anchors;
    anchors.reserve(m_beatGrid.size());
    for (const auto& marker : m_beatGrid) {
        if (marker.isDownbeat || marker.beatInBar == 1)
            anchors.push_back(marker.positionSec);
    }
    if (anchors.size() < 2) {
        anchors.clear();
        anchors.reserve(m_beatGrid.size());
        for (const auto& marker : m_beatGrid)
            anchors.push_back(marker.positionSec);
    }

    auto nearestAnchor = [&](float sec) -> float {
        const double value = static_cast<double>(sec);
        const auto it = std::lower_bound(anchors.begin(), anchors.end(), value);
        if (it == anchors.begin())
            return static_cast<float>(*it);
        if (it == anchors.end())
            return static_cast<float>(anchors.back());
        const auto prev = std::prev(it);
        return static_cast<float>((std::abs(*prev - value) <= std::abs(*it - value)) ? *prev : *it);
    };

    for (auto& segment : m_segments) {
        segment.startTime = nearestAnchor(segment.startTime);
        segment.endTime = std::max(segment.startTime, nearestAnchor(segment.endTime));
    }

    m_segments.erase(std::remove_if(m_segments.begin(), m_segments.end(),
        [](const TrackSegment& segment) {
            return segment.endTime <= segment.startTime + 0.01f;
        }), m_segments.end());
}
