#pragma once

#include "WaveformRenderTile.h"

#include <QImage>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct WaveformLineStoreSnapshot;

namespace waveform_render {

struct RenderTileKey final {
    std::uint64_t trackGeneration = 0;
    std::uint64_t sourceRevision = 0;
    std::int64_t tileIndex = 0;
    std::uint64_t physicalPixelsPerLineMicros = 0;
    std::uint32_t imageHeight = 0;
    std::uint16_t devicePixelRatioMillis = 1000;
    std::uint8_t lodLevel = 0;
    std::uint32_t visualStyleRevision = 0;
    std::uint32_t backgroundRgba = 0xff101114u;

    bool operator==(const RenderTileKey&) const noexcept = default;
};

struct WaveformViewKey final {
    std::uint64_t trackGeneration = 0;
    std::uint64_t physicalPixelsPerLineMicros = 0;
    std::uint32_t imageHeight = 0;
    std::uint16_t devicePixelRatioMillis = 1000;
    std::uint8_t lodLevel = 0;
    std::uint32_t visualStyleRevision = 0;
    std::uint32_t backgroundRgba = 0xff101114u;

    bool operator==(const WaveformViewKey&) const noexcept = default;
};

[[nodiscard]] inline WaveformViewKey viewKeyFor(
    const RenderTileKey& key) noexcept
{
    return {key.trackGeneration, key.physicalPixelsPerLineMicros,
            key.imageHeight, key.devicePixelRatioMillis, key.lodLevel,
            key.visualStyleRevision, key.backgroundRgba};
}

struct RenderTileKeyHash final {
    [[nodiscard]] std::size_t operator()(const RenderTileKey& key) const noexcept;
};

[[nodiscard]] inline bool currentRenderTileKey(
    const std::optional<RenderTileKey>& displayed,
    const RenderTileKey& required) noexcept
{
    return displayed && *displayed == required;
}

struct RasterizedRenderTile final {
    RenderTileKey key;
    RenderTileSpan span;
    QImage image;
    std::uint64_t renderedColumns = 0;
    bool hasAnySourceData = false;
    bool hasCompleteSourceData = false;
};

struct RenderTileRequest final {
    RenderTileKey key;
    RenderTileSpan span;
    std::shared_ptr<const WaveformLineStoreSnapshot> snapshot;
    double physicalPixelsPerLine = 0.0;
    double logicalHeight = 0.0;
    double devicePixelRatio = 1.0;
    double priority = 0.0;
};

struct OverviewSample final {
    float rms = 0.0f;
    float low = 0.0f;
    float lowMid = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
};

struct OverviewRenderKey final {
    std::uint64_t trackGeneration = 0;
    std::uint64_t sourceRevision = 0;
    std::uint32_t imageWidth = 0;
    std::uint32_t imageHeight = 0;
    std::uint32_t sourceBegin = 0;
    std::uint32_t sourceEnd = 0;
    std::uint32_t totalLineCount = 0;

    bool operator==(const OverviewRenderKey&) const noexcept = default;
};

struct OverviewRenderRequest final {
    OverviewRenderKey key;
    std::shared_ptr<const std::vector<OverviewSample>> samples;
};

struct RasterizedOverview final {
    OverviewRenderKey key;
    QImage image;
};

class WaveformTileRasterizer final
{
public:
    // Per deck. Four active decks therefore cap CPU raster images at 192 MiB;
    // GPU textures and the compact source store are reported separately.
    static constexpr std::size_t kMaximumCacheBytes = 48u * 1024u * 1024u;
    static constexpr std::size_t kMaximumCacheEntries = 64;
    static constexpr std::size_t kMaximumPendingRequests = 64;

    struct Stats final {
        std::uint64_t cacheHits = 0;
        std::uint64_t cacheMisses = 0;
        std::uint64_t rasterizedTiles = 0;
        std::uint64_t discardedTiles = 0;
        std::uint64_t worstRasterUsec = 0;
        std::uint64_t totalRasterUsec = 0;
        std::size_t workerCount = 0;
        std::size_t activeWorkers = 0;
        std::size_t maximumConcurrentWorkers = 0;
        std::size_t cacheBytes = 0;
        std::size_t cacheEntries = 0;
        std::size_t pendingRequests = 0;
    };

    explicit WaveformTileRasterizer(std::function<void()> tileReadyCallback);
    ~WaveformTileRasterizer();

    WaveformTileRasterizer(const WaveformTileRasterizer&) = delete;
    WaveformTileRasterizer& operator=(const WaveformTileRasterizer&) = delete;

    [[nodiscard]] std::shared_ptr<const RasterizedRenderTile>
    find(const RenderTileKey& key);
    void request(RenderTileRequest request);
    [[nodiscard]] std::shared_ptr<const RasterizedOverview>
    findOverview(const OverviewRenderKey& key) const;
    void requestOverview(OverviewRenderRequest request);
    // A zoom gesture supersedes queued work for older scales. Cached tiles are
    // retained so zooming back remains instant; only not-yet-started requests
    // are discarded.
    void cancelPendingTiles();
    void setActiveTrackGeneration(std::uint64_t generation);
    void clear();
    [[nodiscard]] Stats stats() const;
    void resetStats();

    [[nodiscard]] static RenderTileKey makeKey(
        const WaveformLineStoreSnapshot& snapshot,
        std::int64_t tileIndex,
        const RenderTileSpan& span,
        double physicalPixelsPerLine,
        int imageHeight,
        double devicePixelRatio,
        std::uint8_t lodLevel = 0,
        std::uint32_t backgroundRgba = 0xff101114u) noexcept;

private:
    struct CacheEntry final {
        std::shared_ptr<const RasterizedRenderTile> tile;
        std::size_t bytes = 0;
        std::list<RenderTileKey>::iterator lruPosition;
    };

    void run(std::stop_token stopToken);
    void notifyTileReady();
    void insert(std::shared_ptr<const RasterizedRenderTile> tile);
    void evictToBudgetLocked();
    [[nodiscard]] static std::shared_ptr<const RasterizedRenderTile>
    rasterize(const RenderTileRequest& request);
    [[nodiscard]] static std::shared_ptr<const RasterizedOverview>
    rasterizeOverview(const OverviewRenderRequest& request);

    std::function<void()> m_tileReadyCallback;
    mutable std::mutex m_mutex;
    std::condition_variable_any m_condition;
    std::deque<RenderTileRequest> m_pending;
    std::optional<OverviewRenderRequest> m_pendingOverview;
    std::unordered_set<RenderTileKey, RenderTileKeyHash> m_pendingKeys;
    std::unordered_set<RenderTileKey, RenderTileKeyHash> m_inFlightKeys;
    std::optional<OverviewRenderKey> m_overviewInFlightKey;
    std::unordered_map<RenderTileKey, CacheEntry, RenderTileKeyHash> m_cache;
    std::list<RenderTileKey> m_lru;
    std::size_t m_cacheBytes = 0;
    std::uint64_t m_activeTrackGeneration = 0;
    std::shared_ptr<const RasterizedOverview> m_overview;
    std::vector<std::jthread> m_workers;
    std::mutex m_callbackMutex;

    std::atomic<std::uint64_t> m_cacheHits{0};
    std::atomic<std::uint64_t> m_cacheMisses{0};
    std::atomic<std::uint64_t> m_rasterizedTiles{0};
    std::atomic<std::uint64_t> m_discardedTiles{0};
    std::atomic<std::uint64_t> m_worstRasterUsec{0};
    std::atomic<std::uint64_t> m_totalRasterUsec{0};
    std::atomic<std::uint64_t> m_activeWorkers{0};
    std::atomic<std::uint64_t> m_maximumConcurrentWorkers{0};
};

} // namespace waveform_render
