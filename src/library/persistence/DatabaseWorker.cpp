#include "DatabaseWorker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

std::uint64_t currentThreadHash() noexcept
{
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void lowerWorkerPriority() noexcept
{
#if defined(__linux__)
    const auto threadId = static_cast<pid_t>(syscall(SYS_gettid));
    static_cast<void>(setpriority(PRIO_PROCESS, static_cast<id_t>(threadId), 10));
#endif
}

QString escapedSqlitePath(QString path)
{
    path.replace(QLatin1Char('\''), QStringLiteral("''"));
    return path;
}

bool cancelled(const DatabaseCommand& command) noexcept
{
    return command.cancellation
        && command.cancellation->load(std::memory_order_acquire);
}

bool executeStatement(QSqlDatabase& database, const DatabaseStatement& statement,
                      qint64* affected, QString* error)
{
    QSqlQuery query(database);
    query.prepare(statement.sql);
    for (auto it = statement.bindings.cbegin(); it != statement.bindings.cend(); ++it)
        query.bindValue(it.key(), it.value());
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    if (affected)
        *affected += std::max<qint64>(0, query.numRowsAffected());
    return true;
}

} // namespace

DatabaseWorker::DatabaseWorker(Configuration configuration)
    : m_configuration(std::move(configuration))
{
    m_configuration.queueCapacity = std::max<std::size_t>(1, m_configuration.queueCapacity);
    m_configuration.resultCapacity = std::max<std::size_t>(1, m_configuration.resultCapacity);
    m_configuration.maintenanceFairnessInterval =
        std::max<std::size_t>(1, m_configuration.maintenanceFairnessInterval);
}

DatabaseWorker::~DatabaseWorker()
{
    stopAndJoin();
}

bool DatabaseWorker::start()
{
    std::unique_lock lock(m_mutex);
    if (m_started)
        return m_startSucceeded;
    if (m_configuration.databasePath.isEmpty())
        return false;

    m_started = true;
    m_stopRequested = false;
    m_startFinished = false;
    m_startSucceeded = false;
    m_thread = std::thread([this] { workerLoop(); });
    m_startedCondition.wait(lock, [this] { return m_startFinished; });
    return m_startSucceeded;
}

void DatabaseWorker::requestStop() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        m_stopRequested = true;
    }
    m_condition.notify_all();
}

void DatabaseWorker::stopAndJoin() noexcept
{
    requestStop();
    if (m_thread.joinable())
        m_thread.join();
    std::lock_guard lock(m_mutex);
    m_started = false;
}

bool DatabaseWorker::enqueue(DatabaseCommand command) noexcept
{
    try {
        std::lock_guard lock(m_mutex);
        if (!m_started || !m_startSucceeded || m_stopRequested) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (!command.coalescingKey.isEmpty()) {
            for (auto& queue : m_queues) {
                const auto existing = std::find_if(queue.begin(), queue.end(), [&](const auto& item) {
                    return item.coalescingKey == command.coalescingKey;
                });
                if (existing != queue.end()) {
                    *existing = std::move(command);
                    m_coalesced.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
        }

        if (queueDepthLocked() >= m_configuration.queueCapacity) {
            const std::size_t incoming = priorityIndex(command.priority);
            bool madeRoom = false;
            for (std::size_t index = kPriorityCount; index-- > incoming + 1;) {
                if (!m_queues[index].empty()) {
                    const auto evicted = std::move(m_queues[index].back());
                    m_queues[index].pop_back();
                    DatabaseResult dropped;
                    dropped.type = evicted.type;
                    dropped.requestId = evicted.requestId;
                    dropped.generation = evicted.generation;
                    dropped.error = QStringLiteral("command dropped by bounded queue backpressure");
                    if (m_results.size() >= m_configuration.resultCapacity)
                        m_results.pop_front();
                    m_results.push_back(std::move(dropped));
                    m_dropped.fetch_add(1, std::memory_order_relaxed);
                    madeRoom = true;
                    break;
                }
            }
            if (!madeRoom) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        m_queues[priorityIndex(command.priority)].push_back(std::move(command));
        m_queued.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    m_condition.notify_one();
    return true;
}

std::vector<DatabaseResult> DatabaseWorker::takeResults(std::size_t maximum)
{
    std::vector<DatabaseResult> results;
    std::lock_guard lock(m_mutex);
    const std::size_t count = std::min(maximum, m_results.size());
    results.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        results.push_back(std::move(m_results.front()));
        m_results.pop_front();
    }
    return results;
}

void DatabaseWorker::setCurrentGeneration(std::uint64_t generation) noexcept
{
    m_generation.store(generation, std::memory_order_release);
    m_condition.notify_one();
}

std::uint64_t DatabaseWorker::currentGeneration() const noexcept
{
    return m_generation.load(std::memory_order_acquire);
}

bool DatabaseWorker::isRunning() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_started && m_startSucceeded && !m_stopRequested;
}

DatabaseWorkerStats DatabaseWorker::stats() const noexcept
{
    DatabaseWorkerStats value;
    value.queuedCommands = m_queued.load(std::memory_order_relaxed);
    value.completedCommands = m_completed.load(std::memory_order_relaxed);
    value.failedCommands = m_failed.load(std::memory_order_relaxed);
    value.cancelledCommands = m_cancelled.load(std::memory_order_relaxed);
    value.staleResults = m_stale.load(std::memory_order_relaxed);
    value.droppedCommands = m_dropped.load(std::memory_order_relaxed);
    value.coalescedCommands = m_coalesced.load(std::memory_order_relaxed);
    value.backupsCompleted = m_backups.load(std::memory_order_relaxed);
    value.integrityChecks = m_integrityChecks.load(std::memory_order_relaxed);
    const auto timed = m_timedCommands.load(std::memory_order_relaxed);
    value.averageCommandMicros = timed == 0 ? 0.0
        : static_cast<double>(m_totalMicros.load(std::memory_order_relaxed))
              / static_cast<double>(timed);
    value.worstCommandMicros = static_cast<double>(m_worstMicros.load(std::memory_order_relaxed));
    value.workerThreadHash = m_workerThreadHash.load(std::memory_order_relaxed);
    value.connectionOpenedThreadHash = m_openThreadHash.load(std::memory_order_relaxed);
    value.connectionClosedThreadHash = m_closeThreadHash.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(m_mutex);
        value.queueDepth = queueDepthLocked();
        value.resultDepth = m_results.size();
    }
    return value;
}

void DatabaseWorker::workerLoop()
{
    lowerWorkerPriority();
    const auto threadHash = currentThreadHash();
    m_workerThreadHash.store(threadHash, std::memory_order_relaxed);
    const QString connectionName = QStringLiteral("%1_%2")
        .arg(m_configuration.connectionPrefix,
             QUuid::createUuid().toString(QUuid::WithoutBraces));

    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(m_configuration.databasePath);
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        const bool opened = database.open();
        if (opened) {
            m_openThreadHash.store(threadHash, std::memory_order_relaxed);
            QSqlQuery pragma(database);
            pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
        }
        {
            std::lock_guard lock(m_mutex);
            m_startSucceeded = opened;
            m_startFinished = true;
        }
        m_startedCondition.notify_all();

        if (opened) {
            while (true) {
                DatabaseCommand command;
                {
                    std::unique_lock lock(m_mutex);
                    m_condition.wait(lock, [this] {
                        return m_stopRequested || queueDepthLocked() != 0;
                    });
                    if (m_stopRequested && queueDepthLocked() == 0)
                        break;
                    if (!popNextCommand(command))
                        continue;
                }

                if (cancelled(command)) {
                    DatabaseResult result;
                    result.type = command.type;
                    result.requestId = command.requestId;
                    result.generation = command.generation;
                    result.cancelled = true;
                    m_cancelled.fetch_add(1, std::memory_order_relaxed);
                    publishResult(std::move(result));
                    continue;
                }
                if (commandIsStale(command)) {
                    DatabaseResult result;
                    result.type = command.type;
                    result.requestId = command.requestId;
                    result.generation = command.generation;
                    result.stale = true;
                    m_stale.fetch_add(1, std::memory_order_relaxed);
                    publishResult(std::move(result));
                    continue;
                }

                const auto start = std::chrono::steady_clock::now();
                auto result = execute(database, command);
                if (!result.cancelled && commandIsStale(command)) {
                    result.success = false;
                    result.stale = true;
                    result.rows.clear();
                    m_stale.fetch_add(1, std::memory_order_relaxed);
                }
                const auto micros = static_cast<std::uint64_t>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                result.elapsedMicros = static_cast<double>(micros);
                m_totalMicros.fetch_add(micros, std::memory_order_relaxed);
                m_timedCommands.fetch_add(1, std::memory_order_relaxed);
                auto worst = m_worstMicros.load(std::memory_order_relaxed);
                while (micros > worst && !m_worstMicros.compare_exchange_weak(
                           worst, micros, std::memory_order_relaxed)) {
                }
                if (result.stale) {
                    // Accounted above; stale work is neither a SQL failure nor completion.
                } else if (result.success)
                    m_completed.fetch_add(1, std::memory_order_relaxed);
                else if (result.cancelled)
                    m_cancelled.fetch_add(1, std::memory_order_relaxed);
                else
                    m_failed.fetch_add(1, std::memory_order_relaxed);
                publishResult(std::move(result));
            }

            QSqlQuery checkpoint(database);
            checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL)"));
            database.close();
            m_closeThreadHash.store(threadHash, std::memory_order_relaxed);
        }
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

bool DatabaseWorker::popNextCommand(DatabaseCommand& command)
{
    const std::size_t maintenance = priorityIndex(DatabasePriority::Maintenance);
    if (m_commandsSinceMaintenance >= m_configuration.maintenanceFairnessInterval
        && !m_queues[maintenance].empty()) {
        command = std::move(m_queues[maintenance].front());
        m_queues[maintenance].pop_front();
        m_commandsSinceMaintenance = 0;
        return true;
    }
    for (std::size_t index = 0; index < kPriorityCount; ++index) {
        if (m_queues[index].empty())
            continue;
        command = std::move(m_queues[index].front());
        m_queues[index].pop_front();
        if (index == maintenance)
            m_commandsSinceMaintenance = 0;
        else
            ++m_commandsSinceMaintenance;
        return true;
    }
    return false;
}

DatabaseResult DatabaseWorker::execute(QSqlDatabase& database,
                                       const DatabaseCommand& command)
{
    DatabaseResult result;
    result.type = command.type;
    result.requestId = command.requestId;
    result.generation = command.generation;
    if (cancelled(command)) {
        result.cancelled = true;
        return result;
    }

    if (command.type == DatabaseCommandType::RunQuickCheck
        || command.type == DatabaseCommandType::RunFullIntegrityCheck) {
        QSqlQuery query(database);
        const QString pragma = command.type == DatabaseCommandType::RunQuickCheck
            ? QStringLiteral("PRAGMA quick_check")
            : QStringLiteral("PRAGMA integrity_check");
        m_integrityChecks.fetch_add(1, std::memory_order_relaxed);
        if (!query.exec(pragma) || !query.next()) {
            result.error = query.lastError().text();
            return result;
        }
        const QString status = query.value(0).toString();
        result.rows.push_back(QVariantMap{{QStringLiteral("status"), status}});
        result.success = status.compare(QStringLiteral("ok"), Qt::CaseInsensitive) == 0;
        if (!result.success)
            result.error = status;
        return result;
    }

    if (command.type == DatabaseCommandType::Checkpoint) {
        QSqlQuery query(database);
        const QString mode = command.sql.isEmpty() ? QStringLiteral("PASSIVE") : command.sql;
        result.success = query.exec(QStringLiteral("PRAGMA wal_checkpoint(%1)").arg(mode));
        if (!result.success)
            result.error = query.lastError().text();
        return result;
    }

    if (command.type == DatabaseCommandType::CreateBackup) {
        if (command.targetPath.isEmpty()) {
            result.error = QStringLiteral("backup target is empty");
            return result;
        }
        const QFileInfo targetInfo(command.targetPath);
        if (!QDir().mkpath(targetInfo.absolutePath())) {
            result.error = QStringLiteral("cannot create backup directory");
            return result;
        }
        const QString temporary = command.targetPath + QStringLiteral(".tmp.")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QFile::remove(temporary);
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("VACUUM INTO '%1'").arg(escapedSqlitePath(temporary)))) {
            result.error = query.lastError().text();
            QFile::remove(temporary);
            return result;
        }
        if (cancelled(command)) {
            QFile::remove(temporary);
            result.cancelled = true;
            return result;
        }
        const QString previous = command.targetPath + QStringLiteral(".previous");
        QFile::remove(previous);
        const bool hadPrevious = QFile::exists(command.targetPath);
        if (hadPrevious && !QFile::rename(command.targetPath, previous)) {
            result.error = QStringLiteral("cannot stage previous backup target");
            QFile::remove(temporary);
            return result;
        }
        if (!QFile::rename(temporary, command.targetPath)) {
            result.error = QStringLiteral("cannot publish backup target");
            QFile::remove(temporary);
            if (hadPrevious)
                (void)QFile::rename(previous, command.targetPath);
            return result;
        }
        QFile::remove(previous);
        result.success = true;
        m_backups.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    if (command.type == DatabaseCommandType::Batch) {
        if (!database.transaction()) {
            result.error = database.lastError().text();
            return result;
        }
        for (const auto& statement : command.statements) {
            if (cancelled(command)) {
                database.rollback();
                result.cancelled = true;
                return result;
            }
            if (!executeStatement(database, statement, &result.rowsAffected, &result.error)) {
                database.rollback();
                return result;
            }
        }
        if (!database.commit()) {
            result.error = database.lastError().text();
            database.rollback();
            return result;
        }
        result.success = true;
        return result;
    }

    QSqlQuery query(database);
    query.prepare(command.sql);
    for (auto it = command.bindings.cbegin(); it != command.bindings.cend(); ++it)
        query.bindValue(it.key(), it.value());
    if (!query.exec()) {
        result.error = query.lastError().text();
        return result;
    }

    const bool queryCommand = command.type == DatabaseCommandType::Query
        || command.type == DatabaseCommandType::LoadTrack
        || command.type == DatabaseCommandType::LoadLibraryPage;
    if (queryCommand) {
        const auto record = query.record();
        while (query.next()) {
            if (cancelled(command)) {
                result.rows.clear();
                result.cancelled = true;
                return result;
            }
            QVariantMap row;
            for (int column = 0; column < record.count(); ++column)
                row.insert(record.fieldName(column), query.value(column));
            result.rows.push_back(std::move(row));
        }
    } else {
        result.rowsAffected = std::max<qint64>(0, query.numRowsAffected());
    }
    result.success = true;
    return result;
}

void DatabaseWorker::publishResult(DatabaseResult result)
{
    std::lock_guard lock(m_mutex);
    if (m_results.size() >= m_configuration.resultCapacity) {
        m_results.pop_front();
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    m_results.push_back(std::move(result));
}

bool DatabaseWorker::commandIsStale(const DatabaseCommand& command) const noexcept
{
    return command.generation != 0
        && command.generation != m_generation.load(std::memory_order_acquire);
}

std::size_t DatabaseWorker::queueDepthLocked() const noexcept
{
    std::size_t depth = 0;
    for (const auto& queue : m_queues)
        depth += queue.size();
    return depth;
}

std::size_t DatabaseWorker::priorityIndex(DatabasePriority priority) noexcept
{
    const auto index = static_cast<std::size_t>(priority);
    return std::min(index, kPriorityCount - 1);
}
