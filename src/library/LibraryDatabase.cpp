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
#include <QPointer>
#include <QSqlQuery>
#include <QSqlError>
#include <QTimer>
#include <QUuid>
#include <thread>

namespace {

bool syncBackupFromPath(const QString& activePath, const QString& mirrorPath)
{
    if (activePath.isEmpty() || mirrorPath.isEmpty())
        return false;

    const QString connectionName =
        QStringLiteral("library_backup_sync_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(activePath);

        if (db.open()) {
            QSqlQuery q(db);
            q.exec(QStringLiteral("PRAGMA wal_checkpoint(PASSIVE)"));

            QFile::remove(mirrorPath);
            QFile::remove(mirrorPath + QStringLiteral("-wal"));
            QFile::remove(mirrorPath + QStringLiteral("-shm"));

            QString vacuumTarget = mirrorPath;
            vacuumTarget.replace(QLatin1Char('\''), QStringLiteral("''"));
            ok = q.exec(QStringLiteral("VACUUM INTO '%1'").arg(vacuumTarget));
            if (!ok)
                qWarning() << "[LibraryDatabase] Deferred VACUUM INTO failed for"
                           << mirrorPath << ':' << q.lastError().text();
        } else {
            qWarning() << "[LibraryDatabase] Deferred backup DB open failed:" << db.lastError().text();
        }

        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

} // namespace

LibraryDatabase::LibraryDatabase(QObject* parent)
    : QObject(parent)
{
    m_mirrorSelfCheckTimer.setInterval(3000);
    m_mirrorSelfCheckTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_mirrorSelfCheckTimer, &QTimer::timeout, this, &LibraryDatabase::performMirrorSelfCheck);

    m_backupSyncTimer.setSingleShot(true);
    m_backupSyncTimer.setInterval(1500);
    m_backupSyncTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_backupSyncTimer, &QTimer::timeout, this, &LibraryDatabase::startDeferredBackupSync);
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

    if (!syncBackupFromPrimary())
        qWarning() << "[LibraryDatabase] Failed to sync backup DB after open";

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
    if (!m_mirrorSelfCheckTimer.isActive())
        m_mirrorSelfCheckTimer.start();

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

    const bool pendingBackupSync = m_backupSyncTimer.isActive() || m_backupSyncAgain;
    QObject::disconnect(&m_mirrorSelfCheckTimer, nullptr, this, nullptr);
    QObject::disconnect(&m_backupSyncTimer, nullptr, this, nullptr);

    {
        QSqlQuery q(m_db);
        q.exec("PRAGMA wal_checkpoint(FULL)");
    }

    setSetting(QStringLiteral("session_dirty"), QStringLiteral("0"));
    setSetting(QStringLiteral("session_closed_cleanly"), QStringLiteral("1"));
    m_sessionDirty = false;

    if ((syncBackup || pendingBackupSync) && !m_backupSyncRunning) {
        if (!syncBackupFromPrimary())
            qWarning() << "[LibraryDatabase] Failed to sync backup DB during shutdown";
    }

    if (syncBackup) {
        const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
        if (!m_manualBackupDbPath.isEmpty() && !copyDatabaseFile(activePath, m_manualBackupDbPath))
            qWarning() << "[LibraryDatabase] Failed to write manual backup DB during shutdown";
    }

    clearDatabaseConnection();
}

void LibraryDatabase::setTableModel(LibraryTableModel* model)
{
    m_tableModel = model;
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
    if (!m_db.isValid() || !m_db.isOpen())
        return;

    if (m_backupSyncRunning) {
        m_backupSyncAgain = true;
        return;
    }

    const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    const QString mirrorPath = (activePath == m_dbPath) ? m_backupDbPath : m_dbPath;
    if (activePath.isEmpty() || mirrorPath.isEmpty())
        return;

    m_backupSyncRunning = true;
    QPointer<LibraryDatabase> self(this);
    std::thread([self, activePath, mirrorPath]() {
        const bool ok = syncBackupFromPath(activePath, mirrorPath);

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, ok]() {
            if (!self)
                return;

            self->m_backupSyncRunning = false;
            if (!ok)
                qWarning() << "[LibraryDatabase] Deferred backup DB sync failed";

            if (self->m_backupSyncAgain) {
                self->m_backupSyncAgain = false;
                self->scheduleBackupSync();
            }

            emit self->mirroredDatabaseStatusChanged();
        }, Qt::QueuedConnection);
    }).detach();
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
    if (!fileInfo.exists() || fileInfo.size() <= 0)
        return false;

    const QString connectionName = QStringLiteral("library_health_%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    bool ok = false;
    {
        QSqlDatabase healthDb = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        healthDb.setDatabaseName(path);

        if (!healthDb.open()) {
            qWarning() << "[LibraryDatabase] Health check open failed for" << path << ':' << healthDb.lastError().text();
        } else {
            {
                QSqlQuery q(healthDb);
                if (!q.exec("PRAGMA integrity_check")) {
                    qWarning() << "[LibraryDatabase] integrity_check failed to run for" << path << ':' << q.lastError().text();
                } else {
                    ok = q.next() && q.value(0).toString().compare(QStringLiteral("ok"), Qt::CaseInsensitive) == 0;
                    if (!ok)
                        qWarning() << "[LibraryDatabase] integrity_check reported corruption for" << path;
                }
            }
            healthDb.close();
        }

        healthDb = QSqlDatabase();
    }

    QSqlDatabase::removeDatabase(connectionName);
    return ok;
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

bool LibraryDatabase::syncBackupFromPrimary()
{
    if (!m_db.isOpen())
        return false;

    const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    const QString mirrorPath = (activePath == m_dbPath) ? m_backupDbPath : m_dbPath;

    {
        QSqlQuery q(m_db);
        if (!q.exec("PRAGMA wal_checkpoint(TRUNCATE)")) {
            qWarning() << "[LibraryDatabase] wal_checkpoint(TRUNCATE) failed:" << q.lastError().text();
            return false;
        }
    }

    return recreateDatabaseFileFromLiveConnection(mirrorPath);
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

bool LibraryDatabase::recreateDatabaseFileFromLiveConnection(const QString& targetPath)
{
    if (!m_db.isOpen() || targetPath.isEmpty())
        return false;

    QFile::remove(targetPath);
    QFile::remove(targetPath + QStringLiteral("-wal"));
    QFile::remove(targetPath + QStringLiteral("-shm"));

    const QString escapedTarget = targetPath;
    QString vacuumTarget = escapedTarget;
    vacuumTarget.replace(QLatin1Char('\''), QStringLiteral("''"));

    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("VACUUM INTO '%1'").arg(vacuumTarget))) {
        qWarning() << "[LibraryDatabase] VACUUM INTO failed for" << targetPath << ':' << q.lastError().text();
        return false;
    }

    return true;
}

bool LibraryDatabase::reopenDatabaseConnection()
{
    clearDatabaseConnection();

    m_db = QSqlDatabase::addDatabase("QSQLITE", "library_conn");
    const QString activePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    m_db.setDatabaseName(activePath);

    if (!m_db.open()) {
        qWarning() << "[LibraryDatabase] Failed to reopen:" << m_db.lastError().text();
        return false;
    }

    {
        QSqlQuery pragma(m_db);
        pragma.exec("PRAGMA journal_mode=WAL");
        pragma.exec("PRAGMA foreign_keys=ON");
    }

    if (!createSchema())
        return false;

    return true;
}

void LibraryDatabase::performMirrorSelfCheck()
{
    if (!m_db.isOpen())
        return;

    const bool primaryHealthy = isHealthyDatabaseFile(m_dbPath);
    const bool backupHealthy = isHealthyDatabaseFile(m_backupDbPath);
    bool repaired = false;
    bool switched = false;

    QString desiredActivePath = !m_activeDbPath.isEmpty() ? m_activeDbPath : m_dbPath;
    if (!primaryHealthy && !backupHealthy) {
        m_lastRecoveryEvent = QStringLiteral("both mirrors missing/corrupted — rebuilding from live session");
        if (m_db.isOpen()) {
            if (recreateDatabaseFileFromLiveConnection(m_dbPath)) {
                repaired = true;
                if (!recreateDatabaseFileFromLiveConnection(m_backupDbPath))
                    qWarning() << "[LibraryDatabase] Failed to recreate backup during both-files-missing recovery";
                m_lastRecoveryEvent = QStringLiteral("both mirrors restored from live session");
            } else {
                qWarning() << "[LibraryDatabase] Failed to recreate primary during both-files-missing recovery";
                m_lastRecoveryEvent = QStringLiteral("restore of both mirrors failed");
            }
        }
        m_primaryMirrorDegraded = true;
        m_backupMirrorDegraded = true;
        const QString currentStatus = mirroredDatabaseStatus();
        if (currentStatus != m_cachedMirrorStatus || repaired) {
            m_cachedMirrorStatus = currentStatus;
            emit mirroredDatabaseStatusChanged();
        }
        return;
    }

    if (desiredActivePath == m_dbPath && !primaryHealthy && backupHealthy) {
        desiredActivePath = m_backupDbPath;
        m_primaryMirrorDegraded = true;
        switched = true;
    } else if (desiredActivePath == m_backupDbPath && !backupHealthy && primaryHealthy) {
        desiredActivePath = m_dbPath;
        m_backupMirrorDegraded = true;
        switched = true;
    }

    if (desiredActivePath != m_activeDbPath) {
        const QString previousActivePath = m_activeDbPath;
        m_activeDbPath = desiredActivePath;
        const QString fromLabel = (previousActivePath == m_backupDbPath) ? QStringLiteral("B") : QStringLiteral("A");
        const QString toLabel = (m_activeDbPath == m_backupDbPath) ? QStringLiteral("B") : QStringLiteral("A");
        qWarning() << "[LibraryDatabase] Active mirror switched from" << fromLabel << "to" << toLabel;
        m_lastRecoveryEvent = QStringLiteral("Umschaltung %1 -> %2").arg(fromLabel, toLabel);
        if (!reopenDatabaseConnection())
            return;
    }

    if (!primaryHealthy && m_db.isOpen()) {
        if (recreateDatabaseFileFromLiveConnection(m_dbPath)) {
            m_primaryMirrorDegraded = true;
            repaired = true;
            m_lastRecoveryEvent = QStringLiteral("DB A restored from live session");
        } else {
            m_lastRecoveryEvent = QStringLiteral("DB A restore failed");
        }
    }

    if (!backupHealthy && m_db.isOpen()) {
        if (syncBackupFromPrimary()) {
            m_backupMirrorDegraded = true;
            repaired = true;
            m_lastRecoveryEvent = QStringLiteral("DB B restored from active mirror");
        } else {
            m_lastRecoveryEvent = QStringLiteral("DB B restore failed");
        }
    }

    const QString currentStatus = mirroredDatabaseStatus();
    if (currentStatus != m_cachedMirrorStatus || repaired || switched) {
        m_cachedMirrorStatus = currentStatus;
        emit mirroredDatabaseStatusChanged();
    }
}
