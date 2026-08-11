#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace waveform_render {

inline constexpr std::int64_t kWaveformStrokePitchPhysicalPixels = 2;

inline bool isWaveformStrokeColumn(std::int64_t globalPhysicalColumn) noexcept
{
    return globalPhysicalColumn % kWaveformStrokePitchPhysicalPixels == 0;
}

inline std::int64_t timelinePhysicalFloor(double linePosition,
                                          double pixelsPerLine,
                                          double devicePixelRatio) noexcept
{
    const double dpr = std::max(1.0, devicePixelRatio);
    return static_cast<std::int64_t>(
        std::floor(linePosition * pixelsPerLine * dpr));
}

inline std::int64_t timelinePhysicalCeil(double linePosition,
                                         double pixelsPerLine,
                                         double devicePixelRatio) noexcept
{
    const double dpr = std::max(1.0, devicePixelRatio);
    return static_cast<std::int64_t>(
        std::ceil(linePosition * pixelsPerLine * dpr));
}

struct VerticalMarkerLayout {
    float waveformInset = 0.0f;
    float regularTickLength = 0.0f;
    float downbeatTickLength = 0.0f;
    float cueLinePhysicalWidth = 3.0f;
};

inline VerticalMarkerLayout verticalMarkerLayout(float height) noexcept
{
    const float h = std::max(0.0f, height);
    const float regular = std::min(
        std::clamp(h * 0.06f, 4.0f, 8.0f), h * 0.18f);
    const float downbeat = std::min(regular + 2.0f, h * 0.24f);
    const float inset = std::min(
        std::max(downbeat + 2.0f, h * 0.075f),
        std::min(14.0f, h * 0.32f));
    return {inset, regular, downbeat, 3.0f};
}

inline double snappedTimelineX(double linePosition,
                               double originLine,
                               double pixelsPerLine,
                               double devicePixelRatio) noexcept
{
    (void)devicePixelRatio;
    return (linePosition - originLine) * pixelsPerLine;
}

inline double pixelAlignedTimelineOrigin(double linePosition,
                                         double pixelsPerLine,
                                         double devicePixelRatio) noexcept
{
    const double dpr = std::max(1.0, devicePixelRatio);
    const double physicalPixelsPerLine = pixelsPerLine * dpr;
    if (physicalPixelsPerLine <= 0.0)
        return linePosition;
    return std::round(linePosition * physicalPixelsPerLine)
        / physicalPixelsPerLine;
}

inline double smoothTimelineTranslation(double width,
                                        double playheadLine,
                                        double originLine,
                                        double pixelsPerLine) noexcept
{
    return width * 0.5 - (playheadLine - originLine) * pixelsPerLine;
}

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

inline bool completeDetailMayCoverOverview(bool keyIsCurrent,
                                           bool hasAnySourceData,
                                           bool hasCompleteSourceData) noexcept
{
    return keyIsCurrent && hasAnySourceData && hasCompleteSourceData;
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

inline double timelinePixelsPerSecond(double pixelsPerPoint,
                                      double waveformPointsPerSecond,
                                      double tempoRatio) noexcept
{
    if (!std::isfinite(pixelsPerPoint)
        || !std::isfinite(waveformPointsPerSecond)
        || !std::isfinite(tempoRatio)) {
        return 0.0;
    }
    return std::max(0.0, pixelsPerPoint)
        * std::max(0.0, waveformPointsPerSecond)
        / std::max(0.05, std::abs(tempoRatio));
}

inline double timelineScreenX(double viewportCenter,
                              double timelineSeconds,
                              double playheadSeconds,
                              double pixelsPerSecond) noexcept
{
    if (!std::isfinite(viewportCenter)
        || !std::isfinite(timelineSeconds)
        || !std::isfinite(playheadSeconds)
        || !std::isfinite(pixelsPerSecond)) {
        return viewportCenter;
    }
    return viewportCenter
        + (timelineSeconds - playheadSeconds) * pixelsPerSecond;
}

inline double screenDeltaToTimelineSeconds(double screenDelta,
                                           double pixelsPerSecond) noexcept
{
    if (!std::isfinite(screenDelta) || !(pixelsPerSecond > 0.0)
        || !std::isfinite(pixelsPerSecond)) {
        return 0.0;
    }
    return screenDelta / pixelsPerSecond;
}

} // namespace waveform_render
