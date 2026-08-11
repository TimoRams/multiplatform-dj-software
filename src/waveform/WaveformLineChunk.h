#pragma once

#include "WaveformLine.h"

#include <cstdint>
#include <memory>
#include <vector>

struct WaveformLineChunk final {
    std::uint64_t trackGeneration = 0;
    std::uint32_t chunkIndex = 0;
    std::uint32_t firstLineIndex = 0;
    std::uint32_t lineCount = 0;
    std::uint32_t totalLineCount = 0;
    std::shared_ptr<const std::vector<WaveformLine>> lines;
    // Monotonic content identity assigned by WaveformLineStore::publish().
    // Render tiles depend only on revisions of chunks they actually cover,
    // rather than being invalidated when an unrelated timeline chunk arrives.
    std::uint64_t revision = 0;

    [[nodiscard]] bool isWellFormed(std::uint32_t chunkSize) const noexcept
    {
        return trackGeneration != 0 && chunkSize != 0 && lines
            && firstLineIndex == chunkIndex * chunkSize
            && lineCount == lines->size()
            && lineCount != 0
            && firstLineIndex < totalLineCount
            && lineCount <= totalLineCount - firstLineIndex;
    }
};
