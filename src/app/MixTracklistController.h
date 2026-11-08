#pragma once

#include "audio/MixTracklist.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <array>
#include <vector>

class DjEngine;
class SetRecorder;
class SettingsManager;

// Tracks the audible track from routing gain (channel fader × crossfader) and,
// while the set recorder runs, writes <recording>.cue and <recording>.txt next
// to the WAV. Files are rewritten on every change so a crash keeps the list.
class MixTracklistController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString nowPlayingTitle READ nowPlayingTitle NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString nowPlayingArtist READ nowPlayingArtist NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString nowPlayingFilePath READ nowPlayingFilePath NOTIFY nowPlayingChanged)
    Q_PROPERTY(int nowPlayingDeck READ nowPlayingDeck NOTIFY nowPlayingChanged)
    Q_PROPERTY(int entryCount READ entryCount NOTIFY entriesChanged)
    Q_PROPERTY(QString tracklistPath READ tracklistPath NOTIFY entriesChanged)
    Q_PROPERTY(QString error READ error NOTIFY entriesChanged)
    // Now-playing export for stream overlays (OBS browser/text source).
    Q_PROPERTY(bool nowPlayingExportEnabled READ nowPlayingExportEnabled WRITE setNowPlayingExportEnabled NOTIFY exportChanged)
    Q_PROPERTY(QString nowPlayingExportFolder READ nowPlayingExportFolder WRITE setNowPlayingExportFolder NOTIFY exportChanged)
    Q_PROPERTY(QString nowPlayingExportPath READ nowPlayingExportPath NOTIFY exportChanged)
    Q_PROPERTY(QString nowPlayingExportError READ nowPlayingExportError NOTIFY exportChanged)
public:
    static constexpr double kHoldSeconds = 8.0;

    MixTracklistController(SetRecorder& recorder, std::array<DjEngine*, 4> decks,
                           SettingsManager* settings = nullptr, QObject* parent = nullptr);

    QString nowPlayingTitle() const;
    QString nowPlayingArtist() const;
    QString nowPlayingFilePath() const;
    int nowPlayingDeck() const;
    int entryCount() const { return static_cast<int>(m_entries.size()); }
    QString tracklistPath() const;
    QString error() const { return m_error; }
    bool nowPlayingExportEnabled() const { return m_exportEnabled; }
    void setNowPlayingExportEnabled(bool enabled);
    QString nowPlayingExportFolder() const { return m_exportFolder; }
    void setNowPlayingExportFolder(const QString& folder);
    QString nowPlayingExportPath() const;
    QString nowPlayingExportError() const { return m_exportError; }

signals:
    void nowPlayingChanged();
    void entriesChanged();
    void exportChanged();

private:
    void poll();
    void onRecorderChanged();
    void writeFiles();
    void writeNowPlaying();
    double now() const { return m_clock.elapsed() * 0.001; }

    SetRecorder& m_recorder;
    SettingsManager* m_settings = nullptr;
    bool m_exportEnabled = false;
    QString m_exportFolder;
    QString m_exportError;
    double m_nowPlayingBpm = 0;
    QString m_nowPlayingKey;
    std::array<DjEngine*, 4> m_decks;
    brockdj::tracklist::AudibleTrackTracker m_tracker {kHoldSeconds};
    std::vector<brockdj::tracklist::Entry> m_entries;
    QElapsedTimer m_clock;
    QTimer m_timer;
    bool m_recordingActive = false;
    double m_recordingStart = 0;
    QString m_audioPath;
    QString m_error;
};
