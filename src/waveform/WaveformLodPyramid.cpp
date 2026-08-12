#include "WaveformLodPyramid.h"

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

    if (levelIndex > 0) {
        const auto persisted = snapshot.lodLevel(levelIndex);
        if (persisted && persisted->canonicalLineStride == stride
            && persisted->chunkSize > 0
            && lodSampleIndex < persisted->totalSampleCount) {
            const auto chunk = persisted->chunkAt(
                lodSampleIndex / persisted->chunkSize);
            if (chunk && chunk->lines
                && lodSampleIndex >= chunk->firstSampleIndex) {
                const auto local = lodSampleIndex - chunk->firstSampleIndex;
                if (local < chunk->lines->size()) {
                    result.line = (*chunk->lines)[local];
                    result.hasData = true;
                    result.complete = true;
                    return result;
                }
            }
        }
    }
    result.complete = true;
    std::uint64_t red = 0;
    std::uint64_t green = 0;
    std::uint64_t blue = 0;
    std::uint64_t weight = 0;
    std::uint8_t flags = 0xff;
    // The whole [begin, end) fold almost always lives inside a single chunk
    // (stride is at most 16, chunks hold 1024 lines). chunkAt() returns a
    // shared_ptr by value, so looking it up per line cost an atomic refcount
    // pair for every source sample — the dominant per-tile overhead now that
    // no persisted LOD level exists and this fallback runs for every sample.
    // Hold the chunk across the fold and only re-resolve on a boundary.
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
        red += static_cast<std::uint64_t>(line.red) * lineWeight;
        green += static_cast<std::uint64_t>(line.green) * lineWeight;
        blue += static_cast<std::uint64_t>(line.blue) * lineWeight;
        weight += lineWeight;
        flags &= line.flags;
    }
    if (weight > 0) {
        result.line.red = static_cast<std::uint8_t>(red / weight);
        result.line.green = static_cast<std::uint8_t>(green / weight);
        result.line.blue = static_cast<std::uint8_t>(blue / weight);
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

    const auto stride = static_cast<std::uint32_t>(
        level(levelIndex).canonicalLineStride);
    if (levelIndex > 0) {
        const auto persisted = snapshot.lodLevel(levelIndex);
        if (persisted && persisted->canonicalLineStride == stride
            && persisted->chunkSize > 0) {
            const auto lodBegin = canonicalBegin / stride;
            const auto lodEnd = (canonicalEnd + stride - 1) / stride;
            const auto firstLodChunk = lodBegin / persisted->chunkSize;
            const auto lastLodChunk = (lodEnd - 1) / persisted->chunkSize;
            bool complete = true;
            std::uint64_t revision = 1469598103934665603ULL
                ^ static_cast<std::uint64_t>(levelIndex);
            for (auto chunkIndex = firstLodChunk;
                 chunkIndex <= lastLodChunk; ++chunkIndex) {
                const auto chunk = persisted->chunkAt(chunkIndex);
                if (!chunk) {
                    complete = false;
                    break;
                }
                revision ^= (static_cast<std::uint64_t>(chunkIndex) << 32U)
                    ^ chunk->revision;
                revision *= 1099511628211ULL;
            }
            if (complete)
                return revision;
        }
    }

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

} // namespace waveform
