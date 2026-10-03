#include "WaveformAggregator.h"

#include "WaveformVisualStyle.h"

#include <algorithm>
#include <cmath>

namespace waveform {

WaveformLodPyramid::Sample WaveformLodPyramid::sample(
    const WaveformLineStoreSnapshot& snapshot,
    std::uint8_t levelIndex,
    std::uint32_t lodSampleIndex) noexcept
{
    Sample result;
    if (!snapshot.chunks || snapshot.chunkSize == 0
        || snapshot.totalLineCount == 0) {
        return result;
    }
    const auto stride = static_cast<std::uint32_t>(
        level(levelIndex).canonicalLineStride);
    const std::uint64_t wideBegin = static_cast<std::uint64_t>(lodSampleIndex)
        * stride;
    if (wideBegin >= snapshot.totalLineCount)
        return result;
    const auto begin = static_cast<std::uint32_t>(wideBegin);
    const auto end = std::min(snapshot.totalLineCount, begin + stride);

    result.complete = true;
    std::uint64_t rms = 0;
    std::uint64_t bass = 0;
    std::uint64_t mid = 0;
    std::uint64_t treble = 0;
    std::uint64_t weight = 0;
    std::uint8_t flags = 0xff;
    // Keep each immutable chunk alive across the fold, avoiding a shared_ptr
    // refcount pair for every source line. Resolve again only at boundaries.
    std::shared_ptr<const WaveformLineChunk> chunk;
    std::uint32_t chunkIndex = 0;
    bool chunkResolved = false;
    for (auto lineIndex = begin; lineIndex < end; ++lineIndex) {
        const auto wantedChunkIndex = lineIndex / snapshot.chunkSize;
        if (!chunkResolved || wantedChunkIndex != chunkIndex) {
            chunk = snapshot.chunkAt(wantedChunkIndex);
            chunkIndex = wantedChunkIndex;
            chunkResolved = true;
        }
        if (!chunk || !chunk->lines || lineIndex < chunk->firstLineIndex) {
            result.complete = false;
            continue;
        }
        const auto local = lineIndex - chunk->firstLineIndex;
        if (local >= chunk->lines->size()) {
            result.complete = false;
            continue;
        }
        const auto& line = (*chunk->lines)[local];
        if ((line.flags & waveform_line_flags::kAvailable) == 0) {
            result.complete = false;
            continue;
        }
        if (!result.hasData) {
            result.line.minimum = line.minimum;
            result.line.maximum = line.maximum;
            result.hasData = true;
        } else {
            result.line.minimum = std::min(result.line.minimum, line.minimum);
            result.line.maximum = std::max(result.line.maximum, line.maximum);
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
        flags &= line.flags;
    }
    if (weight > 0) {
        result.line.rms = static_cast<std::uint8_t>(rms / weight);
        result.line.bass = static_cast<std::uint8_t>(bass / weight);
        result.line.mid = static_cast<std::uint8_t>(mid / weight);
        result.line.treble = static_cast<std::uint8_t>(treble / weight);
        result.line.flags = flags;
    }
    return result;
}

std::uint64_t WaveformLodPyramid::sourceRevision(
    const WaveformLineStoreSnapshot& snapshot,
    std::uint8_t levelIndex,
    std::uint32_t canonicalBegin,
    std::uint32_t canonicalEnd) noexcept
{
    if (!snapshot.chunks || snapshot.chunkSize == 0
        || canonicalBegin >= canonicalEnd) {
        return 0;
    }
    canonicalEnd = std::min(canonicalEnd, snapshot.totalLineCount);
    if (canonicalBegin >= canonicalEnd)
        return 0;

    // FNV-1a over only the immutable canonical chunks intersecting this tile.
    // Missing chunks contribute a stable zero revision; publishing one later
    // changes exactly the tile keys which gain source data.
    std::uint64_t revision = 1469598103934665603ULL;
    const auto firstChunk = canonicalBegin / snapshot.chunkSize;
    const auto lastChunk = (canonicalEnd - 1) / snapshot.chunkSize;
    for (auto chunkIndex = firstChunk; chunkIndex <= lastChunk; ++chunkIndex) {
        const auto chunk = snapshot.chunkAt(chunkIndex);
        const std::uint64_t chunkRevision = chunk ? chunk->revision : 0;
        revision ^= (static_cast<std::uint64_t>(chunkIndex) << 32U)
            ^ chunkRevision;
        revision *= 1099511628211ULL;
    }
    return revision;
}

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
