#pragma once

#include "domain/TrackData.h"
#include <algorithm>
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
    const TrackData::WaveformBin& geometry,
    const TrackData::SpectralWaveformPoint& spectral,
    WaveformNormalizationState state = WaveformNormalizationState::Final) noexcept
{
    const auto quantizeUnit = [](float value) {
        return static_cast<std::uint8_t>(std::lround(
            std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    WaveformLine line;
    line.minimum = static_cast<std::int16_t>(std::lround(
        std::clamp(geometry.minimum, -1.0f, 0.0f) * 32767.0f));
    line.maximum = static_cast<std::int16_t>(std::lround(
        std::clamp(geometry.maximum, 0.0f, 1.0f) * 32767.0f));
    line.rms = quantizeUnit(geometry.rms);
    line.bass = quantizeUnit(spectral.bass);
    line.mid = quantizeUnit(spectral.mid);
    line.treble = quantizeUnit(spectral.treble);
    line.flags = waveform_line_flags::kAvailable;
    if (state == WaveformNormalizationState::Final)
        line.flags |= waveform_line_flags::kFinal;
    return line;
}

inline TrackData::SpectralWaveformPoint interpolateSpectral(
    const QVector<TrackData::SpectralWaveformPoint>& spectral,
    std::uint32_t geometryIndex,
    std::uint32_t geometryCount) noexcept
{
    if (spectral.isEmpty() || geometryCount == 0)
        return {};
    if (spectral.size() == 1)
        return spectral.front();
    const double position = (static_cast<double>(geometryIndex) + 0.5)
        * static_cast<double>(spectral.size())
        / static_cast<double>(geometryCount) - 0.5;
    const int spectralSize = static_cast<int>(spectral.size());
    const int left = std::clamp(static_cast<int>(std::floor(position)),
                                0, spectralSize - 1);
    const int right = std::min(left + 1, spectralSize - 1);
    const float mix = static_cast<float>(
        std::clamp(position - static_cast<double>(left), 0.0, 1.0));
    const auto lerp = [mix](float a, float b) { return a + (b - a) * mix; };
    TrackData::SpectralWaveformPoint out;
    out.peak = lerp(spectral[left].peak, spectral[right].peak);
    out.rms = lerp(spectral[left].rms, spectral[right].rms);
    out.bass = lerp(spectral[left].bass, spectral[right].bass);
    out.mid = lerp(spectral[left].mid, spectral[right].mid);
    out.treble = lerp(spectral[left].treble, spectral[right].treble);
    return out;
}

inline std::shared_ptr<const PreparedWaveformLines> prepareWaveformLines(
    const QVector<TrackData::WaveformBin>& geometry,
    const QVector<TrackData::SpectralWaveformPoint>& spectral)
{
    if (geometry.isEmpty() || spectral.isEmpty())
        return {};
    auto prepared = std::make_shared<PreparedWaveformLines>();
    prepared->totalLineCount = static_cast<std::uint32_t>(geometry.size());
    const auto chunkCount = (prepared->totalLineCount + WaveformLineStore::kChunkSize - 1)
        / WaveformLineStore::kChunkSize;
    prepared->chunks.reserve(chunkCount);
    for (std::uint32_t first = 0; first < prepared->totalLineCount;
         first += WaveformLineStore::kChunkSize) {
        const auto count = std::min(WaveformLineStore::kChunkSize,
                                    prepared->totalLineCount - first);
        auto lines = std::make_shared<std::vector<WaveformLine>>(count);
        for (std::uint32_t local = 0; local < count; ++local) {
            const auto index = first + local;
            (*lines)[local] = makeCanonicalLine(
                geometry[static_cast<int>(index)],
                interpolateSpectral(spectral, index, prepared->totalLineCount));
        }
        prepared->chunks.push_back(std::move(lines));
    }
    return prepared;
}

} // namespace waveform
