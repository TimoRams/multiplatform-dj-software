#pragma once

#include <algorithm>
#include <cmath>

namespace waveform_render {

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
