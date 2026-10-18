#pragma once

#include "domain/TrackData.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace beatgrid {

struct Interval {
    std::size_t index = 0;
    double start = 0.0;
    double duration = 0.5;
};

inline Interval intervalAt(std::span<const TrackData::BeatMarker> grid,
                           double seconds, double fallbackBpm)
{
    const double fallback = std::isfinite(fallbackBpm) && fallbackBpm > 0.0
        ? 60.0 / fallbackBpm : 0.5;
    if (grid.empty())
        return {0, 0.0, fallback};
    const auto after = std::upper_bound(grid.begin(), grid.end(), seconds,
        [](double value, const auto& beat) { return value < beat.positionSec; });
    const auto index = after == grid.begin() ? std::size_t{0}
        : static_cast<std::size_t>(after - grid.begin() - 1);
    double duration = fallback;
    if (index + 1 < grid.size())
        duration = grid[index + 1].positionSec - grid[index].positionSec;
    else if (index > 0)
        duration = grid[index].positionSec - grid[index - 1].positionSec;
    if (!std::isfinite(duration) || duration <= 0.0001)
        duration = fallback;
    return {index, grid[index].positionSec, duration};
}

inline double beatPosition(std::span<const TrackData::BeatMarker> grid,
                           double seconds, double fallbackBpm)
{
    const auto interval = intervalAt(grid, seconds, fallbackBpm);
    return static_cast<double>(interval.index)
        + (seconds - interval.start) / interval.duration;
}

inline double barPhase(std::span<const TrackData::BeatMarker> grid,
                       double seconds, double fallbackBpm)
{
    const auto interval = intervalAt(grid, seconds, fallbackBpm);
    const double beat = grid.empty() ? 0.0 : grid[interval.index].beatInBar - 1.0;
    const double position = beat + (seconds - interval.start) / interval.duration;
    return (position - 4.0 * std::floor(position / 4.0)) / 4.0;
}

} // namespace beatgrid
