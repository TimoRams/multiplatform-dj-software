#pragma once

#include <algorithm>
#include <cmath>

namespace engine::deck {

constexpr double kJogNudgePercentPerTick = 0.625;
constexpr double kJogNudgeMaxPercent = 6.0;
constexpr double kJogNudgeHoldSeconds = 0.080;
constexpr double kJogNudgeDecayTauSeconds = 0.080;
constexpr double kJogNudgeStopPercent = 0.05;

inline double jogNudgeCommandPercent(double signedTicks) noexcept
{
    return std::clamp(signedTicks * kJogNudgePercentPerTick,
                      -kJogNudgeMaxPercent,
                      kJogNudgeMaxPercent);
}

inline double decayedJogNudgePercent(double commandPercent, double idleSeconds) noexcept
{
    if (idleSeconds <= kJogNudgeHoldSeconds)
        return commandPercent;

    const double percent = commandPercent
        * std::exp(-(idleSeconds - kJogNudgeHoldSeconds) / kJogNudgeDecayTauSeconds);
    return std::abs(percent) < kJogNudgeStopPercent ? 0.0 : percent;
}

} // namespace engine::deck
