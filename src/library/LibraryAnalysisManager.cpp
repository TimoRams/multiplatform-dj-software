#include "LibraryAnalysisManager.h"

#include "LibraryDatabase.h"

#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>

LibraryAnalysisManager::LibraryAnalysisManager(QObject* parent)
    : QObject(parent)
{
    m_formatManager.registerBasicFormats();
}

LibraryAnalysisManager::~LibraryAnalysisManager()
{
    cancel();
}

void LibraryAnalysisManager::setLibraryDatabase(LibraryDatabase* db)
{
    m_db = db;
}

double LibraryAnalysisManager::progress() const
{
    return m_total > 0 ? static_cast<double>(m_completed) / static_cast<double>(m_total) : 0.0;
}

void LibraryAnalysisManager::analyzeAll(bool includeAnalyzed)
{
    if (!m_db)
        return;

    enqueue(m_db->getAllTrackAnalysisItems(includeAnalyzed));
}

void LibraryAnalysisManager::analyzePlaylist(const QString& playlistId, bool includeAnalyzed)
{
    if (!m_db || playlistId.isEmpty())
        return;

    enqueue(m_db->getPlaylistAnalysisItems(playlistId, includeAnalyzed));
}

void LibraryAnalysisManager::analyzeTrack(const QString& trackId,
                                          const QString& filePath,
                                          const QString& title)
{
    if (trackId.isEmpty() || filePath.isEmpty())
        return;

    QVariantMap item;
    item.insert(QStringLiteral("trackId"), trackId);
    item.insert(QStringLiteral("filePath"), filePath);
    item.insert(QStringLiteral("title"), title);
    enqueue({item});
}

void LibraryAnalysisManager::cancel()
{
    m_cancelRequested = true;
    m_queue.clear();

    if (m_analyzer) {
        m_analyzer->setCompletionCallback({});
        m_analyzer->stopAnalysis();
    }

    m_analyzer.reset();
    m_trackData.reset();

    if (m_running) {
        m_running = false;
        m_currentTitle.clear();
        emit stateChanged();
        emit progressChanged();
    }
}

void LibraryAnalysisManager::enqueue(const QVariantList& items)
{
    cancel();

    m_cancelRequested = false;
    m_queue.clear();
    m_queue.reserve(static_cast<size_t>(items.size()));

    for (const QVariant& item : items) {
        const QueueItem q = queueItemFromMap(item.toMap());
        if (!q.trackId.isEmpty() && !q.filePath.isEmpty())
            m_queue.push_back(q);
    }

    m_total = static_cast<int>(m_queue.size());
    m_completed = 0;
    m_currentTitle.clear();
    m_running = m_total > 0;
    emit stateChanged();
    emit progressChanged();

    if (m_running)
        startNext();
}

void LibraryAnalysisManager::startNext()
{
    if (m_cancelRequested)
        return;

    if (m_queue.empty()) {
        m_running = false;
        m_currentTitle.clear();
        m_analyzer.reset();
        m_trackData.reset();
        emit stateChanged();
        emit progressChanged();
        return;
    }

    m_current = m_queue.front();
    m_queue.erase(m_queue.begin());
    m_currentTitle = !m_current.title.isEmpty()
        ? m_current.title
        : QFileInfo(m_current.filePath).completeBaseName();
    emit progressChanged();

    m_trackData = std::make_unique<TrackData>();
    m_analyzer = std::make_unique<WaveformAnalyzer>(
        m_trackData.get(),
        &m_formatManager,
        600);

    m_analyzer->setCompletionCallback([this](bool completed) {
        QMetaObject::invokeMethod(this, [this, completed]() {
            finishCurrent(completed);
        }, Qt::QueuedConnection);
    });
    m_analyzer->startAnalysis(m_current.filePath);
}

void LibraryAnalysisManager::finishCurrent(bool completed)
{
    if (m_cancelRequested)
        return;

    if (completed && m_db && m_trackData) {
        const double bpm = m_trackData->getBpm();
        const QString key = m_trackData->getDetectedKey().trimmed();
        m_db->updateAnalysisData(m_current.trackId,
                                 static_cast<float>(bpm),
                                 key,
                                 m_trackData->getFirstBeatSample(),
                                 m_trackData->getSampleRate(),
                                 m_trackData->getBeatGrid(),
                                 m_trackData->getConfidenceInfo(),
                                 m_trackData->getBeatGridInfo());

        const auto segments = m_trackData->getSegments();
        m_db->updateTrackSegments(m_current.trackId, segments);
    } else if (!completed) {
        qWarning() << "[LibraryAnalysisManager] Analysis did not finish:" << m_current.filePath;
    }

    ++m_completed;
    emit progressChanged();

    if (m_analyzer)
        m_analyzer->setCompletionCallback({});
    m_analyzer.reset();
    m_trackData.reset();

    startNext();
}

LibraryAnalysisManager::QueueItem LibraryAnalysisManager::queueItemFromMap(const QVariantMap& map)
{
    QueueItem item;
    item.trackId = map.value(QStringLiteral("trackId")).toString();
    item.filePath = map.value(QStringLiteral("filePath")).toString();
    item.title = map.value(QStringLiteral("title")).toString();
    return item;
}
