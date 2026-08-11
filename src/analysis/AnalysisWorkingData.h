#pragma once

#include "AnalysisResult.h"

#include <QVector>
#include <algorithm>
#include <functional>
#include <chrono>

namespace analysis {

// Mutable, worker-local analysis state.  It intentionally is not a QObject and
// is never shared with the GUI or render threads.
class AnalysisWorkingData final
{
public:
    using ProgressCallback = std::function<void(double, bool)>;

    explicit AnalysisWorkingData(ProgressCallback progress = {})
        : m_progress(std::move(progress)) {}

    void seed(const AnalysisResult& value)
    {
        m_totalExpected = value.totalExpected;
        m_globalMaxPeak = value.globalMaxPeak;
        if (value.waveform) m_data = *value.waveform;
        if (value.rgbWaveform) m_rgbData = *value.rgbWaveform;
        if (value.overviewWaveform) m_overviewRgb = *value.overviewWaveform;
        if (value.peakMip) m_peakMip = *value.peakMip;
        m_bpm = value.bpm;
        m_firstBeatSample = value.firstBeatSample;
        m_sampleRate = value.sampleRate;
        m_confidence = value.confidence;
        m_beatGridInfo = value.beatGrid;
        m_beatGrid = value.beats;
        m_segments = value.phrases;
        m_detectedKey = value.detectedKey;
    }

    AnalysisResult finish(AnalysisIdentity identity) &&
    {
        AnalysisResult out;
        out.identity = std::move(identity);
        out.analysisVersion = out.identity.analysisVersion;
        out.totalExpected = m_totalExpected;
        out.globalMaxPeak = m_globalMaxPeak;
        out.waveform = std::make_shared<const QVector<TrackData::WaveformBin>>(std::move(m_data));
        out.rgbWaveform = std::make_shared<const QVector<TrackData::RgbWaveformFrame>>(std::move(m_rgbData));
        out.overviewWaveform = std::make_shared<const QVector<TrackData::RgbWaveformFrame>>(std::move(m_overviewRgb));
        out.peakMip = std::make_shared<const QVector<TrackData::PeakFrame>>(std::move(m_peakMip));
        out.preparedWaveformLines = preparedWaveformLines();
        out.bpm = m_bpm;
        out.firstBeatSample = m_firstBeatSample;
        out.sampleRate = m_sampleRate;
        out.confidence = m_confidence;
        out.beatGrid = std::move(m_beatGridInfo);
        out.beats = std::move(m_beatGrid);
        out.phrases = std::move(m_segments);
        out.detectedKey = std::move(m_detectedKey);
        out.complete = true;
        return out;
    }

    void setTotalExpected(int v) { m_totalExpected = v; }
    int getTotalExpected() const { return m_totalExpected; }
    void setGlobalMaxPeak(float v) { m_globalMaxPeak = v; }
    float getGlobalMaxPeak() const { return m_globalMaxPeak; }
    void reserve(int size) { m_data.reserve(size); }
    int size() const { return m_data.size(); }

    void reportAnalysisProgress(double value, bool active)
    {
        m_lastProgress = std::clamp(value, 0.0, 1.0);
        const int pct = static_cast<int>(m_lastProgress * 100.0);
        const auto now = std::chrono::steady_clock::now();
        const bool due = m_lastProgressPct < 0 || pct != m_lastProgressPct
            || now - m_lastProgressPublished >= std::chrono::milliseconds(50);
        if (m_progress && (!active || due)) {
            m_lastProgressPct = pct;
            m_lastProgressPublished = now;
            m_progress(m_lastProgress, active);
        }
    }
    double analysisProgress() const { return m_lastProgress; }

    QVector<TrackData::WaveformBin> getWaveformData() const { return m_data; }
    QVector<TrackData::RgbWaveformFrame> getRgbWaveformData() const { return m_rgbData; }
    QVector<TrackData::RgbWaveformFrame> getOverviewRgbData() const { return m_overviewRgb; }
    QVector<TrackData::PeakFrame> getPeakMipData() const { return m_peakMip; }
    int getRgbWaveformSize() const { return m_rgbData.size(); }

    void clearWaveformData()
    {
        m_data.clear(); m_rgbData.clear(); m_peakMip.clear();
        m_preparedLineChunks.clear();
        m_preparedTotalLines = 0;
        m_preparedReadyLines = 0;
        m_globalMaxPeak = 0.001f;
    }
    void appendData(const QVector<TrackData::WaveformBin>& value) { m_data.append(value); }
    void replaceAllData(QVector<TrackData::WaveformBin>&& value, float peak)
    { m_data = std::move(value); m_globalMaxPeak = peak; }
    void preallocateRgbWaveform(int size) { m_rgbData.fill({}, size); }
    void writeRgbWaveformRange(int from, const QVector<TrackData::RgbWaveformFrame>& value)
    {
        if (from < 0 || from >= m_rgbData.size()) return;
        const int count = std::min(value.size(), m_rgbData.size() - from);
        std::copy_n(value.cbegin(), count, m_rgbData.begin() + from);
    }
    void setRgbWaveformData(QVector<TrackData::RgbWaveformFrame>&& value)
    { m_rgbData = std::move(value); m_overviewRgb = TrackData::downsampleOverview(m_rgbData); }
    void setOverviewRgbData(QVector<TrackData::RgbWaveformFrame>&& value) { m_overviewRgb = std::move(value); }
    void setPeakMipData(QVector<TrackData::PeakFrame>&& value) { m_peakMip = std::move(value); }

    // Long-track analysis writes the canonical immutable store directly. This
    // avoids retaining duration-sized legacy waveform + RGB vectors merely to
    // convert them into WaveformLines after the pass has already completed.
    void initializePreparedWaveformLines(int totalLines)
    {
        if (totalLines <= 0) return;
        m_preparedTotalLines = totalLines;
        m_preparedReadyLines = 0;
        const auto chunkCount = (static_cast<std::uint32_t>(totalLines)
            + WaveformLineStore::kChunkSize - 1)
            / WaveformLineStore::kChunkSize;
        m_preparedLineChunks.assign(chunkCount, nullptr);
    }

    void writePreparedWaveformRange(
        int firstLine, const QVector<TrackData::RgbWaveformFrame>& frames)
    {
        if (firstLine < 0 || frames.isEmpty() || m_preparedTotalLines <= 0)
            return;
        const int count = std::min(
            static_cast<int>(frames.size()), m_preparedTotalLines - firstLine);
        for (int local = 0; local < count; ++local) {
            const auto lineIndex = static_cast<std::uint32_t>(firstLine + local);
            const auto chunkIndex = lineIndex / WaveformLineStore::kChunkSize;
            const auto chunkFirst = chunkIndex * WaveformLineStore::kChunkSize;
            auto& chunk = m_preparedLineChunks[chunkIndex];
            if (!chunk) {
                const auto chunkSize = std::min(
                    WaveformLineStore::kChunkSize,
                    static_cast<std::uint32_t>(m_preparedTotalLines) - chunkFirst);
                chunk = std::make_shared<std::vector<WaveformLine>>(chunkSize);
            }
            auto& line = (*chunk)[lineIndex - chunkFirst];
            if ((line.flags & waveform_line_flags::kAvailable) != 0)
                continue;
            line = waveform::makeCanonicalLine(frames[local]);
            ++m_preparedReadyLines;
        }
    }

    [[nodiscard]] std::shared_ptr<const waveform::PreparedWaveformLines>
    preparedWaveformLines() const
    {
        if (m_preparedTotalLines <= 0
            || m_preparedReadyLines != m_preparedTotalLines
            || m_preparedLineChunks.empty()) {
            return {};
        }
        auto prepared = std::make_shared<waveform::PreparedWaveformLines>();
        prepared->totalLineCount = static_cast<std::uint32_t>(
            m_preparedTotalLines);
        prepared->chunks.reserve(m_preparedLineChunks.size());
        for (const auto& chunk : m_preparedLineChunks) {
            if (!chunk)
                return {};
            prepared->chunks.push_back(chunk);
        }
        return prepared;
    }

    void setBpmData(double bpm, qint64 first, double rate,
                    std::vector<TrackData::BeatMarker> beats = {},
                    TrackData::ConfidenceInfo confidence = {},
                    TrackData::BeatGridInfo info = {})
    {
        m_bpm = bpm; m_firstBeatSample = first; m_sampleRate = rate;
        m_confidence = confidence;
        if (!beats.empty()) m_beatGrid = std::move(beats);
        if (info.type != TrackData::BeatGridType::Unknown || !info.tempoNodes.empty()
            || info.userModified || info.lockedByUser) m_beatGridInfo = std::move(info);
    }
    double getBpm() const { return m_bpm; }
    bool isBpmAnalyzed() const { return m_bpm > 0.0; }
    qint64 getFirstBeatSample() const { return m_firstBeatSample; }
    std::vector<TrackData::BeatMarker> getBeatGrid() const { return m_beatGrid; }
    TrackData::BeatGridInfo getBeatGridInfo() const { return m_beatGridInfo; }
    bool beatgridLockedByUser() const { return m_beatGridInfo.lockedByUser; }
    void setSegmentsData(std::vector<TrackSegment> value) { m_segments = std::move(value); }
    std::vector<TrackSegment> getSegments() const { return m_segments; }
    void setKeyData(const QString& value) { m_detectedKey = value; }

private:
    ProgressCallback m_progress;
    double m_lastProgress = 0.0;
    int m_lastProgressPct = -1;
    std::chrono::steady_clock::time_point m_lastProgressPublished{};
    int m_totalExpected = 0;
    float m_globalMaxPeak = 0.001f;
    QVector<TrackData::WaveformBin> m_data;
    QVector<TrackData::RgbWaveformFrame> m_rgbData;
    QVector<TrackData::RgbWaveformFrame> m_overviewRgb;
    QVector<TrackData::PeakFrame> m_peakMip;
    std::vector<std::shared_ptr<std::vector<WaveformLine>>>
        m_preparedLineChunks;
    int m_preparedTotalLines = 0;
    int m_preparedReadyLines = 0;
    double m_bpm = 0.0;
    qint64 m_firstBeatSample = 0;
    double m_sampleRate = 44100.0;
    TrackData::ConfidenceInfo m_confidence;
    TrackData::BeatGridInfo m_beatGridInfo;
    std::vector<TrackData::BeatMarker> m_beatGrid;
    std::vector<TrackSegment> m_segments;
    QString m_detectedKey;
};

} // namespace analysis
