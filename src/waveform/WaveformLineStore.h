#pragma once

#include "WaveformTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

inline constexpr std::uint32_t kWaveformCanonicalChunkSize = 1024;

struct WaveformLodChunk final {
    std::uint64_t revision = 0;
    std::uint32_t firstSampleIndex = 0;
    std::uint32_t sampleCount = 0;
    std::shared_ptr<const std::vector<WaveformLine>> lines;
};

struct WaveformLodLevelSnapshot final {
    std::uint8_t level = 0;
    std::uint32_t canonicalLineStride = 1;
    std::uint32_t chunkSize = kWaveformCanonicalChunkSize;
    std::uint32_t totalSampleCount = 0;
    std::shared_ptr<const std::vector<std::shared_ptr<const WaveformLodChunk>>> chunks;

    [[nodiscard]] std::shared_ptr<const WaveformLodChunk>
    chunkAt(std::uint32_t index) const noexcept;
};

// Immutable-at-the-line-level timeline shared by every waveform renderer.
// Only the control thread mutates the store; renderers hold snapshots.
struct WaveformLineStoreSnapshot final {
    std::uint64_t trackGeneration = 0;
    std::uint64_t dataGeneration = 0;
    std::uint32_t linesPerSecond = 1200;
    std::uint32_t chunkSize = kWaveformCanonicalChunkSize;
    std::uint32_t totalLineCount = 0;
    std::shared_ptr<const std::vector<std::shared_ptr<const WaveformLineChunk>>> chunks;
    std::array<std::shared_ptr<const WaveformLodLevelSnapshot>, 5> lodLevels{};

    [[nodiscard]] std::shared_ptr<const WaveformLineChunk> chunkAt(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t availableChunkCount() const noexcept;
    [[nodiscard]] std::shared_ptr<const WaveformLodLevelSnapshot>
    lodLevel(std::uint8_t level) const noexcept;
};

class WaveformLineStore final {
public:
    // Keep one canonical vertical line for every 1200 Hz analysis frame.  At the
    // normal DJ zoom this gives a dense, continuous-looking line field; low zoom
    // aggregation happens in the renderer rather than throwing detail away.
    static constexpr std::uint32_t kCanonicalLinesPerSecond = 1200;
    // 1024 lines are about 0.853 seconds at the canonical 1200 Hz rate. This is
    // small enough for seek/scratch demand to become immutable quickly while
    // keeping chunk-table and cache-index overhead bounded for two-hour sets.
    static constexpr std::uint32_t kChunkSize = kWaveformCanonicalChunkSize;

    enum class PublishResult : std::uint8_t { Accepted, Duplicate, Rejected };

    WaveformLineStore() = default;
    void reset(std::uint64_t trackGeneration, std::uint32_t totalLineCount,
               std::uint32_t linesPerSecond = kCanonicalLinesPerSecond,
               std::uint32_t chunkSize = kChunkSize);
    [[nodiscard]] PublishResult publish(WaveformLineChunk chunk);
    [[nodiscard]] PublishResult publishBatch(
        std::vector<WaveformLineChunk> chunks);
    [[nodiscard]] bool publishLodBatch(WaveformLodBatch batch);
    [[nodiscard]] std::shared_ptr<const WaveformLineStoreSnapshot> snapshot() const;

private:
    // Guards m_snapshot only. Publication swaps one shared_ptr, so this is
    // held for microseconds. Readers (notably the Qt render thread, which
    // takes a snapshot every frame) must never have to wait behind the much
    // coarser TrackData mutex, which is also held while progressive chunks are
    // staged and copied — that contention was what made scratching stall the
    // whole UI. Lock order is always TrackData::m_mutex -> this, never the
    // reverse: readers take this one alone.
    mutable std::mutex m_snapshotMutex;
    std::shared_ptr<WaveformLineStoreSnapshot> m_snapshot = std::make_shared<WaveformLineStoreSnapshot>();
};
