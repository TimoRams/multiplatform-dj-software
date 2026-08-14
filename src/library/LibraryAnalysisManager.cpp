#include "LibraryAnalysisManager.h"

#include "LibraryDatabase.h"
#include "analysis/AnalysisProgress.h"

#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>

LibraryAnalysisManager::LibraryAnalysisManager(QObject* parent)
    : QObject(parent)
{
    m_formatManager.registerBasicFormats();
    m_resultDrainTimer.setInterval(33);
    connect(&m_resultDrainTimer, &QTimer::timeout,
            this, &LibraryAnalysisManager::drainAnalysisMailbox);
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
    return analysis::aggregateProgress(m_completed, m_total, m_currentProgress);
}

void LibraryAnalysisManager::analyzeAll(bool includeAnalyzed)
{
    if (!m_db)
        return;

    enqueue(m_db->getAllTrackAnalysisItems(includeAnalyzed),
            analysis::AnalysisPriority::BackgroundLibrary);
}

void LibraryAnalysisManager::analyzePlaylist(const QString& playlistId, bool includeAnalyzed)
{
    if (!m_db || playlistId.isEmpty())
        return;

    enqueue(m_db->getPlaylistAnalysisItems(playlistId, includeAnalyzed),
            analysis::AnalysisPriority::VisibleLibrary);
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
    enqueue({item}, analysis::AnalysisPriority::UserSelected);
}

void LibraryAnalysisManager::cancel()
{
    const bool wasRunning = m_running;
    const bool hadProgress = m_total != 0 || m_completed != 0 || m_failed != 0
        || m_currentProgress != 0.0 || !m_currentTitle.isEmpty();
    ++m_jobGeneration;
    m_cancelRequested = true;
    m_queue.clear();

    if (m_analyzer) {
        m_analyzer->setCompletionCallback({});
        m_analyzer->requestCancel();
        m_analyzer->shutdownAndJoin();
    }

    m_analyzer.reset();
    m_trackData.reset();
    m_resultDrainTimer.stop();
    m_analysisMailbox.reset();

    m_running = false;
    m_total = 0;
    m_completed = 0;
    m_failed = 0;
    m_currentProgress = 0.0;
    m_currentTitle.clear();
    if (wasRunning)
        emit stateChanged();
    if (hadProgress)
        emit progressChanged();
}

void LibraryAnalysisManager::enqueue(const QVariantList& items, analysis::AnalysisPriority priority)
{
    cancel();

    m_cancelRequested = false;
    m_queue.clear();
    for (const QVariant& item : items) {
        const QueueItem q = queueItemFromMap(item.toMap());
        if (!q.trackId.isEmpty() && !q.filePath.isEmpty()) {
            auto queued = q;
            queued.priority = priority;
            const QFileInfo info(queued.filePath);
            queued.key = info.canonicalFilePath() + QLatin1Char('|')
                + QString::number(info.size()) + QLatin1Char('|')
                + QString::number(analysis::kCurrentAnalysisVersion);
            (void)m_queue.push(std::move(queued));
        }
    }

    m_total = static_cast<int>(m_queue.size());
    m_completed = 0;
    m_failed = 0;
    m_currentProgress = 0.0;
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
        m_resultDrainTimer.stop();
        m_analysisMailbox.reset();
        emit stateChanged();
        emit progressChanged();
        return;
    }

    const auto next = m_queue.pop();
    if (!next) return;
    m_current = *next;
    m_currentProgress = 0.0;
    m_currentTitle = !m_current.title.isEmpty()
        ? m_current.title
        : QFileInfo(m_current.filePath).completeBaseName();
    emit progressChanged();

    m_trackData = std::make_unique<TrackData>();
    m_analyzer = std::make_unique<WaveformAnalyzer>(
        &m_formatManager,
        600);

    ++m_jobGeneration;
    m_analysisMailbox = std::make_shared<AnalyzerResultMailbox>();
    const auto mailbox = m_analysisMailbox;
    m_analyzer->setProgressCallback([mailbox](double progress, bool active,
                                                  WaveformAnalyzer::AnalysisGeneration generation) {
        mailbox->publishProgress(progress, active, generation);
    });
    m_analyzer->setCompletionCallback([mailbox](bool completed,
                                                  WaveformAnalyzer::AnalysisGeneration generation,
                                                  const QString& filePath,
                                                  WaveformAnalyzer::ResultPtr result) {
        mailbox->publish({completed, generation, filePath, std::move(result)});
    });
    m_currentGeneration = m_analyzer->startAnalysis(
        m_current.filePath, 0.0, 0, m_trackData->createAnalysisSeed());
    m_resultDrainTimer.start();
}

void LibraryAnalysisManager::drainAnalysisMailbox()
{
    if (!m_analysisMailbox) return;

    double currentProgress = 0.0;
    bool active = false;
    WaveformAnalyzer::AnalysisGeneration progressGeneration = 0;
    if (m_analysisMailbox->takeProgress(currentProgress, active, progressGeneration)
        && progressGeneration == m_currentGeneration) {
        const double nextProgress = std::clamp(currentProgress, 0.0, 1.0);
        if (!qFuzzyCompare(m_currentProgress + 1.0, nextProgress + 1.0)) {
            m_currentProgress = nextProgress;
            emit progressChanged();
        }
    }

    const auto completion = m_analysisMailbox->take();
    if (completion) {
        if (completion->generation != m_currentGeneration
            || completion->filePath != m_current.filePath)
            return;
        if (completion->completed && completion->result && m_trackData)
            m_trackData->applyAnalysisResult(*completion->result);
        if (completion->completed && completion->result && m_db)
            (void)m_db->requestAnalysisPersistence(m_current.trackId, *completion->result);
        finishCurrent(completion->completed);
        return;
    }

    // A failed reader/validator normally publishes a negative completion. If a
    // callback is ever lost, still advance the queue once the worker stopped.
    if (m_analyzer && !m_analyzer->isThreadRunning())
        finishCurrent(false);
}

void LibraryAnalysisManager::finishCurrent(bool completed)
{
    if (m_cancelRequested)
        return;

    if (!completed) {
        ++m_failed;
        qWarning() << "[LibraryAnalysisManager] Analysis did not finish:" << m_current.filePath;
    }

    m_currentProgress = 1.0;
    ++m_completed;
    emit progressChanged();

    if (m_analyzer)
        m_analyzer->setCompletionCallback({});
    if (m_analyzer)
        m_analyzer->shutdownAndJoin();
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
