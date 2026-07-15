#pragma once

#include "WaveformLineChunk.h"

#include <cstdint>
#include <memory>
#include <vector>

// Immutable-at-the-line-level timeline shared by every waveform renderer.
// Only the control thread mutates the store; renderers hold snapshots.
struct WaveformLineStoreSnapshot final {
    std::uint64_t trackGeneration = 0;
    std::uint64_t dataGeneration = 0;
    std::uint32_t linesPerSecond = 1200;
    std::uint32_t chunkSize = 4096;
    std::uint32_t totalLineCount = 0;
    std::shared_ptr<const std::vector<std::shared_ptr<const WaveformLineChunk>>> chunks;

    [[nodiscard]] std::shared_ptr<const WaveformLineChunk> chunkAt(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t availableChunkCount() const noexcept;
};

class WaveformLineStore final {
public:
    // Keep one canonical vertical line for every 1200 Hz analysis frame.  At the
    // normal DJ zoom this gives a dense, continuous-looking line field; low zoom
    // aggregation happens in the renderer rather than throwing detail away.
    static constexpr std::uint32_t kCanonicalLinesPerSecond = 1200;
    static constexpr std::uint32_t kChunkSize = 4096;

    enum class PublishResult : std::uint8_t { Accepted, Duplicate, Rejected };

    WaveformLineStore() = default;
    void reset(std::uint64_t trackGeneration, std::uint32_t totalLineCount,
               std::uint32_t linesPerSecond = kCanonicalLinesPerSecond,
               std::uint32_t chunkSize = kChunkSize);
    [[nodiscard]] PublishResult publish(WaveformLineChunk chunk);
    [[nodiscard]] std::shared_ptr<const WaveformLineStoreSnapshot> snapshot() const;

private:
    std::shared_ptr<WaveformLineStoreSnapshot> m_snapshot = std::make_shared<WaveformLineStoreSnapshot>();
};
