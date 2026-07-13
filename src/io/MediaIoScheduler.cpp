#include "MediaIoScheduler.h"

#include "library/CoverArtExtractor.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>

#include <taglib/fileref.h>
#include <taglib/tag.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace {

bool cancelled(const MediaIoRequest& request) noexcept
{
    return request.cancellation
        && request.cancellation->load(std::memory_order_acquire);
}

QByteArray readBoundedFile(const QString& path, std::size_t maximumBytes, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    if (file.size() < 0 || static_cast<std::uint64_t>(file.size()) > maximumBytes) {
        if (error)
            *error = QStringLiteral("file exceeds configured size limit");
        return {};
    }
    return file.readAll();
}

} // namespace

MediaIoScheduler::MediaIoScheduler()
    : MediaIoScheduler(Configuration{})
{
}

MediaIoScheduler::MediaIoScheduler(Configuration configuration)
    : m_configuration(std::move(configuration))
{
    m_configuration.queueCapacity = std::max<std::size_t>(1, m_configuration.queueCapacity);
    m_configuration.resultCapacity = std::max<std::size_t>(1, m_configuration.resultCapacity);
    m_configuration.maintenanceFairnessInterval =
        std::max<std::size_t>(1, m_configuration.maintenanceFairnessInterval);
}

MediaIoScheduler::~MediaIoScheduler()
{
    stopAndJoin();
}

bool MediaIoScheduler::start()
{
    std::lock_guard lock(m_mutex);
    if (m_started)
        return true;
    m_started = true;
    m_stopRequested = false;
    m_thread = std::thread([this] { workerLoop(); });
    return true;
}

void MediaIoScheduler::requestStop() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        m_stopRequested = true;
    }
    m_condition.notify_all();
}

void MediaIoScheduler::stopAndJoin() noexcept
{
    requestStop();
    if (m_thread.joinable())
        m_thread.join();
    std::lock_guard lock(m_mutex);
    m_started = false;
}

bool MediaIoScheduler::enqueue(MediaIoRequest request) noexcept
{
    try {
        std::lock_guard lock(m_mutex);
        if (!m_started || m_stopRequested) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!request.coalescingKey.isEmpty()) {
            for (auto& queue : m_queues) {
                const auto existing = std::find_if(queue.begin(), queue.end(), [&](const auto& item) {
                    return item.coalescingKey == request.coalescingKey;
                });
                if (existing != queue.end()) {
                    *existing = std::move(request);
                    m_coalesced.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
        }
        if (queueDepthLocked() >= m_configuration.queueCapacity) {
            const std::size_t incoming = priorityIndex(request.priority);
            bool madeRoom = false;
            for (std::size_t index = kPriorityCount; index-- > incoming + 1;) {
                if (!m_queues[index].empty()) {
                    const auto evicted = std::move(m_queues[index].back());
                    m_queues[index].pop_back();
                    MediaIoResult dropped;
                    dropped.type = evicted.type;
                    dropped.requestId = evicted.requestId;
                    dropped.generation = evicted.generation;
                    dropped.ownerId = evicted.ownerId;
                    dropped.error = QStringLiteral("request dropped by bounded queue backpressure");
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
        m_queues[priorityIndex(request.priority)].push_back(std::move(request));
        m_queued.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    m_condition.notify_one();
    return true;
}

std::vector<MediaIoResult> MediaIoScheduler::takeResults(std::size_t maximum)
{
    std::vector<MediaIoResult> results;
    std::lock_guard lock(m_mutex);
    const std::size_t count = std::min(maximum, m_results.size());
    results.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        results.push_back(std::move(m_results.front()));
        m_results.pop_front();
    }
    return results;
}

std::vector<MediaIoResult> MediaIoScheduler::takeResultsForOwner(
    std::uint32_t ownerId, std::size_t maximum)
{
    std::vector<MediaIoResult> results;
    std::lock_guard lock(m_mutex);
    for (auto it = m_results.begin(); it != m_results.end() && results.size() < maximum;) {
        if (it->ownerId != ownerId) {
            ++it;
            continue;
        }
        results.push_back(std::move(*it));
        it = m_results.erase(it);
    }
    return results;
}

void MediaIoScheduler::setCurrentGeneration(std::uint64_t generation) noexcept
{
    setCurrentGeneration(0, generation);
}

void MediaIoScheduler::setCurrentGeneration(std::uint32_t ownerId,
                                            std::uint64_t generation) noexcept
{
    m_generations[std::min<std::size_t>(ownerId, kMaximumOwners - 1)]
        .store(generation, std::memory_order_release);
    m_condition.notify_one();
}

std::uint64_t MediaIoScheduler::currentGeneration() const noexcept
{
    return currentGeneration(0);
}

std::uint64_t MediaIoScheduler::currentGeneration(std::uint32_t ownerId) const noexcept
{
    return m_generations[std::min<std::size_t>(ownerId, kMaximumOwners - 1)]
        .load(std::memory_order_acquire);
}

bool MediaIoScheduler::isRunning() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_started && !m_stopRequested;
}

MediaIoSchedulerStats MediaIoScheduler::stats() const noexcept
{
    MediaIoSchedulerStats value;
    value.queuedRequests = m_queued.load(std::memory_order_relaxed);
    value.completedRequests = m_completed.load(std::memory_order_relaxed);
    value.failedRequests = m_failed.load(std::memory_order_relaxed);
    value.cancelledRequests = m_cancelled.load(std::memory_order_relaxed);
    value.staleResults = m_stale.load(std::memory_order_relaxed);
    value.droppedRequests = m_dropped.load(std::memory_order_relaxed);
    value.coalescedRequests = m_coalesced.load(std::memory_order_relaxed);
    const auto timed = m_timedRequests.load(std::memory_order_relaxed);
    value.averageRequestMicros = timed == 0 ? 0.0
        : static_cast<double>(m_totalMicros.load(std::memory_order_relaxed))
              / static_cast<double>(timed);
    value.worstRequestMicros = static_cast<double>(m_worstMicros.load(std::memory_order_relaxed));
    value.workerThreadHash = m_workerThreadHash.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(m_mutex);
        value.queueDepth = queueDepthLocked();
        value.resultDepth = m_results.size();
    }
    return value;
}

void MediaIoScheduler::workerLoop()
{
    m_workerThreadHash.store(
        static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())),
        std::memory_order_relaxed);
    while (true) {
        MediaIoRequest request;
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] {
                return m_stopRequested || queueDepthLocked() != 0;
            });
            if (m_stopRequested && queueDepthLocked() == 0)
                break;
            if (!popNextRequest(request))
                continue;
        }

        if (cancelled(request)) {
            MediaIoResult result;
            result.type = request.type;
            result.requestId = request.requestId;
            result.generation = request.generation;
            result.ownerId = request.ownerId;
            result.cancelled = true;
            m_cancelled.fetch_add(1, std::memory_order_relaxed);
            publishResult(std::move(result));
            continue;
        }
        if (requestIsStale(request)) {
            MediaIoResult result;
            result.type = request.type;
            result.requestId = request.requestId;
            result.generation = request.generation;
            result.ownerId = request.ownerId;
            result.stale = true;
            m_stale.fetch_add(1, std::memory_order_relaxed);
            publishResult(std::move(result));
            continue;
        }

        const auto start = std::chrono::steady_clock::now();
        auto result = execute(request);
        if (!result.cancelled && cancelled(request)) {
            result.success = false;
            result.cancelled = true;
            result.data.clear();
            result.paths.clear();
            result.metadata.clear();
        } else if (!result.cancelled && requestIsStale(request)) {
            result.success = false;
            result.stale = true;
            result.data.clear();
            result.paths.clear();
            result.metadata.clear();
            m_stale.fetch_add(1, std::memory_order_relaxed);
        }
        const auto micros = static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        result.elapsedMicros = static_cast<double>(micros);
        m_totalMicros.fetch_add(micros, std::memory_order_relaxed);
        m_timedRequests.fetch_add(1, std::memory_order_relaxed);
        auto worst = m_worstMicros.load(std::memory_order_relaxed);
        while (micros > worst && !m_worstMicros.compare_exchange_weak(
                   worst, micros, std::memory_order_relaxed)) {
        }
        if (result.stale) {
            // Accounted above; stale work is deliberately not published as success.
        } else if (result.success)
            m_completed.fetch_add(1, std::memory_order_relaxed);
        else if (result.cancelled)
            m_cancelled.fetch_add(1, std::memory_order_relaxed);
        else
            m_failed.fetch_add(1, std::memory_order_relaxed);
        publishResult(std::move(result));
    }
}

bool MediaIoScheduler::popNextRequest(MediaIoRequest& request)
{
    const std::size_t maintenance = priorityIndex(MediaIoPriority::Maintenance);
    if (m_requestsSinceMaintenance >= m_configuration.maintenanceFairnessInterval
        && !m_queues[maintenance].empty()) {
        request = std::move(m_queues[maintenance].front());
        m_queues[maintenance].pop_front();
        m_requestsSinceMaintenance = 0;
        return true;
    }
    for (std::size_t index = 0; index < kPriorityCount; ++index) {
        if (m_queues[index].empty())
            continue;
        request = std::move(m_queues[index].front());
        m_queues[index].pop_front();
        if (index == maintenance)
            m_requestsSinceMaintenance = 0;
        else
            ++m_requestsSinceMaintenance;
        return true;
    }
    return false;
}

MediaIoResult MediaIoScheduler::execute(const MediaIoRequest& request)
{
    MediaIoResult result;
    result.type = request.type;
    result.requestId = request.requestId;
    result.generation = request.generation;
    result.ownerId = request.ownerId;
    if (cancelled(request)) {
        result.cancelled = true;
        return result;
    }

    if (request.type == MediaIoRequestType::ValidateTrackPath) {
        const QFileInfo info(request.inputPath);
        result.success = info.exists() && info.isFile() && info.isReadable();
        result.metadata.insert(QStringLiteral("absolutePath"), info.absoluteFilePath());
        result.metadata.insert(QStringLiteral("size"), info.size());
        if (!result.success)
            result.error = QStringLiteral("path is missing, unreadable, or not a file");
        return result;
    }

    if (request.type == MediaIoRequestType::ReadTrackMetadata) {
        const QFileInfo info(request.inputPath);
        if (!info.exists() || !info.isFile() || !info.isReadable()) {
            result.error = QStringLiteral("track is missing or unreadable");
            return result;
        }
        TagLib::FileRef file(request.inputPath.toUtf8().constData());
        if (file.isNull()) {
            result.error = QStringLiteral("unsupported or invalid media metadata");
            return result;
        }
        result.metadata.insert(QStringLiteral("fileName"), info.fileName());
        result.metadata.insert(QStringLiteral("absolutePath"), info.absoluteFilePath());
        result.metadata.insert(QStringLiteral("size"), info.size());
        if (const auto* tag = file.tag()) {
            result.metadata.insert(QStringLiteral("title"),
                                   QString::fromStdWString(tag->title().toWString()));
            result.metadata.insert(QStringLiteral("artist"),
                                   QString::fromStdWString(tag->artist().toWString()));
            result.metadata.insert(QStringLiteral("album"),
                                   QString::fromStdWString(tag->album().toWString()));
            result.metadata.insert(QStringLiteral("genre"),
                                   QString::fromStdWString(tag->genre().toWString()));
        }
        result.success = true;
        return result;
    }

    if (request.type == MediaIoRequestType::ReadCoverArt) {
        const QString suffix = QFileInfo(request.inputPath).suffix().toLower();
        if (suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg")
            || suffix == QStringLiteral("jpeg")) {
            result.data = readBoundedFile(request.inputPath, request.maximumBytes, &result.error);
        } else {
            result.data = CoverArtExtractor::extractCoverArt(request.inputPath).first;
            if (static_cast<std::uint64_t>(result.data.size()) > request.maximumBytes) {
                result.data.clear();
                result.error = QStringLiteral("cover exceeds configured size limit");
            }
        }
        result.success = !result.data.isEmpty();
        if (!result.success && result.error.isEmpty())
            result.error = QStringLiteral("no supported cover art found");
        return result;
    }

    if (request.type == MediaIoRequestType::DecodeCoverThumbnail) {
        QByteArray source = request.inputData;
        if (source.isEmpty())
            source = readBoundedFile(request.inputPath, request.maximumBytes, &result.error);
        if (source.isEmpty())
            return result;
        QImage image = QImage::fromData(source);
        if (image.isNull()) {
            result.error = QStringLiteral("invalid image data");
            return result;
        }
        const int maximum = std::clamp(request.maximumImageDimension, 16, 4096);
        if (image.width() > maximum || image.height() > maximum)
            image = image.scaled(maximum, maximum, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QBuffer buffer(&result.data);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
            result.error = QStringLiteral("thumbnail encoding failed");
            result.data.clear();
            return result;
        }
        result.metadata.insert(QStringLiteral("width"), image.width());
        result.metadata.insert(QStringLiteral("height"), image.height());
        result.success = true;
        return result;
    }

    if (request.type == MediaIoRequestType::ScanDirectory) {
        const QFileInfo root(request.inputPath);
        if (!root.exists() || !root.isDir() || !root.isReadable()) {
            result.error = QStringLiteral("scan root is missing or unreadable");
            return result;
        }
        QDir::Filters flags = QDir::NoDotAndDotDot | QDir::Readable | QDir::NoSymLinks;
        if (request.includeFiles)
            flags |= QDir::Files;
        if (request.includeDirectories)
            flags |= QDir::Dirs;
        if (!request.includeFiles && !request.includeDirectories) {
            result.error = QStringLiteral("scan has no requested entry type");
            return result;
        }
        const auto iteratorFlags = request.recursive ? QDirIterator::Subdirectories
                                                     : QDirIterator::NoIteratorFlags;
        QDirIterator iterator(request.inputPath, request.nameFilters, flags, iteratorFlags);
        while (iterator.hasNext() && result.paths.size() <
                   static_cast<qsizetype>(request.maximumEntries)) {
            if (cancelled(request)) {
                result.paths.clear();
                result.cancelled = true;
                return result;
            }
            const QFileInfo entry = iterator.nextFileInfo();
            result.paths.push_back(entry.fileName());
        }
        result.success = true;
        return result;
    }

    if (request.type == MediaIoRequestType::ReadAnalysisArtifact) {
        result.data = readBoundedFile(request.inputPath, request.maximumBytes, &result.error);
        result.success = !result.data.isEmpty() || QFileInfo(request.inputPath).size() == 0;
        return result;
    }

    if (request.type == MediaIoRequestType::WriteAnalysisArtifact
        || request.type == MediaIoRequestType::ExportLibraryData) {
        QSaveFile file(request.outputPath);
        if (!file.open(QIODevice::WriteOnly)) {
            result.error = file.errorString();
            return result;
        }
        if (file.write(request.inputData) != request.inputData.size() || cancelled(request)) {
            file.cancelWriting();
            result.cancelled = cancelled(request);
            result.error = result.cancelled ? QString() : file.errorString();
            return result;
        }
        result.success = file.commit();
        if (!result.success)
            result.error = file.errorString();
        return result;
    }

    if (request.type == MediaIoRequestType::CopyFile) {
        const QString temporary = request.outputPath + QStringLiteral(".tmp");
        QFile::remove(temporary);
        if (!QFile::copy(request.inputPath, temporary)) {
            result.error = QStringLiteral("copy failed");
            return result;
        }
        if (cancelled(request)) {
            QFile::remove(temporary);
            result.cancelled = true;
            return result;
        }
        QFile::remove(request.outputPath);
        result.success = QFile::rename(temporary, request.outputPath);
        if (!result.success) {
            QFile::remove(temporary);
            result.error = QStringLiteral("copy publish failed");
        }
        return result;
    }

    result.error = QStringLiteral("unsupported media I/O request");
    return result;
}

void MediaIoScheduler::publishResult(MediaIoResult result)
{
    std::lock_guard lock(m_mutex);
    if (m_results.size() >= m_configuration.resultCapacity) {
        m_results.pop_front();
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    m_results.push_back(std::move(result));
}

bool MediaIoScheduler::requestIsStale(const MediaIoRequest& request) const noexcept
{
    return request.generation != 0
        && request.generation != currentGeneration(request.ownerId);
}

std::size_t MediaIoScheduler::queueDepthLocked() const noexcept
{
    std::size_t depth = 0;
    for (const auto& queue : m_queues)
        depth += queue.size();
    return depth;
}

std::size_t MediaIoScheduler::priorityIndex(MediaIoPriority priority) noexcept
{
    const auto index = static_cast<std::size_t>(priority);
    return std::min(index, kPriorityCount - 1);
}
