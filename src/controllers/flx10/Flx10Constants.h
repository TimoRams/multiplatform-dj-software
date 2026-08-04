#pragma once

namespace flx10 {

// The FLX10's relative CC stream sums to about 12,750 semantic ticks for one
// physical revolution. The legacy Mixxx value of 1,500 made a 360-degree hand
// movement advance BrockDJ's one-to-one platter by roughly 8.5 revolutions.
constexpr double kScratchIntervalsPerRevolution = 12750.0;
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
