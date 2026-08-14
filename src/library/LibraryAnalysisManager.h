#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QTimer>
#include <memory>
#include <vector>
#include "analysis/AnalysisJobQueue.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "TrackData.h"
#include "waveform/WaveformAnalyzer.h"

class LibraryDatabase;

class LibraryAnalysisManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(int total READ total NOTIFY progressChanged)
    Q_PROPERTY(int completed READ completed NOTIFY progressChanged)
    Q_PROPERTY(int failed READ failed NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(double currentProgress READ currentProgress NOTIFY progressChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY progressChanged)

public:
    explicit LibraryAnalysisManager(QObject* parent = nullptr);
    ~LibraryAnalysisManager() override;

    void setLibraryDatabase(LibraryDatabase* db);

    [[nodiscard]] bool running() const { return m_running; }
    [[nodiscard]] int total() const { return m_total; }
    [[nodiscard]] int completed() const { return m_completed; }
    [[nodiscard]] int failed() const { return m_failed; }
    [[nodiscard]] double progress() const;
    [[nodiscard]] double currentProgress() const { return m_currentProgress; }
    [[nodiscard]] QString currentTitle() const { return m_currentTitle; }

    Q_INVOKABLE void analyzeAll(bool includeAnalyzed = false);
    Q_INVOKABLE void analyzePlaylist(const QString& playlistId, bool includeAnalyzed = false);
    Q_INVOKABLE void analyzeTrack(const QString& trackId,
                                  const QString& filePath,
                                  const QString& title = QString());
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void progressChanged();

private:
    using QueueItem = analysis::AnalysisJob;

    void enqueue(const QVariantList& items, analysis::AnalysisPriority priority);
    void startNext();
    void finishCurrent(bool completed);
    void drainAnalysisMailbox();
    static QueueItem queueItemFromMap(const QVariantMap& map);

    LibraryDatabase* m_db = nullptr;
    juce::AudioFormatManager m_formatManager;

    analysis::AnalysisJobQueue m_queue{4096, 16};
    QueueItem m_current;
    int m_total = 0;
    int m_completed = 0;
    int m_failed = 0;
    double m_currentProgress = 0.0;
    bool m_running = false;
    bool m_cancelRequested = false;
    QString m_currentTitle;

    std::unique_ptr<TrackData> m_trackData;
    std::unique_ptr<WaveformAnalyzer> m_analyzer;
    std::shared_ptr<AnalyzerResultMailbox> m_analysisMailbox;
    QTimer m_resultDrainTimer;
    WaveformAnalyzer::AnalysisGeneration m_currentGeneration = 0;
    std::uint64_t m_jobGeneration = 0;
};
