#pragma once

#include <QHash>
#include <QString>
#include <QVector>

namespace rekordbox {

enum class DeviceLibraryKind {
    GenericUsb = 0,
    LegacyPdb,
    DeviceLibraryPlus
};

struct Beat {
    double positionSec = 0.0;
    double bpm = 0.0;
    int beatInBar = 1;
};

enum class CueKind {
    MemoryCue = 0,
    HotCue,
    Loop
};

struct Cue {
    CueKind kind = CueKind::MemoryCue;
    int hotCueIndex = -1;
    double positionSec = 0.0;
    double loopEndSec = -1.0;
    QString label;
    QString color;
};

struct Analysis {
    QVector<Beat> beats;
    QVector<Cue> hotCues;
    QVector<Cue> memoryCues;
    QVector<Cue> loops;
};

struct Track {
    quint32 id = 0;
    QString sourceAwareId;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString key;
    QString comment;
    double bpm = 0.0;
    double durationSec = 0.0;
    int bitrateKbps = 0;
    int rating = 0;
    QString color;
    QString relativeAudioPath;
    QString relativeAnalysisPath;
    QString relativeArtworkPath;
    QString audioPath;
    QString analysisPath;
    QString artworkPath;
};

struct Playlist {
    quint32 id = 0;
    quint32 parentId = 0;
    quint32 sortOrder = 0;
    bool folder = false;
    QString name;
    QVector<quint32> trackIds;
};

struct DeviceIndex {
    QString deviceId;
    QString mountPath;
    QVector<Track> tracks;
    QVector<Playlist> playlists;
    QHash<quint32, qsizetype> trackById;
    QHash<QString, qsizetype> trackBySourceAwareId;
};

struct ReadResult {
    bool ok = false;
    QString error;
};

} // namespace rekordbox
