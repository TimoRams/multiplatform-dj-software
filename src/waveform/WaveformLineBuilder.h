#pragma once

#include "domain/TrackData.h"
#include "WaveformVisualStyle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace waveform {

struct PreparedWaveformLines final {
    std::uint32_t totalLineCount = 0;
    std::vector<std::shared_ptr<const std::vector<WaveformLine>>> chunks;
};

inline WaveformLine makeCanonicalLine(
    const TrackData::RgbWaveformFrame& frame) noexcept
{
    WaveformLine line;
    line.minimum = static_cast<std::int16_t>(std::lround(
        -std::clamp(frame.rms, 0.0f, 1.0f) * 32767.0f));
    line.maximum = static_cast<std::int16_t>(std::lround(
        std::clamp(frame.rms, 0.0f, 1.0f) * 32767.0f));
    const auto color = waveform_visual::color(
        {frame.low, frame.lowMid, frame.mid, frame.high, frame.rms});
    line.red = color[0];
    line.green = color[1];
    line.blue = color[2];
    line.flags = waveform_line_flags::kAvailable
        | waveform_line_flags::kFinal;
    return line;
}

inline WaveformLine makeCanonicalLine(const QVector<TrackData::RgbWaveformFrame>& rgb,
                                      const QVector<TrackData::PeakFrame>& peaks,
                                      int lineIndex)
{
    const auto& frame = rgb[lineIndex];
    float minimum = -frame.rms;
    float maximum = frame.rms;
    constexpr int peakFramesPerLine = TrackData::PEAK_POINTS_PER_SECOND
        / static_cast<int>(WaveformLineStore::kCanonicalLinesPerSecond);
    const int peakBegin = lineIndex * peakFramesPerLine;
    const int peakEnd = std::min(peakBegin + peakFramesPerLine,
                                 static_cast<int>(peaks.size()));
    for (int index = peakBegin; index < peakEnd; ++index) {
        minimum = std::min(minimum, peaks[index].minSample / 127.0f);
        maximum = std::max(maximum, peaks[index].maxSample / 127.0f);
    }

    WaveformLine line = makeCanonicalLine(frame);
    line.minimum = static_cast<std::int16_t>(std::lround(
        std::clamp(minimum, -1.0f, 0.0f) * 32767.0f));
    line.maximum = static_cast<std::int16_t>(std::lround(
        std::clamp(maximum, 0.0f, 1.0f) * 32767.0f));
    return line;
}

inline std::shared_ptr<const PreparedWaveformLines> prepareWaveformLines(
    const QVector<TrackData::RgbWaveformFrame>& rgb,
    const QVector<TrackData::PeakFrame>& peaks)
{
    if (rgb.isEmpty())
        return {};
    auto prepared = std::make_shared<PreparedWaveformLines>();
    prepared->totalLineCount = static_cast<std::uint32_t>(rgb.size());
    const auto chunkCount = (prepared->totalLineCount + WaveformLineStore::kChunkSize - 1)
        / WaveformLineStore::kChunkSize;
    prepared->chunks.reserve(chunkCount);
    for (std::uint32_t first = 0; first < prepared->totalLineCount;
         first += WaveformLineStore::kChunkSize) {
        const auto count = std::min(WaveformLineStore::kChunkSize,
                                    prepared->totalLineCount - first);
        auto lines = std::make_shared<std::vector<WaveformLine>>(count);
        for (std::uint32_t local = 0; local < count; ++local)
            (*lines)[local] = makeCanonicalLine(rgb, peaks, static_cast<int>(first + local));
        prepared->chunks.push_back(std::move(lines));
    }
    return prepared;
}

} // namespace waveform
