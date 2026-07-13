#include "LibraryDatabase.h"
#include "LibraryTableModel.h"
#include "app/SettingsManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSqlQuery>
#include <QSqlError>
#include <QTimer>

LibraryDatabase::LibraryDatabase(QObject* parent)
    : QObject(parent)
{
    m_backupSyncTimer.setSingleShot(true);
    m_backupSyncTimer.setInterval(1500);
    m_backupSyncTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_backupSyncTimer, &QTimer::timeout, this, &LibraryDatabase::startDeferredBackupSync);

    m_databaseWorkerResultTimer.setInterval(50);
    m_databaseWorkerResultTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_databaseWorkerResultTimer, &QTimer::timeout,
            this, &LibraryDatabase::collectDatabaseWorkerResults);
}

LibraryDatabase::~LibraryDatabase()
{
    if (!m_shutdownComplete)
        shutdown(false);
}

bool LibraryDatabase::open()
{
    QElapsedTimer timer;
    timer.start();

    // ── Determine the database directory ─────────────────────────────────
    // Use the same config directory as SettingsManager for consistency.
    // This ensures all config files, settings, and database live in the same place.
    QString configDir = SettingsManager::getInstance().getConfigDirectoryPath();
    if (configDir.isEmpty()) {
        qWarning() << "[LibraryDatabase] SettingsManager config directory is empty";
        return false;
    }

    QDir dbDir(configDir + "/db");
    if (!dbDir.exists()) {
        if (!dbDir.mkpath(".")) {
            qWarning() << "[LibraryDatabase] Failed to create db directory:" << dbDir.absolutePath();
            return false;
        }
    }

    const QString legacyDbPath   = dbDir.filePath("RamsbrockDJ_Library.db");
    const QString legacyPrimary  = dbDir.filePath("RamsbrockDJ_Library_A.db");
    const QString legacyBackup   = dbDir.filePath("RamsbrockDJ_Library_B.db");
    m_dbPath = dbDir.filePath("BrockDJ_Library_A.db");
    m_backupDbPath = dbDir.filePath("BrockDJ_Library_B.db");
    m_activeDbPath = m_dbPath;
    m_manualBackupDbPath = dbDir.filePath("BrockDJ_Library_backup_manual.db");

    // One-time migration: copy old RamsbrockDJ databases into new BrockDJ paths
    if (!QFile::exists(m_dbPath) && !QFile::exists(m_backupDbPath)) {
        if (isHealthyDatabaseFile(legacyPrimary)) {
            qWarning() << "[LibraryDatabase] Migrating RamsbrockDJ → BrockDJ (primary)";
            copyDatabaseFile(legacyPrimary, m_dbPath);
            if (isHealthyDatabaseFile(legacyBackup))
                copyDatabaseFile(legacyBackup, m_backupDbPath);
        } else if (isHealthyDatabaseFile(legacyBackup)) {
            qWarning() << "[LibraryDatabase] Migrating RamsbrockDJ → BrockDJ (backup only)";
            copyDatabaseFile(legacyBackup, m_dbPath);
        }
    }

    qDebug() << "[LibraryDatabase] DB primary path:" << m_dbPath;
    qDebug() << "[LibraryDatabase] DB backup path:" << m_backupDbPath;
    qDebug() << "[LibraryDatabase] DB manual backup path:" << m_manualBackupDbPath;

    const bool primaryHealthy = isHealthyDatabaseFile(m_dbPath);
    const bool backupHealthy = isHealthyDatabaseFile(m_backupDbPath);

    if (!primaryHealthy && backupHealthy) {
        qWarning() << "[LibraryDatabase] Primary DB is not healthy, restoring from backup";
        if (!restorePrimaryFromBackup())
            return false;
    } else if (!primaryHealthy && !backupHealthy && isHealthyDatabaseFile(legacyDbPath)) {
        qWarning() << "[LibraryDatabase] Migrating legacy DB into mirrored layout";
        if (!copyDatabaseFile(legacyDbPath, m_dbPath))
            return false;
    } else if (primaryHealthy && !backupHealthy) {
        qWarning() << "[LibraryDatabase] Backup DB is not healthy, refreshing from primary";
        if (!copyDatabaseFile(m_dbPath, m_backupDbPath))
            qWarning() << "[LibraryDatabase] Failed to refresh backup DB from primary";
    } else if (!primaryHealthy && !backupHealthy) {
        qWarning() << "[LibraryDatabase] Neither DB copy is healthy, creating a fresh database";
        m_lastRecoveryEvent = QStringLiteral("Both databases missing/corrupted — fresh database created");
    }

    // ── Open via QSqlDatabase ────────────────────────────────────────────
    clearDatabaseConnection();
    m_db = QSqlDatabase::addDatabase("QSQLITE", "library_conn");
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        qWarning() << "[LibraryDatabase] Failed to open:" << m_db.lastError().text();
        return false;
    }
    qDebug() << "[LibraryDatabase] open(): connection established in" << timer.elapsed() << "ms";

    // Enable WAL mode for better concurrency and foreign keys.
    {
        QSqlQuery pragma(m_db);
        pragma.exec("PRAGMA journal_mode=WAL");
        pragma.exec("PRAGMA foreign_keys=ON");
    }

    if (!createSchema())
        return false;
    qDebug() << "[LibraryDatabase] open(): schema ready in" << timer.elapsed() << "ms";

    assessPreviousSessionRecovery();

    DatabaseWorker::Configuration workerConfiguration;
    workerConfiguration.databasePath = m_activeDbPath;
    workerConfiguration.connectionPrefix = QStringLiteral("library_worker");
    m_databaseWorker = std::make_unique<DatabaseWorker>(std::move(workerConfiguration));
    if (!m_databaseWorker->start()) {
        qWarning() << "[LibraryDatabase] Failed to start database worker";
        m_databaseWorker.reset();
        return false;
    }
    m_databaseWorkerResultTimer.start();
    requestQuickCheck();
    startDeferredBackupSync();

    if (!primaryHealthy && backupHealthy)
        m_primaryMirrorDegraded = true;
    if (primaryHealthy && !backupHealthy)
        m_backupMirrorDegraded = true;
    if (!primaryHealthy && !backupHealthy) {
        m_primaryMirrorDegraded = true;
        m_backupMirrorDegraded = true;
    }

    m_cachedMirrorStatus = mirroredDatabaseStatus();
    emit mirroredDatabaseStatusChanged();
    qDebug() << "[LibraryDatabase] open(): finished in" << timer.elapsed() << "ms";

    return true;
}
QString LibraryDatabase::mirroredDatabaseStatus() const
{
    if (m_lastRecoveryEvent.isEmpty())
        return QStringLiteral("DB A: OK | DB B: OK");

    const auto describe = [this](const QString& path, bool degraded) -> QString {
        if (path.isEmpty())
            return QStringLiteral("unknown");

        const QFileInfo info(path);
        if (!info.exists())
            return QStringLiteral("missing");
        if (info.size() <= 0)
            return QStringLiteral("empty");

        if (degraded)
            return QStringLiteral("degraded");

        return isHealthyDatabaseFile(path) ? QStringLiteral("gut") : QStringLiteral("beschädigt");
    };

    const QString activeLabel = (m_activeDbPath == m_backupDbPath) ? QStringLiteral("B") : QStringLiteral("A");

    return QStringLiteral("DB A: %1 | DB B: %2 | Aktiv: %3 | Recovery: %4")
        .arg(describe(m_dbPath, m_primaryMirrorDegraded),
             describe(m_backupDbPath, m_backupMirrorDegraded),
             activeLabel,
             m_lastRecoveryEvent);
}

QString LibraryDatabase::recoveryWarningMessage() const
{
    if (!m_recoveryWarningNeeded)
        return {};

    return QStringLiteral(
        "BrockDJ was interrupted while the library database was being updated. "
        "Your library was reopened from the mirrored copy — please check recent "
        "changes if anything looks off.");
}

void LibraryDatabase::assessPreviousSessionRecovery()
{
    // Persisted across runs: was the last session closed cleanly and were there
    // any library writes while it was open?
    const QString prevClosedCleanly = getSetting(QStringLiteral("session_closed_cleanly"),
                                                 QStringLiteral("1"));
    const QString prevDirty         = getSetting(QStringLiteral("session_dirty"),
                                                 QStringLiteral("0"));

    const bool uncleanExit      = (prevClosedCleanly != QStringLiteral("1"));
    const bool hadPendingWrites = (prevDirty == QStringLiteral("1"));

    // Only warn when an abrupt exit could have left an in-flight write unfinished.
    // A Ctrl+C exit with no library mutations is not a recovery event.
    m_recoveryWarningNeeded = uncleanExit && hadPendingWrites;

    // Mark this session as open; cleared again in shutdown().
    setSetting(QStringLiteral("session_closed_cleanly"), QStringLiteral("0"));
    setSetting(QStringLiteral("session_dirty"), QStringLiteral("0"));
    m_sessionDirty = false;

    if (m_recoveryWarningNeeded)
        emit recoveryWarningNeededChanged();
}

void LibraryDatabase::markSessionDirty()
{
    if (m_sessionDirty || !m_db.isOpen())
        return;

    m_sessionDirty = true;
    setSetting(QStringLiteral("session_dirty"), QStringLiteral("1"));
}

void LibraryDatabase::shutdown(bool syncBackup)
{
    if (m_shutdownComplete)
        return;

    if (!m_db.isValid() || !m_db.isOpen()) {
        m_shutdownComplete = true;
        return;
    }

    m_shutdownComplete = true;

    QCoreApplication::removePostedEvents(this);

    if (m_tableModel)
        QObject::disconnect(this, nullptr, m_tableModel, nullptr);

    const bool pendingBackupSync = m_backupSyncTimer.isActive() || m_backupSyncRunning;
    QObject::disconnect(&m_backupSyncTimer, nullptr, this, nullptr);
    m_backupSyncTimer.stop();
    m_databaseWorkerResultTimer.stop();

    {
        QSqlQuery q(m_db);
        q.exec("PRAGMA wal_checkpoint(FULL)");
    }

    setSetting(QStringLiteral("session_dirty"), QStringLiteral("0"));
    setSetting(QStringLiteral("session_closed_cleanly"), QStringLiteral("1"));
    m_sessionDirty = false;

    clearDatabaseConnection();

    if (m_databaseWorker) {
        if (syncBackup || pendingBackupSync)
            startDeferredBackupSync();
        if (syncBackup && !m_manualBackupDbPath.isEmpty()) {
            DatabaseCommand manual;
            manual.type = DatabaseCommandType::CreateBackup;
            manual.priority = DatabasePriority::Background;
            manual.requestId = m_nextDatabaseRequestId++;
            manual.targetPath = m_manualBackupDbPath;
            manual.coalescingKey = QStringLiteral("manual-backup");
            (void)m_databaseWorker->enqueue(std::move(manual));
        }
        m_databaseWorker->requestStop();
        m_databaseWorker->stopAndJoin();
        collectDatabaseWorkerResults();
        m_databaseWorker.reset();
    }
}

void LibraryDatabase::setTableModel(LibraryTableModel* model)
{
    m_tableModel = model;
    if (m_tableModel)
        m_tableModel->setDatabase(this);
}

void LibraryDatabase::scheduleTableModelRefresh()
{
    if (m_shutdownComplete || m_tableModel == nullptr || m_tableModelRefreshPending)
        return;

    m_tableModelRefreshPending = true;

    // Use QueuedConnection (not a native timer) to avoid macOS CFRunLoop re-entrancy
    // that can destroy ListView delegates while signal handlers are still on the stack.
    QMetaObject::invokeMethod(this, [this]() {
        m_tableModelRefreshPending = false;

        if (m_tableModel != nullptr)
            m_tableModel->refresh();
    }, Qt::QueuedConnection);
}

void LibraryDatabase::scheduleBackupSync()
{
    if (!m_db.isValid() || !m_db.isOpen())
        return;

    markSessionDirty();
    m_backupSyncTimer.start();
}

void LibraryDatabase::startDeferredBackupSync()
{
    if (!m_databaseWorker || !m_databaseWorker->isRunning())
        return;
    const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    const QString mirrorPath = (activePath == m_dbPath) ? m_backupDbPath : m_dbPath;
    if (activePath.isEmpty() || mirrorPath.isEmpty())
        return;

    DatabaseCommand backup;
    backup.type = DatabaseCommandType::CreateBackup;
    backup.priority = DatabasePriority::Background;
    backup.requestId = m_nextDatabaseRequestId++;
    backup.targetPath = mirrorPath;
    backup.coalescingKey = QStringLiteral("mirror-backup");
    if (m_databaseWorker->enqueue(std::move(backup))) {
        m_backupRequestId = m_nextDatabaseRequestId - 1;
        m_backupSyncRunning = true;
    }
}

void LibraryDatabase::requestQuickCheck()
{
    if (!m_databaseWorker || !m_databaseWorker->isRunning())
        return;
    DatabaseCommand command;
    command.type = DatabaseCommandType::RunQuickCheck;
    command.priority = DatabasePriority::Maintenance;
    command.requestId = m_nextDatabaseRequestId++;
    command.coalescingKey = QStringLiteral("quick-check");
    if (m_databaseWorker->enqueue(std::move(command)))
        m_quickCheckRequestId = m_nextDatabaseRequestId - 1;
}

void LibraryDatabase::requestFullIntegrityCheck()
{
    if (!m_databaseWorker || !m_databaseWorker->isRunning())
        return;
    DatabaseCommand command;
    command.type = DatabaseCommandType::RunFullIntegrityCheck;
    command.priority = DatabasePriority::Maintenance;
    command.requestId = m_nextDatabaseRequestId++;
    command.coalescingKey = QStringLiteral("full-integrity-check");
    if (m_databaseWorker->enqueue(std::move(command)))
        m_fullCheckRequestId = m_nextDatabaseRequestId - 1;
}

DatabaseWorkerStats LibraryDatabase::databaseWorkerStats() const noexcept
{
    return m_databaseWorker ? m_databaseWorker->stats() : DatabaseWorkerStats{};
}

bool LibraryDatabase::requestLibraryPage(QString sql, QVariantMap bindings,
                                         std::uint64_t generation)
{
    if (!m_databaseWorker || !m_databaseWorker->isRunning())
        return false;
    DatabaseCommand command;
    command.type = DatabaseCommandType::LoadLibraryPage;
    command.priority = DatabasePriority::Interactive;
    command.requestId = m_nextDatabaseRequestId++;
    command.generation = generation;
    command.sql = std::move(sql);
    command.bindings = std::move(bindings);
    command.coalescingKey = QStringLiteral("library-model-page");
    m_databaseWorker->setCurrentGeneration(generation);
    return m_databaseWorker->enqueue(std::move(command));
}

void LibraryDatabase::collectDatabaseWorkerResults()
{
    if (!m_databaseWorker)
        return;
    for (const auto& result : m_databaseWorker->takeResults()) {
        if (result.type == DatabaseCommandType::LoadLibraryPage) {
            emit libraryPageReady(result.generation, result.rows,
                                  result.success ? QString{} : result.error);
        } else if (result.requestId == m_backupRequestId) {
            m_backupSyncRunning = false;
            if (!result.success)
                qWarning() << "[LibraryDatabase] Database worker backup failed:" << result.error;
            emit mirroredDatabaseStatusChanged();
        } else if (result.requestId == m_quickCheckRequestId
                   || result.requestId == m_fullCheckRequestId) {
            if (!result.success) {
                m_lastRecoveryEvent = QStringLiteral("database check failed: %1").arg(result.error);
                m_primaryMirrorDegraded = (m_activeDbPath == m_dbPath);
                m_backupMirrorDegraded = (m_activeDbPath == m_backupDbPath);
                emit mirroredDatabaseStatusChanged();
            }
        }
    }
}
// ── Generic settings ──────────────────────────────────────────────────────────

QString LibraryDatabase::getSetting(const QString& key, const QString& defaultValue) const
{
    if (!m_db.isOpen() || key.isEmpty())
        return defaultValue;

    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM Meta WHERE key = :key LIMIT 1");
    q.bindValue(":key", key);
    if (!q.exec() || !q.next())
        return defaultValue;
    const QString v = q.value(0).toString();
    return v.isNull() ? defaultValue : v;
}

bool LibraryDatabase::setSetting(const QString& key, const QString& value)
{
    if (!m_db.isOpen() || key.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO Meta (key, value) VALUES (:key, :value)");
    q.bindValue(":key",   key);
    q.bindValue(":value", value);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] setSetting:" << q.lastError().text();
        return false;
    }
    return true;
}
// ─────────────────────────────────────────────────────────────────────────────

bool LibraryDatabase::isHealthyDatabaseFile(const QString& path) const
{
    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() || fileInfo.size() < 16)
        return false;
    QFile file(path);
    return file.open(QIODevice::ReadOnly)
        && file.read(16) == QByteArrayLiteral("SQLite format 3\0");
}

bool LibraryDatabase::copyDatabaseFile(const QString& sourcePath, const QString& targetPath) const
{
    if (sourcePath.isEmpty() || targetPath.isEmpty() || sourcePath == targetPath)
        return false;

    QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || sourceInfo.size() <= 0)
        return false;

    QFile::remove(targetPath);
    QFile::remove(targetPath + QStringLiteral("-wal"));
    QFile::remove(targetPath + QStringLiteral("-shm"));

    if (!QFile::copy(sourcePath, targetPath)) {
        qWarning() << "[LibraryDatabase] Failed to copy DB file from" << sourcePath << "to" << targetPath;
        return false;
    }

    return true;
}

bool LibraryDatabase::restorePrimaryFromBackup()
{
    if (!isHealthyDatabaseFile(m_backupDbPath))
        return false;

    return copyDatabaseFile(m_backupDbPath, m_dbPath);
}

void LibraryDatabase::clearDatabaseConnection()
{
    if (!m_db.isValid())
        return;

    const QString connectionName = m_db.connectionName();
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();

    if (!connectionName.isEmpty())
        QSqlDatabase::removeDatabase(connectionName);
}

void LibraryDatabase::performMirrorSelfCheck()
{
    // Error paths request a cheap worker-side diagnostic. Full integrity checks
    // are deliberately exposed only through requestFullIntegrityCheck().
    requestQuickCheck();
}
