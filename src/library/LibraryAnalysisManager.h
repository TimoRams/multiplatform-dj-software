#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "TrackData.h"
#include "WaveformAnalyzer.h"

class LibraryDatabase;

class LibraryAnalysisManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(int total READ total NOTIFY progressChanged)
    Q_PROPERTY(int completed READ completed NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY progressChanged)

public:
    explicit LibraryAnalysisManager(QObject* parent = nullptr);
    ~LibraryAnalysisManager() override;

    void setLibraryDatabase(LibraryDatabase* db);

    [[nodiscard]] bool running() const { return m_running; }
    [[nodiscard]] int total() const { return m_total; }
    [[nodiscard]] int completed() const { return m_completed; }
    [[nodiscard]] double progress() const;
    [[nodiscard]] QString currentTitle() const { return m_currentTitle; }

    Q_INVOKABLE void analyzeAll(bool includeAnalyzed = true);
    Q_INVOKABLE void analyzePlaylist(const QString& playlistId, bool includeAnalyzed = true);
    Q_INVOKABLE void analyzeTrack(const QString& trackId,
                                  const QString& filePath,
                                  const QString& title = QString());
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void progressChanged();

private:
    struct QueueItem {
        QString trackId;
        QString filePath;
        QString title;
    };

    void enqueue(const QVariantList& items);
    void startNext();
    void finishCurrent(bool completed);
    static QueueItem queueItemFromMap(const QVariantMap& map);

    LibraryDatabase* m_db = nullptr;
    juce::AudioFormatManager m_formatManager;

    std::vector<QueueItem> m_queue;
    QueueItem m_current;
    int m_total = 0;
    int m_completed = 0;
    bool m_running = false;
    bool m_cancelRequested = false;
    QString m_currentTitle;

    std::unique_ptr<TrackData> m_trackData;
    std::unique_ptr<WaveformAnalyzer> m_analyzer;
};
