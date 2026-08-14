#pragma once

#include <algorithm>

namespace analysis {

inline double aggregateProgress(int completed, int total, double currentProgress)
{
    if (total <= 0)
        return 0.0;

    const int finished = std::clamp(completed, 0, total);
    const double current = finished < total
        ? std::clamp(currentProgress, 0.0, 1.0)
        : 0.0;
    return std::clamp((static_cast<double>(finished) + current)
                          / static_cast<double>(total),
                      0.0, 1.0);
}

} // namespace analysis
