#include "MixTracklistController.h"

#include "audio/AudioEngine.h"
#include "audio/SetRecorder.h"
#include "app/SettingsManager.h"
#include "deck/DjEngine.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

namespace {

QString fromStd(const std::string& text) { return QString::fromStdString(text); }

bool saveText(const QString& path, const std::string& text)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    file.write(text.data(), static_cast<qint64>(text.size()));
    return file.commit();
}

bool saveBytes(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(bytes);
    return file.commit();
}

// Self-contained OBS browser-source overlay. Browsers block fetch() on file://,
// so the page re-injects nowplaying.js (which assigns a global) once a second.
constexpr const char* kOverlayHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>BrockDJ Now Playing</title>
<style>
html,body{margin:0;background:transparent;font-family:"Inter","Segoe UI",sans-serif;overflow:hidden}
#card{position:absolute;left:24px;bottom:24px;max-width:80vw;padding:14px 22px;border-radius:12px;
background:rgba(12,14,18,.82);color:#fff;border-left:4px solid #22d3ee;opacity:0;transform:translateY(12px);
transition:opacity .5s,transform .5s}
#card.show{opacity:1;transform:none}
#label{font-size:12px;letter-spacing:.18em;color:#22d3ee;text-transform:uppercase}
#title{font-size:28px;font-weight:700;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#artist{font-size:18px;color:#cbd5e1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
</style></head><body>
<div id="card"><div id="label">Now playing</div><div id="title"></div><div id="artist"></div></div>
<script>
let last = "";
function apply() {
  const np = window.brockdjNowPlaying || {};
  const id = (np.artist || "") + "\n" + (np.title || "");
  if (id === last) return;
  last = id;
  const card = document.getElementById("card");
  card.classList.remove("show");
  setTimeout(() => {
    document.getElementById("title").textContent = np.title || "";
    document.getElementById("artist").textContent = np.artist || "";
    if (np.title) card.classList.add("show");
  }, 450);
}
function poll() {
  const old = document.getElementById("np");
  const s = document.createElement("script");
  s.id = "np";
  s.src = "nowplaying.js?t=" + Date.now();
  s.onload = s.onerror = apply;
  if (old) old.remove();
  document.body.appendChild(s);
}
poll();
setInterval(poll, 1000);
</script></body></html>
)HTML";

QString sidecarPath(const QString& audioPath, const char* suffix)
{
    const QFileInfo info(audioPath);
    return info.absolutePath() + "/" + info.completeBaseName() + suffix;
}

} // namespace

MixTracklistController::MixTracklistController(SetRecorder& recorder, std::array<DjEngine*, 4> decks,
                                               SettingsManager* settings, QObject* parent)
    : QObject(parent), m_recorder(recorder), m_settings(settings), m_decks(decks)
{
    if (m_settings) {
        m_exportEnabled = m_settings->getUiState("nowPlaying/exportEnabled", "0") == "1";
        m_exportFolder = m_settings->getUiState("nowPlaying/exportFolder");
    }
    m_clock.start();
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &MixTracklistController::poll);
    connect(&m_recorder, &SetRecorder::changed, this, &MixTracklistController::onRecorderChanged);
    m_timer.start();
    if (m_exportEnabled)
        writeNowPlaying();
}

QString MixTracklistController::nowPlayingExportPath() const
{
    if (!m_exportFolder.trimmed().isEmpty())
        return QDir::cleanPath(m_exportFolder.trimmed());
    if (m_settings)
        return m_settings->getConfigDirectoryPath() + "/NowPlaying";
    return QDir::tempPath() + "/BrockDJ-NowPlaying";
}

void MixTracklistController::setNowPlayingExportEnabled(bool enabled)
{
    if (m_exportEnabled == enabled)
        return;
    m_exportEnabled = enabled;
    if (m_settings)
        m_settings->setUiState("nowPlaying/exportEnabled", enabled ? "1" : "0");
    m_exportError.clear();
    if (enabled)
        writeNowPlaying();
    emit exportChanged();
}

void MixTracklistController::setNowPlayingExportFolder(const QString& folder)
{
    if (m_exportFolder == folder)
        return;
    m_exportFolder = folder;
    if (m_settings)
        m_settings->setUiState("nowPlaying/exportFolder", folder);
    m_exportError.clear();
    if (m_exportEnabled)
        writeNowPlaying();
    emit exportChanged();
}

void MixTracklistController::writeNowPlaying()
{
    const QString folder = nowPlayingExportPath();
    const QString title = nowPlayingTitle();
    const QString artist = nowPlayingArtist();
    QJsonObject json {
        {"title", title},
        {"artist", artist},
        {"deck", nowPlayingDeck() >= 0 ? QString(QChar('A' + nowPlayingDeck())) : QString()},
        {"bpm", m_nowPlayingBpm > 0 ? QJsonValue(std::round(m_nowPlayingBpm * 10.0) / 10.0) : QJsonValue()},
        {"key", m_nowPlayingKey},
        {"updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
    };
    const QByteArray jsonBytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
    const QString line = artist.isEmpty() ? title : artist + " - " + title;
    const QString overlay = folder + "/overlay.html";
    const bool ok = QDir().mkpath(folder)
        && saveBytes(folder + "/nowplaying.txt", line.toUtf8())
        && saveBytes(folder + "/nowplaying.json", jsonBytes)
        && saveBytes(folder + "/nowplaying.js", "window.brockdjNowPlaying = " + jsonBytes + ";\n")
        && (QFileInfo::exists(overlay) || saveBytes(overlay, kOverlayHtml));
    const QString error = ok ? QString() : QStringLiteral("Now-playing files could not be written to ") + folder;
    if (error != m_exportError) {
        m_exportError = error;
        emit exportChanged();
    }
}

QString MixTracklistController::nowPlayingTitle() const
{
    const auto& current = m_tracker.current();
    if (!current)
        return {};
    return current->title.empty() ? QFileInfo(fromStd(current->key)).completeBaseName()
                                  : fromStd(current->title);
}

QString MixTracklistController::nowPlayingArtist() const
{
    const auto& current = m_tracker.current();
    return current ? fromStd(current->artist) : QString();
}

QString MixTracklistController::nowPlayingFilePath() const
{
    const auto& current = m_tracker.current();
    return current ? fromStd(current->key) : QString();
}

int MixTracklistController::nowPlayingDeck() const
{
    const auto& current = m_tracker.current();
    return current ? current->deck : -1;
}

QString MixTracklistController::tracklistPath() const
{
    return m_audioPath.isEmpty() ? QString() : sidecarPath(m_audioPath, ".txt");
}

void MixTracklistController::poll()
{
    std::vector<brockdj::tracklist::DeckObservation> observations;
    observations.reserve(m_decks.size());
    for (std::size_t i = 0; i < m_decks.size(); ++i) {
        brockdj::tracklist::DeckObservation obs;
        const DjEngine* deck = m_decks[i];
        if (deck && deck->hasTrack() && deck->isPlaying()) {
            obs.key = deck->trackFilePath().toStdString();
            obs.title = deck->trackTitle().toStdString();
            obs.artist = deck->trackArtist().toStdString();
            obs.airGain = AudioEngine::deckAirGain(static_cast<int>(i));
        }
        observations.push_back(std::move(obs));
    }

    const int previousDeck = nowPlayingDeck();
    auto entry = m_tracker.update(now(), observations);
    if (!entry) {
        if (previousDeck != nowPlayingDeck())
            emit nowPlayingChanged();
        return;
    }
    if (const DjEngine* deck = m_decks[static_cast<std::size_t>(entry->deck)]) {
        m_nowPlayingBpm = deck->getCurrentBpm();
        m_nowPlayingKey = deck->trackKey();
    }
    emit nowPlayingChanged();
    if (m_exportEnabled)
        writeNowPlaying();
    if (!m_recordingActive)
        return;
    entry->startSeconds = std::max(0.0, entry->startSeconds - m_recordingStart);
    m_entries.push_back(*entry);
    writeFiles();
}

void MixTracklistController::onRecorderChanged()
{
    const bool recording = m_recorder.recording();
    if (recording && !m_recordingActive) {
        m_recordingActive = true;
        m_audioPath = m_recorder.filePath();
        m_recordingStart = now() - m_recorder.elapsedSeconds();
        m_entries.clear();
        m_error.clear();
        // A track already on air when recording starts opens the list at 00:00.
        if (const auto& current = m_tracker.current();
            current && current->deck >= 0
            && AudioEngine::deckAirGain(current->deck) >= brockdj::tracklist::AudibleTrackTracker::kMinimumGain) {
            auto first = *current;
            first.startSeconds = 0;
            m_entries.push_back(first);
            writeFiles();
        }
        emit entriesChanged();
    } else if (!recording && m_recordingActive) {
        m_recordingActive = false;
        if (!m_entries.empty())
            writeFiles();
    }
}

void MixTracklistController::writeFiles()
{
    if (m_audioPath.isEmpty())
        return;
    const QFileInfo audio(m_audioPath);
    const std::string cue = brockdj::tracklist::formatCue(
        m_entries, audio.fileName().toStdString(), audio.completeBaseName().toStdString());
    const bool ok = saveText(sidecarPath(m_audioPath, ".cue"), cue)
        && saveText(sidecarPath(m_audioPath, ".txt"), brockdj::tracklist::formatText(m_entries));
    m_error = ok ? QString() : QStringLiteral("Tracklist could not be written next to the recording");
    emit entriesChanged();
}
