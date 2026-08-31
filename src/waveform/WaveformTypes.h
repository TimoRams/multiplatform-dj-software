#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// Neutral canonical waveform sample. Geometry/dynamics and spectral energy are
// stored independently; renderers decide how bass/mid/treble become colour.
struct WaveformLine final {
    std::int16_t minimum = 0;
    std::int16_t maximum = 0;
    std::uint8_t rms = 0;
    std::uint8_t bass = 0;
    std::uint8_t mid = 0;
    std::uint8_t treble = 0;
    std::uint8_t flags = 0;
    std::uint8_t reserved = 0;
};

namespace waveform_line_flags {
inline constexpr std::uint8_t kAvailable = 1u << 0u;
inline constexpr std::uint8_t kFinal = 1u << 1u;
}

static_assert(sizeof(WaveformLine) == 10);

enum class WaveformChunkState : std::uint8_t {
    Missing,
    Loading,
    PreviewReady,
    FinalReady
};

struct WaveformLineChunk final {
    std::uint64_t trackGeneration = 0;
    std::uint32_t chunkIndex = 0;
    std::uint32_t firstLineIndex = 0;
    std::uint32_t lineCount = 0;
    std::uint32_t totalLineCount = 0;
    std::shared_ptr<const std::vector<WaveformLine>> lines;
    std::uint64_t revision = 0;
    WaveformChunkState state = WaveformChunkState::Loading;

    [[nodiscard]] bool isReady() const noexcept
    {
        return state == WaveformChunkState::PreviewReady
            || state == WaveformChunkState::FinalReady;
    }

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

struct WaveformLineBlock final {
    int firstLine = 0;
    std::shared_ptr<const std::vector<WaveformLine>> lines;
};

using WaveformLineBatch = std::vector<WaveformLineBlock>;

struct WaveformLodBlock final {
    int level = 0;
    int canonicalLineStride = 1;
    int firstSample = 0;
    int totalSamples = 0;
    std::shared_ptr<const std::vector<WaveformLine>> lines;
};

using WaveformLodBatch = std::vector<WaveformLodBlock>;

enum class WaveformNormalizationState : std::uint8_t {
    Preview,
    Final
};
