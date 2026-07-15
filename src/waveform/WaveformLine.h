#pragma once

#include <cstdint>

// Canonical waveform sample: one fixed-duration vertical line.  Eight bytes
// keeps a two-hour 1200-lines/s track at about 66 MiB before chunk-table overhead.
struct WaveformLine final {
    std::int16_t minimum = 0;
    std::int16_t maximum = 0;
    std::uint8_t red = 255;
    std::uint8_t green = 255;
    std::uint8_t blue = 255;
    std::uint8_t flags = 0;
};

static_assert(sizeof(WaveformLine) == 8);
