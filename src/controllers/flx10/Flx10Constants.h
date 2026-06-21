#pragma once

namespace flx10 {

// FLX10 jog scratch: MIDI tick delta → file seconds (matches Serato-scale feel).
constexpr double kScratchTickToSeconds = 1.0 / 8192.0;

constexpr int kHidPacketSize = 64;

} // namespace flx10
