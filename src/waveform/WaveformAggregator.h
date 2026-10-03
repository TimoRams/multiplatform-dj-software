#pragma once

#include "WaveformLineStore.h"

#include <algorithm>
#include <array>
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

// A half-open range of canonical source lines. This is the only thing a
// consumer ever asks the aggregator about — chunk boundaries, LOD levels and
// cache units are deliberately not expressible here.
struct SourceLineRange final {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    [[nodiscard]] bool valid() const noexcept { return begin < end; }
};

// One visible vertical waveform column, in the shared semantics used by every
// consumer (scrolling deck waveform, overview strip, DDJ-FLX10 PWV5 encoder).
// Values are kept at canonical precision here; quantisation to a display
// format (5-bit height / 3-bit RGB for PWV5, pixel coordinates for the
// desktop) is the consumer's business, never the aggregator's.
struct WaveformColumn final {
    std::int16_t minimum = 0;
    std::int16_t maximum = 0;
    // Neutral, magnitude-weighted values shared by all desktop render styles.
    // These remain at display precision only; canonical audio analysis lives
    // in WaveformLineStore/TrackData.
    std::uint8_t rms = 0;
    std::uint8_t bass = 0;
    std::uint8_t mid = 0;
    std::uint8_t treble = 0;
    // Compatibility output for the external PWV5 display encoder.  New
    // desktop rendering must use the neutral fields above and map a selected
    // WaveformRenderStyle itself.
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    // False when no source line in the range carried data yet.
    bool hasData = false;
    // True only when every source line backing this column was present. A
    // consumer that must not show partially-analysed geometry checks this.
    bool complete = false;

    // Peak magnitude of the column, normalised to 0..1.
    [[nodiscard]] float amplitude() const noexcept;
};

// Maps output column `index` of `columnCount` evenly across the whole track.
// Guarantees a non-empty range for every index (adjacent columns may overlap
// by one line when zoomed out past 1 line per column, which is what keeps a
// column from silently rendering as empty).
[[nodiscard]] SourceLineRange sourceLineRangeForColumn(
    std::uint32_t totalLineCount, int index, int columnCount) noexcept;

// THE aggregation rule. Peak-preserving on amplitude (min/max extrema over the
// range, never averaged, so transients survive) and magnitude-weighted on the
// neutral dynamics/band values (loud lines dominate, so a column reflects what
// you actually hear rather than being washed out by neighbouring silence).
//
// Column consumers leave dynamic LOD selection to the aggregator.
[[nodiscard]] WaveformColumn aggregateWaveformColumn(
    const WaveformLineStoreSnapshot& snapshot, SourceLineRange range) noexcept;

} // namespace waveform
