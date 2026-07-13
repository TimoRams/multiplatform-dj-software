#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

enum class MediaIoPriority : std::uint8_t {
    Interactive = 0,
    TrackLoadSupport,
    CoverArt,
    LibraryScan,
    BackgroundExport,
    Maintenance,
    Count
};

enum class MediaIoRequestType : std::uint8_t {
    ReadCoverArt,
    DecodeCoverThumbnail,
    ValidateTrackPath,
    ReadTrackMetadata,
    ScanDirectory,
    WriteAnalysisArtifact,
    ReadAnalysisArtifact,
    ExportLibraryData,
    CopyFile
};

struct MediaIoRequest {
    MediaIoRequestType type = MediaIoRequestType::ValidateTrackPath;
    MediaIoPriority priority = MediaIoPriority::Interactive;
    std::uint64_t requestId = 0;
    std::uint64_t generation = 0;
    std::uint32_t ownerId = 0;
    QString inputPath;
    QString outputPath;
    QStringList nameFilters;
    QByteArray inputData;
    QString coalescingKey;
    std::size_t maximumBytes = 16 * 1024 * 1024;
    std::size_t maximumEntries = 4096;
    int maximumImageDimension = 1024;
    bool recursive = false;
    bool includeFiles = true;
    bool includeDirectories = false;
    std::shared_ptr<std::atomic_bool> cancellation;
};

struct MediaIoResult {
    MediaIoRequestType type = MediaIoRequestType::ValidateTrackPath;
    std::uint64_t requestId = 0;
    std::uint64_t generation = 0;
    std::uint32_t ownerId = 0;
    bool success = false;
    bool cancelled = false;
    bool stale = false;
    QString error;
    QByteArray data;
    QStringList paths;
    QVariantMap metadata;
    double elapsedMicros = 0.0;
};

struct MediaIoSchedulerStats {
    std::uint64_t queuedRequests = 0;
    std::uint64_t completedRequests = 0;
    std::uint64_t failedRequests = 0;
    std::uint64_t cancelledRequests = 0;
    std::uint64_t staleResults = 0;
    std::uint64_t droppedRequests = 0;
    std::uint64_t coalescedRequests = 0;
    std::uint64_t queueDepth = 0;
    std::uint64_t resultDepth = 0;
    double averageRequestMicros = 0.0;
    double worstRequestMicros = 0.0;
    std::uint64_t workerThreadHash = 0;
};

class MediaIoScheduler final {
public:
    struct Configuration {
        std::size_t queueCapacity = 128;
        std::size_t resultCapacity = 128;
        std::size_t maintenanceFairnessInterval = 32;
    };

    MediaIoScheduler();
    explicit MediaIoScheduler(Configuration configuration);
    ~MediaIoScheduler();
    MediaIoScheduler(const MediaIoScheduler&) = delete;
    MediaIoScheduler& operator=(const MediaIoScheduler&) = delete;

    bool start();
    void requestStop() noexcept;
    void stopAndJoin() noexcept;
    [[nodiscard]] bool enqueue(MediaIoRequest request) noexcept;
    [[nodiscard]] std::vector<MediaIoResult> takeResults(std::size_t maximum = 128);
    [[nodiscard]] std::vector<MediaIoResult> takeResultsForOwner(
        std::uint32_t ownerId, std::size_t maximum = 128);
    void setCurrentGeneration(std::uint64_t generation) noexcept;
    void setCurrentGeneration(std::uint32_t ownerId, std::uint64_t generation) noexcept;
    [[nodiscard]] std::uint64_t currentGeneration() const noexcept;
    [[nodiscard]] std::uint64_t currentGeneration(std::uint32_t ownerId) const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] MediaIoSchedulerStats stats() const noexcept;

private:
    static constexpr std::size_t kPriorityCount =
        static_cast<std::size_t>(MediaIoPriority::Count);

    void workerLoop();
    [[nodiscard]] bool popNextRequest(MediaIoRequest& request);
    [[nodiscard]] MediaIoResult execute(const MediaIoRequest& request);
    void publishResult(MediaIoResult result);
    [[nodiscard]] bool requestIsStale(const MediaIoRequest& request) const noexcept;
    [[nodiscard]] std::size_t queueDepthLocked() const noexcept;
    [[nodiscard]] static std::size_t priorityIndex(MediaIoPriority priority) noexcept;

    Configuration m_configuration;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::array<std::deque<MediaIoRequest>, kPriorityCount> m_queues;
    std::deque<MediaIoResult> m_results;
    std::thread m_thread;
    bool m_started = false;
    bool m_stopRequested = false;
    std::size_t m_requestsSinceMaintenance = 0;

    static constexpr std::size_t kMaximumOwners = 16;
    std::array<std::atomic<std::uint64_t>, kMaximumOwners> m_generations {};
    std::atomic<std::uint64_t> m_queued {0};
    std::atomic<std::uint64_t> m_completed {0};
    std::atomic<std::uint64_t> m_failed {0};
    std::atomic<std::uint64_t> m_cancelled {0};
    std::atomic<std::uint64_t> m_stale {0};
    std::atomic<std::uint64_t> m_dropped {0};
    std::atomic<std::uint64_t> m_coalesced {0};
    std::atomic<std::uint64_t> m_totalMicros {0};
    std::atomic<std::uint64_t> m_worstMicros {0};
    std::atomic<std::uint64_t> m_timedRequests {0};
    std::atomic<std::uint64_t> m_workerThreadHash {0};
};
