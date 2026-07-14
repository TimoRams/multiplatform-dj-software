#include "TrackData.h"
#include "analysis/AnalysisResult.h"

#include <QMetaObject>
#include <QThread>
#include <algorithm>
#include <cmath>

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
        m_data.clear(); m_rgbData.clear(); m_overviewRgb.clear(); m_peakMip.clear();
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
        if (!beatGrid.empty())
            m_beatGrid = std::move(beatGrid);
        if (beatGridInfo.type != BeatGridType::Unknown || !beatGridInfo.tempoNodes.empty()
            || beatGridInfo.userModified || beatGridInfo.lockedByUser) {
            m_beatGridInfo = std::move(beatGridInfo);
        }
    }
    emit bpmAnalyzed();
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
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
        if (!keepPreview)
            m_totalExpected = 0;
        m_globalMaxPeak = 0.001f;
    }
    if (keepPreview)
        emit overviewRgbUpdated();
    else
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
        m_progressiveOvr.clear();
        m_progressiveLastFrame = 0;
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
    }
    emit rgbWaveformUpdated();
    emit overviewRgbUpdated();
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
                                              const QVector<RgbWaveformFrame>& rgb)
{
    assertOwnerThread();
    if (totalBins <= 0 || firstBin < 0 || (waveform.isEmpty() && rgb.isEmpty()))
        return;
    {
        QMutexLocker locker(&m_mutex);
        if (m_totalExpected != totalBins || m_data.size() != totalBins) {
            m_totalExpected = totalBins;
            m_data.fill(WaveformBin{}, totalBins);
            m_rgbData.fill(RgbWaveformFrame{}, totalBins);
            m_waveformSnapshot.reset();
            m_rgbSnapshot.reset();
        }
        const int waveformCount = std::min(static_cast<int>(waveform.size()), totalBins - firstBin);
        for (int i = 0; i < waveformCount; ++i)
            m_data[firstBin + i] = waveform[i];
        const int rgbCount = std::min(static_cast<int>(rgb.size()), totalBins - firstBin);
        for (int i = 0; i < rgbCount; ++i)
            m_rgbData[firstBin + i] = rgb[i];
        if (rgbCount > 0)
            _updateProgressiveOvr(firstBin, firstBin + rgbCount);
    }
    emit dataUpdated();
    if (!rgb.isEmpty())
        scheduleRgbWaveformEmit();
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
