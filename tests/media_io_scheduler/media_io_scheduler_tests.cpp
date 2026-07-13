#include "io/MediaIoScheduler.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
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

std::vector<MediaIoResult> waitFor(MediaIoScheduler& scheduler, std::size_t count)
{
    std::vector<MediaIoResult> all;
    QElapsedTimer timer;
    timer.start();
    while (all.size() < count && timer.elapsed() < 10000) {
        auto batch = scheduler.takeResults();
        for (auto& result : batch)
            all.push_back(std::move(result));
        QThread::msleep(1);
    }
    require(all.size() >= count, "timed out waiting for media results");
    return all;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QString imagePath = directory.filePath(QStringLiteral("cover.png"));
    QImage image(128, 64, QImage::Format_ARGB32);
    image.fill(Qt::magenta);
    require(image.save(imagePath), "synthetic image");
    QFile text(directory.filePath(QStringLiteral("track.txt")));
    require(text.open(QIODevice::WriteOnly), "synthetic file");
    text.write("synthetic");
    text.close();

    MediaIoScheduler::Configuration configuration;
    configuration.queueCapacity = 8;
    configuration.resultCapacity = 32;
    configuration.maintenanceFairnessInterval = 2;
    MediaIoScheduler scheduler(configuration);
    QElapsedTimer lifecycle;
    lifecycle.start();
    require(scheduler.start() && scheduler.start(), "idempotent start");
    const auto startMicros = lifecycle.nsecsElapsed() / 1000;

    MediaIoRequest cover;
    cover.type = MediaIoRequestType::ReadCoverArt;
    cover.requestId = 1;
    cover.inputPath = imagePath;
    MediaIoRequest thumbnail = cover;
    thumbnail.type = MediaIoRequestType::DecodeCoverThumbnail;
    thumbnail.requestId = 2;
    thumbnail.maximumImageDimension = 32;
    MediaIoRequest validate = cover;
    validate.type = MediaIoRequestType::ValidateTrackPath;
    validate.requestId = 3;
    MediaIoRequest scan;
    scan.type = MediaIoRequestType::ScanDirectory;
    scan.priority = MediaIoPriority::LibraryScan;
    scan.requestId = 4;
    scan.inputPath = directory.path();
    scan.maximumEntries = 8;
    require(scheduler.enqueue(std::move(cover)), "cover request");
    require(scheduler.enqueue(std::move(thumbnail)), "thumbnail request");
    require(scheduler.enqueue(std::move(validate)), "validate request");
    require(scheduler.enqueue(std::move(scan)), "scan request");
    const auto basic = waitFor(scheduler, 4);
    for (const auto& result : basic)
        require(result.success, "basic media operation");

    auto cancellation = std::make_shared<std::atomic_bool>(true);
    MediaIoRequest cancelled;
    cancelled.type = MediaIoRequestType::ScanDirectory;
    cancelled.requestId = 5;
    cancelled.inputPath = directory.path();
    cancelled.cancellation = cancellation;
    require(scheduler.enqueue(std::move(cancelled)), "cancelled request queued");
    require(waitFor(scheduler, 1).front().cancelled, "cancellation result");

    scheduler.setCurrentGeneration(3, 2);
    MediaIoRequest stale;
    stale.type = MediaIoRequestType::ValidateTrackPath;
    stale.ownerId = 3;
    stale.requestId = 6;
    stale.generation = 1;
    stale.inputPath = imagePath;
    require(scheduler.enqueue(std::move(stale)), "stale request queued");
    require(waitFor(scheduler, 1).front().stale, "generation result discarded");

    MediaIoRequest invalid;
    invalid.type = MediaIoRequestType::DecodeCoverThumbnail;
    invalid.requestId = 7;
    invalid.inputData = QByteArrayLiteral("not an image");
    MediaIoRequest disappeared;
    disappeared.type = MediaIoRequestType::ValidateTrackPath;
    disappeared.requestId = 8;
    disappeared.inputPath = directory.filePath(QStringLiteral("missing.wav"));
    require(scheduler.enqueue(std::move(invalid)), "invalid image request");
    require(scheduler.enqueue(std::move(disappeared)), "missing path request");
    const auto errors = waitFor(scheduler, 2);
    require(!errors[0].success && !errors[1].success, "portable errors returned");

    lifecycle.restart();
    scheduler.requestStop();
    scheduler.stopAndJoin();
    scheduler.stopAndJoin();
    const auto stopMicros = lifecycle.nsecsElapsed() / 1000;
    const auto stats = scheduler.stats();
    require(stats.workerThreadHash != 0, "worker thread recorded");
    std::cout << "MediaIoScheduler start_us=" << startMicros
              << " stop_us=" << stopMicros
              << " avg_request_us=" << stats.averageRequestMicros
              << " worst_request_us=" << stats.worstRequestMicros << '\n';
    return 0;
}
