#pragma once

#include "AudioCacheTypes.h"

#include <QString>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace juce { class AudioFormatReader; }

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
    // Permanent, lock-free worker diagnostics.  The values are cumulative from
    // cache construction and make starvation/eviction regressions observable
    // without putting logging or timers in the audio callback.
    std::uint64_t pendingRequests = 0, peakPendingRequests = 0, workerRequests = 0;
    std::uint64_t workerRequestLatencyMicros = 0, worstWorkerRequestLatencyMicros = 0;
    std::uint64_t decodeMicros = 0, worstDecodeMicros = 0;
    std::uint64_t evictionScans = 0, evictionCandidatesVisited = 0;
    std::uint64_t evictionScanMicros = 0, worstEvictionScanMicros = 0;
    std::uint64_t evictionReaderWaitMicros = 0, worstEvictionReaderWaitMicros = 0;
    std::uint64_t priorityPromotions = 0;
};

struct AudioCacheHandleStats {
    std::int64_t residentPages = 0;
    std::int64_t totalPages = 0;
    bool sealed = false;
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
    AudioCacheHandle openTrack(
        const TrackCacheOpenRequest& request,
        std::unique_ptr<juce::AudioFormatReader> preparedReader);
    void releaseTrack(const AudioCacheHandle& handle);
    [[nodiscard]] AudioPageReadGuard tryGetPage(const AudioCacheHandle& handle,
                                                std::int64_t pageIndex) const noexcept;
    bool requestPage(const AudioCacheHandle& handle, std::int64_t pageIndex,
                     AudioCachePriority priority) noexcept;
    bool requestRange(const AudioCacheHandle& handle, std::int64_t firstPage,
                      std::int64_t lastPage, AudioCachePriority priority) noexcept;
    [[nodiscard]] AudioCacheHandleStats handleStats(const AudioCacheHandle& handle) const noexcept;
    // A sealed handle owns every decoded page and no longer depends on its source
    // reader, making it safe to eject removable media. Sealed tracks are bounded
    // to half of the shared cache so active decks retain read-ahead headroom.
    bool sealTrack(const AudioCacheHandle& handle);
    // Blocking consumer-side wait for loader/tests only. The audio callback
    // continues to use tryGetPage() exclusively and never touches a mutex.
    bool waitForPageRange(
        const AudioCacheHandle& handle,
        std::int64_t firstPage,
        std::int64_t lastPage,
        std::chrono::milliseconds timeout,
        const std::function<bool()>& shouldCancel = {}) const;
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
