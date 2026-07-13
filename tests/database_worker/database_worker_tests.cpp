#include "database/DatabaseWorker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool value, const char* message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<DatabaseResult> waitFor(DatabaseWorker& worker, std::size_t count)
{
    std::vector<DatabaseResult> all;
    QElapsedTimer timer;
    timer.start();
    while (all.size() < count && timer.elapsed() < 10000) {
        auto batch = worker.takeResults();
        for (auto& result : batch)
            all.push_back(std::move(result));
        QThread::msleep(1);
    }
    require(all.size() >= count, "timed out waiting for database results");
    return all;
}

DatabaseCommand sql(std::uint64_t id, QString statement,
                    DatabaseCommandType type = DatabaseCommandType::Execute)
{
    DatabaseCommand command;
    command.requestId = id;
    command.type = type;
    command.sql = std::move(statement);
    return command;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");

    DatabaseWorker::Configuration configuration;
    configuration.databasePath = directory.filePath(QStringLiteral("library.db"));
    configuration.queueCapacity = 8;
    configuration.resultCapacity = 32;
    configuration.maintenanceFairnessInterval = 2;
    DatabaseWorker worker(configuration);

    QElapsedTimer lifecycle;
    lifecycle.start();
    require(worker.start(), "start");
    require(worker.start(), "idempotent start");
    const auto startMicros = lifecycle.nsecsElapsed() / 1000;

    require(worker.enqueue(sql(1, QStringLiteral(
        "CREATE TABLE tracks(id TEXT PRIMARY KEY, title TEXT, bpm REAL, cue REAL)"))), "create");
    auto insert = sql(2, QStringLiteral(
        "INSERT INTO tracks(id,title,bpm,cue) VALUES(:id,:title,:bpm,:cue)"));
    insert.type = DatabaseCommandType::UpsertTrack;
    insert.priority = DatabasePriority::Interactive;
    insert.bindings = {{QStringLiteral(":id"), QStringLiteral("track-1")},
                       {QStringLiteral(":title"), QStringLiteral("Synthetic")},
                       {QStringLiteral(":bpm"), 128.0},
                       {QStringLiteral(":cue"), 1.25}};
    require(worker.enqueue(std::move(insert)), "insert");
    require(worker.enqueue(sql(3, QStringLiteral(
        "SELECT id,title,bpm,cue FROM tracks"), DatabaseCommandType::LoadTrack)), "load");
    auto crud = waitFor(worker, 3);
    require(crud[0].success && crud[1].success && crud[2].success, "CRUD success");
    require(crud[2].rows.size() == 1, "track loaded");

    DatabaseCommand batch;
    batch.type = DatabaseCommandType::Batch;
    batch.priority = DatabasePriority::Persistence;
    batch.requestId = 4;
    batch.statements = {
        {QStringLiteral("UPDATE tracks SET bpm=130 WHERE id='track-1'"), {}},
        {QStringLiteral("UPDATE tracks SET cue=2.5 WHERE id='track-1'"), {}}};
    require(worker.enqueue(std::move(batch)), "batch");
    require(waitFor(worker, 1).front().success, "batch commit");

    DatabaseCommand rollback;
    rollback.type = DatabaseCommandType::Batch;
    rollback.requestId = 5;
    rollback.statements = {
        {QStringLiteral("UPDATE tracks SET bpm=140 WHERE id='track-1'"), {}},
        {QStringLiteral("INSERT INTO missing_table VALUES(1)"), {}}};
    require(worker.enqueue(std::move(rollback)), "rollback batch");
    require(!waitFor(worker, 1).front().success, "rollback error reported");

    auto cancelled = sql(6, QStringLiteral("SELECT 1"), DatabaseCommandType::Query);
    cancelled.cancellation = std::make_shared<std::atomic_bool>(true);
    require(worker.enqueue(std::move(cancelled)), "cancel request");
    require(waitFor(worker, 1).front().cancelled, "cancel result");

    worker.setCurrentGeneration(2);
    auto stale = sql(7, QStringLiteral("SELECT 1"), DatabaseCommandType::Query);
    stale.generation = 1;
    require(worker.enqueue(std::move(stale)), "stale request");
    require(waitFor(worker, 1).front().stale, "stale generation discarded");

    DatabaseCommand quick;
    quick.type = DatabaseCommandType::RunQuickCheck;
    quick.priority = DatabasePriority::Maintenance;
    quick.requestId = 8;
    DatabaseCommand full = quick;
    full.type = DatabaseCommandType::RunFullIntegrityCheck;
    full.requestId = 9;
    DatabaseCommand backup;
    backup.type = DatabaseCommandType::CreateBackup;
    backup.priority = DatabasePriority::Background;
    backup.requestId = 10;
    backup.targetPath = directory.filePath(QStringLiteral("backup.db"));
    require(worker.enqueue(std::move(quick)), "quick check");
    require(worker.enqueue(std::move(full)), "full check");
    require(worker.enqueue(std::move(backup)), "backup");
    const auto maintenance = waitFor(worker, 3);
    for (const auto& result : maintenance)
        require(result.success, "maintenance operation");
    require(QFileInfo::exists(directory.filePath(QStringLiteral("backup.db"))), "backup published");

    lifecycle.restart();
    worker.requestStop();
    worker.stopAndJoin();
    worker.stopAndJoin();
    const auto stopMicros = lifecycle.nsecsElapsed() / 1000;
    const auto stats = worker.stats();
    require(stats.workerThreadHash != 0, "worker thread recorded");
    require(stats.workerThreadHash == stats.connectionOpenedThreadHash, "connection opened on worker");
    require(stats.workerThreadHash == stats.connectionClosedThreadHash, "connection closed on worker");
    require(stats.integrityChecks == 2 && stats.backupsCompleted == 1, "maintenance stats");

    std::cout << "DatabaseWorker start_us=" << startMicros
              << " stop_us=" << stopMicros
              << " avg_command_us=" << stats.averageCommandMicros
              << " worst_command_us=" << stats.worstCommandMicros << '\n';
    return 0;
}
