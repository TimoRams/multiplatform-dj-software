#pragma once

#include <QString>
#include <QVariantList>
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

enum class DatabasePriority : std::uint8_t {
    UserBlocking = 0,
    Interactive,
    Persistence,
    Background,
    Maintenance,
    Count
};

enum class DatabaseCommandType : std::uint8_t {
    Query,
    Execute,
    Batch,
    LoadTrack,
    UpsertTrack,
    SaveAnalysis,
    SaveCueLoopState,
    LoadLibraryPage,
    UpdatePlayHistory,
    CreateBackup,
    RunQuickCheck,
    RunFullIntegrityCheck,
    Checkpoint
};

struct DatabaseStatement {
    QString sql;
    QVariantMap bindings;
};

struct DatabaseCommand {
    DatabaseCommandType type = DatabaseCommandType::Query;
    DatabasePriority priority = DatabasePriority::Interactive;
    std::uint64_t requestId = 0;
    std::uint64_t generation = 0;
    QString sql;
    QVariantMap bindings;
    std::vector<DatabaseStatement> statements;
    QString targetPath;
    QString coalescingKey;
    std::shared_ptr<std::atomic_bool> cancellation;
};

struct DatabaseResult {
    DatabaseCommandType type = DatabaseCommandType::Query;
    std::uint64_t requestId = 0;
    std::uint64_t generation = 0;
    bool success = false;
    bool cancelled = false;
    bool stale = false;
    QString error;
    QVariantList rows;
    qint64 rowsAffected = 0;
    double elapsedMicros = 0.0;
};

struct DatabaseWorkerStats {
    std::uint64_t queuedCommands = 0;
    std::uint64_t completedCommands = 0;
    std::uint64_t failedCommands = 0;
    std::uint64_t cancelledCommands = 0;
    std::uint64_t staleResults = 0;
    std::uint64_t droppedCommands = 0;
    std::uint64_t coalescedCommands = 0;
    std::uint64_t backupsCompleted = 0;
    std::uint64_t integrityChecks = 0;
    std::uint64_t queueDepth = 0;
    std::uint64_t resultDepth = 0;
    double averageCommandMicros = 0.0;
    double worstCommandMicros = 0.0;
    std::uint64_t workerThreadHash = 0;
    std::uint64_t connectionOpenedThreadHash = 0;
    std::uint64_t connectionClosedThreadHash = 0;
};

class DatabaseWorker final {
public:
    struct Configuration {
        QString databasePath;
        QString connectionPrefix = QStringLiteral("brockdj_database_worker");
        std::size_t queueCapacity = 256;
        std::size_t resultCapacity = 256;
        std::size_t maintenanceFairnessInterval = 32;
    };

    explicit DatabaseWorker(Configuration configuration);
    ~DatabaseWorker();
    DatabaseWorker(const DatabaseWorker&) = delete;
    DatabaseWorker& operator=(const DatabaseWorker&) = delete;

    bool start();
    void requestStop() noexcept;
    void stopAndJoin() noexcept;
    [[nodiscard]] bool enqueue(DatabaseCommand command) noexcept;
    [[nodiscard]] std::vector<DatabaseResult> takeResults(std::size_t maximum = 256);
    void setCurrentGeneration(std::uint64_t generation) noexcept;
    [[nodiscard]] std::uint64_t currentGeneration() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] DatabaseWorkerStats stats() const noexcept;

private:
    static constexpr std::size_t kPriorityCount =
        static_cast<std::size_t>(DatabasePriority::Count);

    void workerLoop();
    [[nodiscard]] bool popNextCommand(DatabaseCommand& command);
    [[nodiscard]] DatabaseResult execute(class QSqlDatabase& database,
                                         const DatabaseCommand& command);
    void publishResult(DatabaseResult result);
    [[nodiscard]] bool commandIsStale(const DatabaseCommand& command) const noexcept;
    [[nodiscard]] std::size_t queueDepthLocked() const noexcept;
    [[nodiscard]] static std::size_t priorityIndex(DatabasePriority priority) noexcept;

    Configuration m_configuration;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::condition_variable m_startedCondition;
    std::array<std::deque<DatabaseCommand>, kPriorityCount> m_queues;
    std::deque<DatabaseResult> m_results;
    std::thread m_thread;
    bool m_started = false;
    bool m_startFinished = false;
    bool m_startSucceeded = false;
    bool m_stopRequested = false;
    std::size_t m_commandsSinceMaintenance = 0;

    std::atomic<std::uint64_t> m_generation {0};
    std::atomic<std::uint64_t> m_queued {0};
    std::atomic<std::uint64_t> m_completed {0};
    std::atomic<std::uint64_t> m_failed {0};
    std::atomic<std::uint64_t> m_cancelled {0};
    std::atomic<std::uint64_t> m_stale {0};
    std::atomic<std::uint64_t> m_dropped {0};
    std::atomic<std::uint64_t> m_coalesced {0};
    std::atomic<std::uint64_t> m_backups {0};
    std::atomic<std::uint64_t> m_integrityChecks {0};
    std::atomic<std::uint64_t> m_totalMicros {0};
    std::atomic<std::uint64_t> m_worstMicros {0};
    std::atomic<std::uint64_t> m_timedCommands {0};
    std::atomic<std::uint64_t> m_workerThreadHash {0};
    std::atomic<std::uint64_t> m_openThreadHash {0};
    std::atomic<std::uint64_t> m_closeThreadHash {0};
};
