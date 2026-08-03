#pragma once

namespace flx10 {

// Mixxx's FLX10 mapping uses 1500 scratch intervals per 33 1/3 RPM revolution.
constexpr double kScratchIntervalsPerRevolution = 1500.0;
constexpr double kVinylRpm = 33.0 + 1.0 / 3.0;
constexpr double kJogSpeedWindowSeconds = 0.032;
constexpr double kJogSpeedStaleSeconds = 0.060;
constexpr double kJogTailSuppressionSeconds = 0.120;

constexpr double scratchDeltaSeconds(double ticks) noexcept
{
    return ticks * (60.0 / kVinylRpm) / kScratchIntervalsPerRevolution;
}

constexpr int relativeTicksFromRaw(int rawValue) noexcept
{
    const int raw = rawValue < 0 ? 0 : (rawValue > 127 ? 127 : rawValue);
    return raw - 0x40;
}

constexpr int kHidPacketSize = 64;

} // namespace flx10
