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

// Background colour a device was assigned in the exporting library software.
// The numbering matches the track-colour palette; 0 means the user left the
// device on the neutral default.
enum class DeviceColor {
    None = 0,
    Pink,
    Red,
    Orange,
    Yellow,
    Green,
    Aqua,
    Blue,
    Purple,
    // A value a newer exporter wrote that this build does not know yet. Kept
    // distinct from None so the UI can stay neutral without pretending the
    // device had no colour at all.
    Unknown
};

// How a device presents itself, independent of which track library it carries.
// A device that was never named in the exporting software leaves `name` empty
// and the caller falls back to the filesystem volume label.
struct DeviceIdentity {
    QString name;
    DeviceColor color = DeviceColor::None;
    // The value exactly as stored, so nothing is lost when a future exporter
    // introduces a colour this build cannot name.
    int rawColorType = 0;
};

struct ReadResult {
    bool ok = false;
    QString error;
};

} // namespace rekordbox
