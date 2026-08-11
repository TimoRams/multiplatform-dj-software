#pragma once

#include "WaveformMarkerLayout.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace waveform_render {

// Render tiles live on one track-wide physical-pixel grid. Their texture size
// is deliberately independent of analysis chunk boundaries, zoom and DPR.
inline constexpr std::int64_t kRenderTilePhysicalWidth = 1024;

enum class WaveformCoverage : std::uint8_t {
    Missing,
    Fallback,
    HighResolution
};

inline WaveformCoverage bestAvailableCoverage(bool highResolutionReady,
                                              bool fallbackReady) noexcept
{
    if (highResolutionReady)
        return WaveformCoverage::HighResolution;
    return fallbackReady ? WaveformCoverage::Fallback
                         : WaveformCoverage::Missing;
}

struct RenderTileSpan final {
    std::int64_t tileIndex = 0;
    std::int64_t physicalBegin = 0;
    std::int64_t physicalEnd = 0;
    std::uint32_t sourceBegin = 0;
    std::uint32_t sourceEnd = 0;

    [[nodiscard]] int physicalWidth() const noexcept
    {
        return static_cast<int>(physicalEnd - physicalBegin);
    }

    [[nodiscard]] bool hasSource() const noexcept
    {
        return sourceBegin < sourceEnd;
    }
};

inline std::int64_t floorDiv(std::int64_t numerator,
                             std::int64_t denominator) noexcept
{
    if (denominator <= 0)
        return 0;
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    return remainder < 0 ? quotient - 1 : quotient;
}

inline std::int64_t firstRenderTile(std::int64_t physicalBegin) noexcept
{
    return floorDiv(physicalBegin, kRenderTilePhysicalWidth);
}

inline std::int64_t lastRenderTile(std::int64_t physicalEnd) noexcept
{
    if (physicalEnd <= std::numeric_limits<std::int64_t>::min() + 1)
        return firstRenderTile(physicalEnd);
    return firstRenderTile(physicalEnd - 1);
}

inline RenderTileSpan renderTileSpan(std::int64_t tileIndex,
                                     double physicalPixelsPerLine,
                                     std::uint32_t totalLineCount) noexcept
{
    RenderTileSpan span;
    span.tileIndex = tileIndex;
    span.physicalBegin = tileIndex * kRenderTilePhysicalWidth;
    span.physicalEnd = span.physicalBegin + kRenderTilePhysicalWidth;
    if (!(physicalPixelsPerLine > 0.0) || !std::isfinite(physicalPixelsPerLine)
        || totalLineCount == 0) {
        return span;
    }

    const double first = std::floor(
        static_cast<double>(span.physicalBegin) / physicalPixelsPerLine);
    const double last = std::ceil(
        static_cast<double>(span.physicalEnd) / physicalPixelsPerLine);
    const auto clampLine = [totalLineCount](double line) {
        if (line <= 0.0)
            return std::uint32_t{0};
        if (line >= static_cast<double>(totalLineCount))
            return totalLineCount;
        return static_cast<std::uint32_t>(line);
    };
    span.sourceBegin = clampLine(first);
    span.sourceEnd = clampLine(last);
    return span;
}

inline bool physicalStrokeIntersectsTrack(
    std::int64_t physicalColumn,
    double physicalPixelsPerLine,
    std::uint32_t totalLineCount) noexcept
{
    if (!(physicalPixelsPerLine > 0.0)
        || !std::isfinite(physicalPixelsPerLine)
        || totalLineCount == 0) {
        return false;
    }
    const double strokeBegin = static_cast<double>(physicalColumn);
    const double strokeEnd = strokeBegin
        + static_cast<double>(kWaveformStrokePitchPhysicalPixels);
    const double trackEnd = static_cast<double>(totalLineCount)
        * physicalPixelsPerLine;
    return strokeEnd > 0.0 && strokeBegin < trackEnd;
}

} // namespace waveform_render
