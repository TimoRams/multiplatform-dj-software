#pragma once

#include <cmath>

namespace flx10 {
inline bool tempoNearZero(double percent) noexcept
{
    return std::isfinite(percent) && std::abs(percent) <= 0.05;
}
}
