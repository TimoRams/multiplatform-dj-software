#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Reader for the Rekordbox "Export Collection in xml format" file
// (DJ_PLAYLISTS / COLLECTION / PLAYLISTS). Pure parsing, no database access.
namespace brockdj::library {

struct RekordboxXmlTempo {
    double positionSec = 0.0;
    double bpm = 0.0;
    int beatInBar = 1;       // 1..4, beat number of the node itself
};

struct RekordboxXmlMark {
    QString name;
    double startSec = 0.0;
    double endSec = -1.0;    // loops only
    int hotCueIndex = -1;    // 0..7 = pad A..H, -1 = memory cue/loop
    bool loop = false;
    QString color;           // "#rrggbb" or empty
};

struct RekordboxXmlTrack {
    QString key;             // TrackID used by playlists
    QString path;            // local file path
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString comment;
    QString musicalKey;      // Camelot ("8A") when the tonality could be converted
    QString color;           // "#rrggbb" or empty
    int durationSec = 0;
    int bitrateKbps = 0;
    int rating = 0;          // 0..5
    int playCount = 0;
    qint64 dateAdded = 0;    // Unix seconds, 0 = unknown
    double bpm = 0.0;
    QList<RekordboxXmlTempo> tempos;
    QList<RekordboxXmlMark> marks;
};

struct RekordboxXmlNode {
    QString name;
    bool folder = false;
    QList<RekordboxXmlNode> children;
    QStringList trackKeys;   // TrackIDs, or paths for location-keyed playlists
};

struct RekordboxXmlLibrary {
    QList<RekordboxXmlTrack> tracks;
    QList<RekordboxXmlNode> playlists;   // children of the ROOT node
};

bool readRekordboxXml(const QString& path, RekordboxXmlLibrary& out, QString* error = nullptr);
bool parseRekordboxXml(const QByteArray& data, RekordboxXmlLibrary& out, QString* error = nullptr);

// "file://localhost/C:/Music/a%20b.mp3" -> "C:/Music/a b.mp3"; "/Users/x" stays.
QString rekordboxLocationToPath(const QString& location);
// "Am" -> "8A", "F#" -> "2B", "8A" stays, unknown -> "".
QString musicalKeyToCamelot(const QString& key);

// Expands tempo nodes into beat positions (seconds) with their beat numbers 1..4,
// up to `durationSec`.
struct RekordboxBeat {
    double positionSec = 0.0;
    int beatInBar = 1;
    double bpm = 0.0;
};
QList<RekordboxBeat> expandRekordboxBeats(const QList<RekordboxXmlTempo>& tempos, double durationSec);

} // namespace brockdj::library
