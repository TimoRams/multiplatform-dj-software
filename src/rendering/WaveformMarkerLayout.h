#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace waveform_render {

// Audio is rendered as a stable picket-fence of one-physical-pixel strokes.
// Keeping the cadence in physical pixels makes the apparent density identical
// at every zoom level and on every device pixel ratio.
inline constexpr std::int64_t kWaveformStrokePitchPhysicalPixels = 2;

inline bool isWaveformStrokeColumn(std::int64_t globalPhysicalColumn) noexcept
{
    const auto remainder = globalPhysicalColumn
        % kWaveformStrokePitchPhysicalPixels;
    return remainder == 0;
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
    const float regular = std::min(std::clamp(h * 0.06f, 4.0f, 8.0f), h * 0.18f);
    const float downbeat = std::min(regular + 2.0f, h * 0.24f);
    const float inset = std::min(std::max(downbeat + 2.0f, h * 0.075f),
                                 std::min(14.0f, h * 0.32f));
    return {inset, regular, downbeat, 3.0f};
}

inline double snappedTimelineX(double linePosition,
                               double originLine,
                               double pixelsPerLine,
                               double devicePixelRatio) noexcept
{
    // Do not snap every marker independently. Per-marker rounding changes the
    // distance between adjacent beats as zoom crosses a physical-pixel phase,
    // which looks like gaps or a warped grid. The shared origin keeps floats
    // small and the feathered geometry provides stable subpixel coverage.
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
    return std::round(linePosition * physicalPixelsPerLine) / physicalPixelsPerLine;
}

inline double smoothTimelineTranslation(double width,
                                        double playheadLine,
                                        double originLine,
                                        double pixelsPerLine) noexcept
{
    return width * 0.5 - (playheadLine - originLine) * pixelsPerLine;
}

} // namespace waveform_render
