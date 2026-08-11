#pragma once

#include "WaveformLineStore.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace waveform {

class WaveformLodPyramid final
{
public:
    struct Level final {
        std::uint8_t index = 0;
        std::uint8_t canonicalLineStride = 1;
    };

    struct Sample final {
        WaveformLine line;
        bool hasData = false;
        bool complete = false;
    };

    inline static constexpr std::array<Level, 5> kLevels{{
        {0, 1}, {1, 2}, {2, 4}, {3, 8}, {4, 16}
    }};

    [[nodiscard]] static constexpr Level level(std::uint8_t index) noexcept
    {
        return kLevels[index < kLevels.size() ? index : 0];
    }

    [[nodiscard]] static std::uint8_t selectLevel(
        double physicalPixelsPerCanonicalLine) noexcept
    {
        if (!(physicalPixelsPerCanonicalLine > 0.0)
            || !std::isfinite(physicalPixelsPerCanonicalLine)) {
            return 0;
        }
        std::uint8_t selected = 0;
        for (const auto candidate : kLevels) {
            if (physicalPixelsPerCanonicalLine
                    * static_cast<double>(candidate.canonicalLineStride) <= 2.0) {
                selected = candidate.index;
            }
        }
        return selected;
    }

    [[nodiscard]] static std::uint32_t linesPerSecond(
        std::uint32_t canonicalLinesPerSecond,
        std::uint8_t levelIndex) noexcept
    {
        const auto stride = level(levelIndex).canonicalLineStride;
        return std::max(1u, canonicalLinesPerSecond / stride);
    }

    [[nodiscard]] static Sample sample(
        const WaveformLineStoreSnapshot& snapshot,
        std::uint8_t levelIndex,
        std::uint32_t lodSampleIndex) noexcept;

    [[nodiscard]] static std::uint64_t sourceRevision(
        const WaveformLineStoreSnapshot& snapshot,
        std::uint8_t levelIndex,
        std::uint32_t canonicalBegin,
        std::uint32_t canonicalEnd) noexcept;
};

} // namespace waveform
