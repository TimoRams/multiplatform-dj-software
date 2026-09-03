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

bool LibraryDatabase::requestAnalysisPersistence(const QString& trackId,
                                                 const analysis::AnalysisResult& result)
{
    if (trackId.isEmpty() || !result.validated || !result.complete || !result.error.isEmpty()
        || !m_databaseWorker || !m_databaseWorker->isRunning())
        return false;

    DatabaseCommand command;
    command.type = DatabaseCommandType::Batch;
    command.priority = DatabasePriority::Persistence;
    command.requestId = m_nextDatabaseRequestId++;
    // No generation: the worker's generation counter tracks the library page
    // the table model is currently showing, and anything tagged with a
    // different value is dropped as stale before it reaches SQLite. Feeding an
    // analyzer request generation into that field meant practically every
    // finished analysis was discarded instead of written, so BPM, key and
    // beatgrid never survived a reload and every load re-ran a full analysis.
    // A superseded write for the same track is already replaced in the queue by
    // the coalescing key below.
    command.coalescingKey = QStringLiteral("analysis:%1").arg(trackId);

    QString gridType = QStringLiteral("unknown");
    if (result.beatGrid.type == TrackData::BeatGridType::ConstantTempo) gridType = QStringLiteral("constant");
    if (result.beatGrid.type == TrackData::BeatGridType::DynamicTempo) gridType = QStringLiteral("dynamic");
    command.statements.push_back({
        QStringLiteral("UPDATE Tracks SET bpm=:bpm,key=CASE WHEN length(trim(:key))>0 THEN :key ELSE key END,"
                       "is_analyzed=1,first_beat_sample=:first,analysis_sample_rate=:rate,analysis_version=:version,"
                       "analysis_section_versions=:sections,"
                       "bpm_confidence=:bc,beat_confidence=:bec,downbeat_confidence=:dc,grid_confidence=:gc,"
                       "beatgrid_type=:type,beatgrid_user_modified=:modified,"
                       "beatgrid_locked_by_user=CASE WHEN beatgrid_locked_by_user!=0 THEN beatgrid_locked_by_user ELSE :locked END,"
                       "track_segments=:segments WHERE id=:id"),
        {{QStringLiteral(":bpm"), result.bpm}, {QStringLiteral(":key"), result.detectedKey},
         {QStringLiteral(":first"), result.firstBeatSample}, {QStringLiteral(":rate"), result.sampleRate},
         {QStringLiteral(":version"), static_cast<int>(result.identity.analysisVersion)},
         {QStringLiteral(":sections"), result.sections.toStorageString()},
         {QStringLiteral(":bc"), result.confidence.bpmConfidence},
         {QStringLiteral(":bec"), result.confidence.beatConfidence},
         {QStringLiteral(":dc"), result.confidence.downbeatConfidence},
         {QStringLiteral(":gc"), result.confidence.gridConfidence},
         {QStringLiteral(":type"), gridType},
         {QStringLiteral(":modified"), result.beatGrid.userModified ? 1 : 0},
         {QStringLiteral(":locked"), result.beatGrid.lockedByUser ? 1 : 0},
         {QStringLiteral(":segments"), trackSegmentsToJson(result.phrases)},
         {QStringLiteral(":id"), trackId}}});

    const bool manual = result.beatGrid.userModified || result.beatGrid.lockedByUser;
    const QString writable = manual
        ? QStringLiteral("1")
        : QStringLiteral("COALESCE((SELECT beatgrid_locked_by_user FROM Tracks WHERE id=:id),0)=0");
    command.statements.push_back({QStringLiteral("DELETE FROM BeatGridMarkers WHERE track_id=:id AND %1").arg(writable),
                                  {{QStringLiteral(":id"), trackId}}});
    command.statements.push_back({QStringLiteral("DELETE FROM TempoNodes WHERE track_id=:id AND %1").arg(writable),
                                  {{QStringLiteral(":id"), trackId}}});
    for (int i = 0; i < static_cast<int>(result.beats.size()); ++i) {
        const auto& beat = result.beats[static_cast<std::size_t>(i)];
        command.statements.push_back({
            QStringLiteral("INSERT INTO BeatGridMarkers(track_id,beat_index,position_sec,is_downbeat,bar_number,beat_in_bar,confidence,user_modified,locked_by_user) "
                           "SELECT :id,:idx,:pos,:down,:bar,:inbar,:conf,:modified,:locked WHERE %1").arg(writable),
            {{QStringLiteral(":id"), trackId}, {QStringLiteral(":idx"), i},
             {QStringLiteral(":pos"), beat.positionSec}, {QStringLiteral(":down"), beat.isDownbeat ? 1 : 0},
             {QStringLiteral(":bar"), beat.barNumber}, {QStringLiteral(":inbar"), beat.beatInBar},
             {QStringLiteral(":conf"), beat.confidence}, {QStringLiteral(":modified"), beat.userModified ? 1 : 0},
             {QStringLiteral(":locked"), beat.lockedByUser ? 1 : 0}}});
    }
    for (int i = 0; i < static_cast<int>(result.beatGrid.tempoNodes.size()); ++i) {
        const auto& node = result.beatGrid.tempoNodes[static_cast<std::size_t>(i)];
        command.statements.push_back({
            QStringLiteral("INSERT INTO TempoNodes(track_id,node_index,position_sec,bpm,confidence) "
                           "SELECT :id,:idx,:pos,:bpm,:conf WHERE %1").arg(writable),
            {{QStringLiteral(":id"), trackId}, {QStringLiteral(":idx"), i},
             {QStringLiteral(":pos"), node.positionSec}, {QStringLiteral(":bpm"), node.bpm},
             {QStringLiteral(":conf"), node.confidence}}});
    }
    const auto requestId = command.requestId;
    if (!m_databaseWorker->enqueue(std::move(command))) return false;
    m_pendingAnalysisWrites.insert(requestId, trackId);
    return true;
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
        if (const auto analysisIt = m_pendingAnalysisWrites.find(result.requestId);
            analysisIt != m_pendingAnalysisWrites.end()) {
            const QString trackId = analysisIt.value();
            m_pendingAnalysisWrites.erase(analysisIt);
            if (result.success) {
                emit analysisUpdated(trackId);
                scheduleTableModelRefresh();
                scheduleBackupSync();
            } else {
                // Stale and cancelled results carry no SQL error text, so name
                // the reason instead of logging an empty string.
                const QString reason = result.stale
                    ? QStringLiteral("dropped as stale")
                    : (result.cancelled ? QStringLiteral("cancelled")
                                        : result.error);
                qWarning() << "[LibraryDatabase] async analysis persistence failed:"
                           << reason;
            }
        } else if (result.type == DatabaseCommandType::LoadLibraryPage) {
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

#include "LibraryDatabase.h"

#include <QDebug>
#include <QSqlQuery>
#include <QSqlError>

namespace {

bool tableHasColumn(QSqlDatabase& db, const QString& tableName, const QString& columnName)
{
    QSqlQuery q(db);
    if (!q.exec(QString("PRAGMA table_info(%1)").arg(tableName)))
        return false;

    while (q.next()) {
        if (q.value(1).toString().compare(columnName, Qt::CaseInsensitive) == 0)
            return true;
    }

    return false;
}

} // namespace

bool LibraryDatabase::createSchema()
{
    QSqlQuery q(m_db);

    // ── Schema version bookkeeping ───────────────────────────────────────
    q.exec("CREATE TABLE IF NOT EXISTS Meta ("
           "  key   TEXT PRIMARY KEY,"
           "  value TEXT"
           ")");

    q.prepare("SELECT value FROM Meta WHERE key = 'schema_version'");
    q.exec();

    int currentVersion = 0;
    if (q.next())
        currentVersion = q.value(0).toInt();

    if (currentVersion >= kSchemaVersion)
        return true; // already up to date

    // ── Version 1: initial tables ────────────────────────────────────────
    if (currentVersion < 1) {
        bool ok = true;

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS Tracks ("
            "  id           TEXT PRIMARY KEY,"
            "  title        TEXT,"
            "  artist       TEXT,"
            "  duration_sec INTEGER,"
            "  bitrate_kbps INTEGER DEFAULT 0,"
            "  bpm          REAL    DEFAULT 0.0,"
            "  key          TEXT    DEFAULT '',"
            "  main_cue_sec REAL    DEFAULT -1.0,"
            "  track_segments TEXT DEFAULT '',"
            "  is_analyzed  BOOLEAN DEFAULT 0"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] Tracks:" << q.lastError().text();

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS Locations ("
            "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  track_id  TEXT,"
            "  file_path TEXT UNIQUE,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] Locations:" << q.lastError().text();

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS CuePoints ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  track_id   TEXT,"
            "  cue_index  INTEGER,"
            "  position_sec REAL,"
            "  label      TEXT DEFAULT '',"
            "  color      TEXT DEFAULT '#FF0000',"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] CuePoints:" << q.lastError().text();

        if (!ok) return false;
    }

    if (currentVersion < 2) {
        bool ok = true;

        if (!tableHasColumn(m_db, "Tracks", "first_beat_sample")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN first_beat_sample INTEGER DEFAULT 0");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks first_beat_sample:" << q.lastError().text();
        }

        if (!tableHasColumn(m_db, "Tracks", "analysis_sample_rate")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN analysis_sample_rate REAL DEFAULT 44100.0");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks analysis_sample_rate:" << q.lastError().text();
        }

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS BeatGridMarkers ("
            "  track_id     TEXT NOT NULL,"
            "  beat_index   INTEGER NOT NULL,"
            "  position_sec REAL NOT NULL,"
            "  is_downbeat  INTEGER NOT NULL DEFAULT 0,"
            "  bar_number   INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY(track_id, beat_index),"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] BeatGridMarkers:" << q.lastError().text();

        if (!ok) return false;
    }

    if (currentVersion < 3) {
        bool ok = true;

        if (!tableHasColumn(m_db, "Tracks", "bitrate_kbps")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN bitrate_kbps INTEGER DEFAULT 0");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks bitrate_kbps:" << q.lastError().text();
        }

        if (!ok) return false;
    }

    if (currentVersion < 4) {
        bool ok = true;

        if (!tableHasColumn(m_db, "Tracks", "track_segments")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN track_segments TEXT DEFAULT ''");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks track_segments:" << q.lastError().text();
        }

        if (!ok) return false;
    }

    if (currentVersion < 5) {
        bool ok = true;

        ok &= q.exec(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_cuepoints_track_slot "
            "ON CuePoints(track_id, cue_index)");
        if (!ok) qWarning() << "[LibraryDatabase] CuePoints index:" << q.lastError().text();

        if (!tableHasColumn(m_db, "CuePoints", "label")) {
            ok &= q.exec("ALTER TABLE CuePoints ADD COLUMN label TEXT DEFAULT ''");
            if (!ok) qWarning() << "[LibraryDatabase] CuePoints label:" << q.lastError().text();
        }

        if (!tableHasColumn(m_db, "CuePoints", "color")) {
            ok &= q.exec("ALTER TABLE CuePoints ADD COLUMN color TEXT DEFAULT '#FF0000'");
            if (!ok) qWarning() << "[LibraryDatabase] CuePoints color:" << q.lastError().text();
        }

        if (!ok) return false;
    }

    if (currentVersion < 6) {
        bool ok = true;

        if (!tableHasColumn(m_db, "Tracks", "main_cue_sec")) {
            ok &= q.exec("ALTER TABLE Tracks ADD COLUMN main_cue_sec REAL DEFAULT -1.0");
            if (!ok) qWarning() << "[LibraryDatabase] Tracks main_cue_sec:" << q.lastError().text();
        }

        if (!ok) return false;
    }

    if (currentVersion < 7) {
        bool ok = true;

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS Playlists ("
            "  id         TEXT PRIMARY KEY,"
            "  name       TEXT NOT NULL,"
            "  parent_id  TEXT DEFAULT NULL,"
            "  sort_order INTEGER DEFAULT 0,"
            "  FOREIGN KEY(parent_id) REFERENCES Playlists(id) ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] Playlists:" << q.lastError().text();

        ok &= q.exec(
            "CREATE TABLE IF NOT EXISTS PlaylistItems ("
            "  playlist_id TEXT NOT NULL,"
            "  track_id    TEXT NOT NULL,"
            "  position    INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY(playlist_id, track_id),"
            "  FOREIGN KEY(playlist_id) REFERENCES Playlists(id) ON DELETE CASCADE,"
            "  FOREIGN KEY(track_id)    REFERENCES Tracks(id)    ON DELETE CASCADE"
            ")");
        if (!ok) qWarning() << "[LibraryDatabase] PlaylistItems:" << q.lastError().text();

        if (!ok) return false;
    }

    if (currentVersion < 8) {
        // Version 8: reserved (FLAC metadata fix applied manually to existing databases;
        // code-level fix via TagLib means new data is stored correctly from the start).
    }

    if (currentVersion < 9) {
        // Version 9: add per-track user metadata columns.
        const QStringList cols = {
            "ALTER TABLE Tracks ADD COLUMN genre       TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN album       TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN comment     TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN rating      INTEGER DEFAULT 0",
            "ALTER TABLE Tracks ADD COLUMN energy      INTEGER DEFAULT 0",
            "ALTER TABLE Tracks ADD COLUMN color       TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN notes       TEXT    DEFAULT ''",
            "ALTER TABLE Tracks ADD COLUMN play_count  INTEGER DEFAULT 0",
            "ALTER TABLE Tracks ADD COLUMN last_played INTEGER DEFAULT 0",
            "ALTER TABLE Tracks ADD COLUMN date_added  INTEGER DEFAULT 0",
        };
        for (const auto& sql : cols) {
            const QString col = sql.section(' ', 5, 5);
            if (!tableHasColumn(m_db, "Tracks", col))
                q.exec(sql);
        }
        // Indices for fast sorting / filtering on new columns.
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_rating ON Tracks(rating)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_energy ON Tracks(energy)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_genre  ON Tracks(genre COLLATE NOCASE)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_bpm    ON Tracks(bpm)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracks_date   ON Tracks(date_added)");
    }

    if (currentVersion < 10) {
        // Version 10: tag system.
        q.exec(
            "CREATE TABLE IF NOT EXISTS Tags ("
            "  id    TEXT PRIMARY KEY,"
            "  name  TEXT NOT NULL,"
            "  color TEXT DEFAULT '#888888'"
            ")");
        q.exec(
            "CREATE TABLE IF NOT EXISTS TrackTags ("
            "  track_id TEXT NOT NULL,"
            "  tag_id   TEXT NOT NULL,"
            "  PRIMARY KEY(track_id, tag_id),"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE,"
            "  FOREIGN KEY(tag_id)   REFERENCES Tags(id)   ON DELETE CASCADE"
            ")");
        q.exec("CREATE INDEX IF NOT EXISTS idx_tracktags_tag ON TrackTags(tag_id)");
    }

    if (currentVersion < 11) {
        // Version 11: play history.
        q.exec(
            "CREATE TABLE IF NOT EXISTS PlayHistory ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  track_id   TEXT NOT NULL,"
            "  played_at  INTEGER NOT NULL,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        q.exec("CREATE INDEX IF NOT EXISTS idx_history_track   ON PlayHistory(track_id)");
        q.exec("CREATE INDEX IF NOT EXISTS idx_history_time    ON PlayHistory(played_at DESC)");
    }

    if (currentVersion < 12) {
        // Version 12: favorites.
        q.exec(
            "CREATE TABLE IF NOT EXISTS Favorites ("
            "  track_id TEXT PRIMARY KEY,"
            "  added_at INTEGER NOT NULL DEFAULT 0,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
    }

    if (currentVersion < 13) {
        // Version 13: prepare crate.
        q.exec(
            "CREATE TABLE IF NOT EXISTS PrepareCrate ("
            "  track_id TEXT PRIMARY KEY,"
            "  position INTEGER NOT NULL DEFAULT 0,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
    }

    if (currentVersion < 14) {
        // Version 14: track queue + smart collections.
        q.exec(
            "CREATE TABLE IF NOT EXISTS TrackQueue ("
            "  track_id TEXT PRIMARY KEY,"
            "  position INTEGER NOT NULL DEFAULT 0,"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
        q.exec(
            "CREATE TABLE IF NOT EXISTS SmartCollections ("
            "  id         TEXT PRIMARY KEY,"
            "  name       TEXT NOT NULL,"
            "  rules_json TEXT NOT NULL DEFAULT '[]',"
            "  sort_order INTEGER DEFAULT 0"
            ")");
    }

    if (currentVersion < 15) {
        struct ColumnAdd {
            const char* table;
            const char* column;
            const char* sql;
        };
        const ColumnAdd adds[] = {
            {"Tracks", "analysis_version", "ALTER TABLE Tracks ADD COLUMN analysis_version INTEGER DEFAULT 0"},
            {"Tracks", "bpm_confidence", "ALTER TABLE Tracks ADD COLUMN bpm_confidence REAL DEFAULT 0.0"},
            {"Tracks", "beat_confidence", "ALTER TABLE Tracks ADD COLUMN beat_confidence REAL DEFAULT 0.0"},
            {"Tracks", "downbeat_confidence", "ALTER TABLE Tracks ADD COLUMN downbeat_confidence REAL DEFAULT 0.0"},
            {"Tracks", "grid_confidence", "ALTER TABLE Tracks ADD COLUMN grid_confidence REAL DEFAULT 0.0"},
            {"Tracks", "beatgrid_type", "ALTER TABLE Tracks ADD COLUMN beatgrid_type TEXT DEFAULT 'constant'"},
            {"Tracks", "beatgrid_user_modified", "ALTER TABLE Tracks ADD COLUMN beatgrid_user_modified INTEGER DEFAULT 0"},
            {"Tracks", "beatgrid_locked_by_user", "ALTER TABLE Tracks ADD COLUMN beatgrid_locked_by_user INTEGER DEFAULT 0"},
            {"BeatGridMarkers", "beat_in_bar", "ALTER TABLE BeatGridMarkers ADD COLUMN beat_in_bar INTEGER DEFAULT 1"},
            {"BeatGridMarkers", "confidence", "ALTER TABLE BeatGridMarkers ADD COLUMN confidence REAL DEFAULT 0.0"},
            {"BeatGridMarkers", "user_modified", "ALTER TABLE BeatGridMarkers ADD COLUMN user_modified INTEGER DEFAULT 0"},
            {"BeatGridMarkers", "locked_by_user", "ALTER TABLE BeatGridMarkers ADD COLUMN locked_by_user INTEGER DEFAULT 0"},
        };
        for (const auto& add : adds) {
            if (!tableHasColumn(m_db, add.table, add.column) && !q.exec(add.sql))
                qWarning() << "[LibraryDatabase] schema v15 column" << add.column << q.lastError().text();
        }

        q.exec(
            "CREATE TABLE IF NOT EXISTS TempoNodes ("
            "  track_id     TEXT NOT NULL,"
            "  node_index   INTEGER NOT NULL,"
            "  position_sec REAL NOT NULL,"
            "  bpm          REAL NOT NULL,"
            "  confidence   REAL DEFAULT 0.0,"
            "  PRIMARY KEY(track_id, node_index),"
            "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
            ")");
    }

    if (currentVersion < 16) {
        if (!q.exec(
                "CREATE TABLE IF NOT EXISTS SavedLoops ("
                "  track_id   TEXT NOT NULL,"
                "  loop_index INTEGER NOT NULL,"
                "  in_sec     REAL NOT NULL,"
                "  out_sec    REAL NOT NULL,"
                "  label      TEXT DEFAULT '',"
                "  color      TEXT DEFAULT '#30b050',"
                "  PRIMARY KEY(track_id, loop_index),"
                "  FOREIGN KEY(track_id) REFERENCES Tracks(id) ON DELETE CASCADE"
                ")")) {
            qWarning() << "[LibraryDatabase] SavedLoops:" << q.lastError().text();
        }
    }

    if (currentVersion < 17) {
        if (!tableHasColumn(m_db, "Tracks", "analysis_section_versions")
            && !q.exec("ALTER TABLE Tracks ADD COLUMN analysis_section_versions TEXT DEFAULT ''")) {
            qWarning() << "[LibraryDatabase] analysis_section_versions:" << q.lastError().text();
        }
    }

    // ── Stamp current version ────────────────────────────────────────────
    q.prepare("INSERT OR REPLACE INTO Meta (key, value) VALUES ('schema_version', :v)");
    q.bindValue(":v", kSchemaVersion);
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase] Meta schema_version:" << q.lastError().text();
        return false;
    }

    qDebug() << "[LibraryDatabase] Schema created/updated to version" << kSchemaVersion;
    return true;
}
