#include "WaveformAggregator.h"

#include "WaveformLodPyramid.h"
#include "WaveformVisualStyle.h"

#include <algorithm>
#include <cmath>

namespace waveform {

float WaveformColumn::amplitude() const noexcept
{
    if (!hasData)
        return 0.0f;
    const auto magnitude = std::max(std::abs(static_cast<int>(minimum)),
                                    std::abs(static_cast<int>(maximum)));
    return static_cast<float>(magnitude) / 32767.0f;
}

SourceLineRange sourceLineRangeForColumn(
    std::uint32_t totalLineCount, int index, int columnCount) noexcept
{
    if (totalLineCount == 0 || columnCount <= 0 || index < 0
        || index >= columnCount) {
        return {};
    }
    const long double total = static_cast<long double>(totalLineCount);
    const long double beginExact
        = total * static_cast<long double>(index)
        / static_cast<long double>(columnCount);
    const long double endExact
        = total * static_cast<long double>(index + 1)
        / static_cast<long double>(columnCount);

    const auto begin = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(beginExact), 0u, totalLineCount - 1u);
    // Rounding up the end keeps every column non-empty even when the track is
    // shown at fewer than one source line per column.
    const auto end = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::ceil(endExact)),
        begin + 1u, totalLineCount);
    return {begin, end};
}

WaveformColumn aggregateWaveformColumn(
    const WaveformLineStoreSnapshot& snapshot, SourceLineRange range) noexcept
{
    WaveformColumn column;
    if (!range.valid() || snapshot.totalLineCount == 0 || !snapshot.chunks)
        return column;

    const auto begin = std::min(range.begin, snapshot.totalLineCount);
    const auto end = std::clamp(range.end, begin, snapshot.totalLineCount);
    if (begin >= end)
        return column;

    // Pick the coarsest level whose samples are still finer than the column,
    // so a column never folds more than a couple of samples regardless of how
    // far out the caller is zoomed. This is the only place LOD is decided.
    const auto sourceLinesPerColumn = static_cast<double>(end - begin);
    const auto lodLevel = WaveformLodPyramid::selectLevel(
        sourceLinesPerColumn > 0.0 ? 1.0 / sourceLinesPerColumn : 1.0);
    const auto stride = static_cast<std::uint32_t>(
        WaveformLodPyramid::level(lodLevel).canonicalLineStride);

    const auto lodBegin = begin / stride;
    const auto lodEnd = (end + stride - 1u) / stride;

    std::uint64_t rms = 0;
    std::uint64_t bass = 0;
    std::uint64_t mid = 0;
    std::uint64_t treble = 0;
    std::uint64_t weight = 0;
    bool complete = true;

    for (auto lodIndex = lodBegin; lodIndex < lodEnd; ++lodIndex) {
        const auto sample = WaveformLodPyramid::sample(
            snapshot, lodLevel, lodIndex);
        complete = complete && sample.complete;
        if (!sample.hasData)
            continue;
        const auto& line = sample.line;
        if (!column.hasData) {
            column.minimum = line.minimum;
            column.maximum = line.maximum;
            column.hasData = true;
        } else {
            column.minimum = std::min(column.minimum, line.minimum);
            column.maximum = std::max(column.maximum, line.maximum);
        }
        const auto magnitude = static_cast<std::uint32_t>(std::max(
            std::abs(static_cast<int>(line.minimum)),
            std::abs(static_cast<int>(line.maximum))));
        const auto lineWeight = std::max(1u, magnitude);
        rms += static_cast<std::uint64_t>(line.rms) * lineWeight;
        bass += static_cast<std::uint64_t>(line.bass) * lineWeight;
        mid += static_cast<std::uint64_t>(line.mid) * lineWeight;
        treble += static_cast<std::uint64_t>(line.treble) * lineWeight;
        weight += lineWeight;
    }

    if (!column.hasData || weight == 0) {
        column.hasData = false;
        column.complete = false;
        return column;
    }
    column.rms = static_cast<std::uint8_t>(rms / weight);
    column.bass = static_cast<std::uint8_t>(bass / weight);
    column.mid = static_cast<std::uint8_t>(mid / weight);
    column.treble = static_cast<std::uint8_t>(treble / weight);
    // The controller protocol still consumes packed RGB.  Keep its compatible
    // default interpretation here, but desktop tiles deliberately use the
    // neutral fields above through waveform_visual::map().
    const auto color = waveform_visual::color({
        static_cast<float>(column.bass) / 255.0f,
        static_cast<float>(column.mid) / 255.0f,
        static_cast<float>(column.treble) / 255.0f,
        static_cast<float>(column.rms) / 255.0f});
    column.red = color[0];
    column.green = color[1];
    column.blue = color[2];
    column.complete = complete;
    return column;
}

} // namespace waveform
