#pragma once

#include "AudioCacheHandle.h"
#include "AudioPage.h"

#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>

enum class AudioCachePriority : std::uint8_t {
    RealtimeCritical,
    ScratchNearPlayhead,
    PlaybackReadAhead,
    PausedDeck,
    Background,
    Count
};

struct AudioCacheKey {
    QString canonicalPath;
    std::uint64_t fileSize = 0;
    std::int64_t lastModifiedMs = 0;
    friend bool operator==(const AudioCacheKey&, const AudioCacheKey&) = default;
};

struct TrackCacheOpenRequest { QString filePath; };

struct AudioCacheStats {
    std::uint64_t hits = 0, misses = 0, queuedRequests = 0, droppedRequests = 0;
    std::uint64_t decodedPages = 0, decodeFailures = 0, evictedPages = 0;
    std::uint64_t residentBytes = 0, openTracks = 0;
};

class AudioPageReadGuard final
{
public:
    AudioPageReadGuard() = default;
    AudioPageReadGuard(AudioPageReadGuard&& other) noexcept;
    AudioPageReadGuard& operator=(AudioPageReadGuard&& other) noexcept;
    ~AudioPageReadGuard();
    AudioPageReadGuard(const AudioPageReadGuard&) = delete;
    AudioPageReadGuard& operator=(const AudioPageReadGuard&) = delete;
    [[nodiscard]] const AudioPage* get() const noexcept { return m_page; }
    [[nodiscard]] const AudioPage* operator->() const noexcept { return m_page; }
    explicit operator bool() const noexcept { return m_page != nullptr; }

private:
    friend class AudioPageCache;
    AudioPageReadGuard(const AudioPage* page, std::atomic<std::uint32_t>* readers) noexcept
        : m_page(page), m_readers(readers) {}
    void reset() noexcept;
    const AudioPage* m_page = nullptr;
    std::atomic<std::uint32_t>* m_readers = nullptr;
};

class AudioCacheWorker;

class AudioPageCache final
{
public:
    static constexpr std::uint64_t kDefaultBudgetBytes = 256ull * 1024ull * 1024ull;
    static constexpr size_t kRequestQueueCapacity = 1024;

    explicit AudioPageCache(std::uint64_t budgetBytes = kDefaultBudgetBytes);
    ~AudioPageCache();
    AudioPageCache(const AudioPageCache&) = delete;
    AudioPageCache& operator=(const AudioPageCache&) = delete;

    AudioCacheHandle openTrack(const TrackCacheOpenRequest& request);
    void releaseTrack(const AudioCacheHandle& handle);
    [[nodiscard]] AudioPageReadGuard tryGetPage(const AudioCacheHandle& handle,
                                                std::int64_t pageIndex) const noexcept;
    bool requestPage(const AudioCacheHandle& handle, std::int64_t pageIndex,
                     AudioCachePriority priority) noexcept;
    bool requestRange(const AudioCacheHandle& handle, std::int64_t firstPage,
                      std::int64_t lastPage, AudioCachePriority priority) noexcept;
    [[nodiscard]] AudioCacheStats stats() const noexcept;
    [[nodiscard]] std::uint64_t budgetBytes() const noexcept { return m_budgetBytes; }
    void shutdownAndJoin() noexcept;

private:
    friend class AudioCacheWorker;
    struct Impl;
    void workerRun(const std::atomic<bool>& shutdown);
    void notifyWorker() noexcept;
    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<AudioCacheWorker> m_worker;
    const std::uint64_t m_budgetBytes;
};
