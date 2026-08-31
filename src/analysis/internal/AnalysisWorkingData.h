#pragma once

#include "analysis/AnalysisTypes.h"

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
        if (value.spectralWaveform) m_spectralData = *value.spectralWaveform;
        if (value.overviewWaveform) m_overviewData = *value.overviewWaveform;
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
        out.spectralWaveform = std::make_shared<const QVector<TrackData::SpectralWaveformPoint>>(std::move(m_spectralData));
        out.overviewWaveform = std::make_shared<const QVector<TrackData::SpectralWaveformPoint>>(std::move(m_overviewData));
        out.peakMip = std::make_shared<const QVector<TrackData::PeakFrame>>(std::move(m_peakMip));
        out.preparedWaveformLines = preparedWaveformLines();
        if (!out.preparedWaveformLines && out.waveform && out.spectralWaveform)
            out.preparedWaveformLines = waveform::prepareWaveformLines(
                *out.waveform, *out.spectralWaveform);
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
    QVector<TrackData::SpectralWaveformPoint> getSpectralWaveformData() const { return m_spectralData; }
    QVector<TrackData::SpectralWaveformPoint> getOverviewWaveformData() const { return m_overviewData; }
    QVector<TrackData::PeakFrame> getPeakMipData() const { return m_peakMip; }
    int getSpectralWaveformSize() const { return m_spectralData.size(); }

    void clearWaveformData()
    {
        m_data.clear(); m_spectralData.clear(); m_peakMip.clear();
        m_preparedLineChunks.clear();
        m_preparedTotalLines = 0;
        m_preparedReadyLines = 0;
        m_globalMaxPeak = 0.001f;
    }
    void appendData(const QVector<TrackData::WaveformBin>& value) { m_data.append(value); }
    // Parallel envelope segments finish out of order, so the legacy bin vector
    // is sized up front and written by index instead of appended.
    void preallocateWaveform(int size) { m_data.fill({}, size); }
    void writeWaveformRange(int from, const QVector<TrackData::WaveformBin>& value)
    {
        if (from < 0 || from >= m_data.size()) return;
        const int count = std::min(value.size(), m_data.size() - from);
        std::copy_n(value.cbegin(), count, m_data.begin() + from);
    }
    void replaceAllData(QVector<TrackData::WaveformBin>&& value, float peak)
    { m_data = std::move(value); m_globalMaxPeak = peak; }
    void preallocateSpectralWaveform(int size) { m_spectralData.fill({}, size); }
    void writeSpectralWaveformRange(int from, const QVector<TrackData::SpectralWaveformPoint>& value)
    {
        if (from < 0 || from >= m_spectralData.size()) return;
        const int count = std::min(value.size(), m_spectralData.size() - from);
        std::copy_n(value.cbegin(), count, m_spectralData.begin() + from);
    }
    void setSpectralWaveformData(QVector<TrackData::SpectralWaveformPoint>&& value)
    { m_spectralData = std::move(value); m_overviewData = TrackData::downsampleOverview(m_spectralData); }
    void setOverviewWaveformData(QVector<TrackData::SpectralWaveformPoint>&& value) { m_overviewData = std::move(value); }
    void initializeWaveformOverview(int totalSpectralBins)
    {
        m_overviewSourceBins = std::max(0, totalSpectralBins);
        m_overviewData.fill({}, TrackData::kOverviewBins);
    }
    void writeWaveformOverviewRange(
        int firstSpectralBin,
        const QVector<TrackData::SpectralWaveformPoint>& values)
    {
        if (m_overviewSourceBins <= 0 || values.isEmpty())
            return;
        for (int local = 0; local < values.size(); ++local) {
            const int source = firstSpectralBin + local;
            if (source < 0 || source >= m_overviewSourceBins)
                continue;
            const int targetBegin = static_cast<int>(
                (static_cast<std::int64_t>(source) * TrackData::kOverviewBins)
                / m_overviewSourceBins);
            const int targetEnd = std::max(targetBegin + 1, static_cast<int>(
                (static_cast<std::int64_t>(source + 1) * TrackData::kOverviewBins)
                / m_overviewSourceBins));
            const auto& in = values[local];
            for (int target = std::max(0, targetBegin);
                 target < std::min(TrackData::kOverviewBins, targetEnd);
                 ++target) {
                auto& out = m_overviewData[target];
                out.peak = std::max(out.peak, in.peak);
                out.rms = std::max(out.rms, in.rms);
                out.bass = std::max(out.bass, in.bass);
                out.mid = std::max(out.mid, in.mid);
                out.treble = std::max(out.treble, in.treble);
            }
        }
    }
    void setPeakMipData(QVector<TrackData::PeakFrame>&& value) { m_peakMip = std::move(value); }

    // Long-track analysis writes the canonical immutable store directly. This
    // avoids retaining duration-sized geometry and spectral vectors merely to
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
        int firstLine, const QVector<TrackData::WaveformBin>& geometry,
        const QVector<TrackData::SpectralWaveformPoint>& spectral)
    {
        if (firstLine < 0 || geometry.isEmpty() || spectral.isEmpty()
            || m_preparedTotalLines <= 0)
            return;
        const int count = std::min(
            static_cast<int>(geometry.size()), m_preparedTotalLines - firstLine);
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
            line = waveform::makeCanonicalLine(
                geometry[local],
                waveform::interpolateSpectral(
                    spectral, static_cast<std::uint32_t>(local),
                    static_cast<std::uint32_t>(count)));
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
    QVector<TrackData::SpectralWaveformPoint> m_spectralData;
    QVector<TrackData::SpectralWaveformPoint> m_overviewData;
    int m_overviewSourceBins = 0;
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
