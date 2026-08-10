#pragma once

#include <algorithm>
#include <cmath>

namespace waveform_render {

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
    const double dpr = std::max(1.0, devicePixelRatio);
    return std::round((linePosition - originLine) * pixelsPerLine * dpr) / dpr;
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
