#pragma once

#include "domain/TrackData.h"

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

inline std::array<std::uint8_t, 3> lineColor(float low, float lowMid,
                                             float mid, float high, float rms)
{
    const float wLow = std::pow(std::clamp(low, 0.0f, 1.0f), 2.8f);
    const float wLowMid = std::pow(std::clamp(lowMid, 0.0f, 1.0f), 2.5f);
    const float wMid = std::pow(std::clamp(mid, 0.0f, 1.0f), 2.2f);
    const float wHigh = std::pow(std::clamp(high, 0.0f, 1.0f), 1.6f);
    const float sum = wLow + wLowMid + wMid + wHigh;
    if (sum <= 1.0e-7f)
        return {150, 170, 190};

    const float brightness = 0.58f + 0.42f
        * std::pow(std::clamp(rms, 0.0f, 1.0f), 0.35f);
    const float red = ((wLow * 255.0f + wLowMid * 255.0f + wMid * 210.0f) / sum)
        * brightness;
    const float green = ((wLow * 20.0f + wLowMid * 130.0f + wMid * 255.0f
                          + wHigh * 185.0f) / sum) * brightness;
    const float blue = ((wLow * 20.0f + wHigh * 255.0f) / sum) * brightness;
    return {
        static_cast<std::uint8_t>(std::lround(std::clamp(red, 0.0f, 255.0f))),
        static_cast<std::uint8_t>(std::lround(std::clamp(green, 0.0f, 255.0f))),
        static_cast<std::uint8_t>(std::lround(std::clamp(blue, 0.0f, 255.0f)))
    };
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

    WaveformLine line;
    line.minimum = static_cast<std::int16_t>(std::lround(
        std::clamp(minimum, -1.0f, 0.0f) * 32767.0f));
    line.maximum = static_cast<std::int16_t>(std::lround(
        std::clamp(maximum, 0.0f, 1.0f) * 32767.0f));
    const auto color = lineColor(frame.low, frame.lowMid, frame.mid, frame.high, frame.rms);
    line.red = color[0];
    line.green = color[1];
    line.blue = color[2];
    line.flags = 1;
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
