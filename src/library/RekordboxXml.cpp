#include "RekordboxXml.h"

#include <QDate>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

namespace brockdj::library {

namespace {

constexpr int kMaxBeats = 40'000;   // ~5 h at 130 BPM; guards malformed tempo lists

QString colorFromRgb(const QXmlStreamAttributes& a)
{
    if (!a.hasAttribute(QStringLiteral("Red")))
        return {};
    const auto channel = [&a](const char* name) {
        return std::clamp(a.value(QLatin1String(name)).toInt(), 0, 255);
    };
    return QStringLiteral("#%1%2%3")
        .arg(channel("Red"), 2, 16, QLatin1Char('0'))
        .arg(channel("Green"), 2, 16, QLatin1Char('0'))
        .arg(channel("Blue"), 2, 16, QLatin1Char('0'));
}

QString colorFromHex(QStringView value)
{
    QString v = value.trimmed().toString();
    if (v.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        v = v.mid(2);
    bool ok = false;
    const uint rgb = v.toUInt(&ok, 16);
    if (!ok || v.isEmpty())
        return {};
    return QStringLiteral("#%1").arg(rgb & 0xFFFFFF, 6, 16, QLatin1Char('0'));
}

double toDouble(QStringView value)
{
    bool ok = false;
    const double v = value.trimmed().toDouble(&ok);
    return ok && std::isfinite(v) ? v : 0.0;
}

RekordboxXmlTrack readTrack(const QXmlStreamAttributes& a)
{
    RekordboxXmlTrack t;
    t.key = a.value(QStringLiteral("TrackID")).toString();
    t.path = rekordboxLocationToPath(a.value(QStringLiteral("Location")).toString());
    t.title = a.value(QStringLiteral("Name")).toString().trimmed();
    t.artist = a.value(QStringLiteral("Artist")).toString().trimmed();
    t.album = a.value(QStringLiteral("Album")).toString().trimmed();
    t.genre = a.value(QStringLiteral("Genre")).toString().trimmed();
    t.comment = a.value(QStringLiteral("Comments")).toString().trimmed();
    t.musicalKey = musicalKeyToCamelot(a.value(QStringLiteral("Tonality")).toString());
    t.color = colorFromHex(a.value(QStringLiteral("Colour")));
    t.durationSec = a.value(QStringLiteral("TotalTime")).toInt();
    t.bitrateKbps = a.value(QStringLiteral("BitRate")).toInt();
    // Stars are stored as 0, 51, 102, 153, 204, 255.
    t.rating = std::clamp((a.value(QStringLiteral("Rating")).toInt() + 25) / 51, 0, 5);
    t.playCount = std::max(0, a.value(QStringLiteral("PlayCount")).toInt());
    const QDate added = QDate::fromString(a.value(QStringLiteral("DateAdded")).toString(), Qt::ISODate);
    if (added.isValid())
        t.dateAdded = added.startOfDay(QTimeZone::utc()).toSecsSinceEpoch();
    t.bpm = toDouble(a.value(QStringLiteral("AverageBpm")));
    return t;
}

} // namespace

QString rekordboxLocationToPath(const QString& location)
{
    QString value = location.trimmed();
    if (value.isEmpty())
        return {};
    if (value.startsWith(QStringLiteral("file://localhost/"), Qt::CaseInsensitive))
        value = value.mid(16);
    else if (value.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive))
        value = value.mid(7);
    value = QUrl::fromPercentEncoding(value.toUtf8());
    static const QRegularExpression drive(QStringLiteral("^/[A-Za-z]:[/\\\\]"));
    if (drive.match(value).hasMatch())
        value = value.mid(1);
    return value;
}

QString musicalKeyToCamelot(const QString& key)
{
    static const QRegularExpression camelot(QStringLiteral("^0?([1-9]|1[0-2])([AaBb])$"));
    const QString k = key.trimmed();
    if (const auto m = camelot.match(k); m.hasMatch())
        return m.captured(1) + m.captured(2).toUpper();

    static const QHash<QString, QString> table{
        {"G#m", "1A"}, {"Abm", "1A"}, {"D#m", "2A"}, {"Ebm", "2A"}, {"A#m", "3A"}, {"Bbm", "3A"},
        {"Fm", "4A"}, {"Cm", "5A"}, {"Gm", "6A"}, {"Dm", "7A"}, {"Am", "8A"}, {"Em", "9A"},
        {"Bm", "10A"}, {"F#m", "11A"}, {"Gbm", "11A"}, {"C#m", "12A"}, {"Dbm", "12A"},
        {"B", "1B"}, {"Cb", "1B"}, {"F#", "2B"}, {"Gb", "2B"}, {"C#", "3B"}, {"Db", "3B"},
        {"G#", "4B"}, {"Ab", "4B"}, {"D#", "5B"}, {"Eb", "5B"}, {"A#", "6B"}, {"Bb", "6B"},
        {"F", "7B"}, {"C", "8B"}, {"G", "9B"}, {"D", "10B"}, {"A", "11B"}, {"E", "12B"},
    };
    QString normalized = k;
    normalized.replace(QStringLiteral("minor"), QStringLiteral("m"), Qt::CaseInsensitive);
    normalized.remove(QStringLiteral("major"), Qt::CaseInsensitive);
    normalized.replace(QStringLiteral("min"), QStringLiteral("m"), Qt::CaseInsensitive);
    normalized.remove(QStringLiteral("maj"), Qt::CaseInsensitive);
    normalized.remove(u' ');
    if (!normalized.isEmpty())
        normalized[0] = normalized[0].toUpper();
    return table.value(normalized);
}

QList<RekordboxBeat> expandRekordboxBeats(const QList<RekordboxXmlTempo>& tempos, double durationSec)
{
    QList<RekordboxBeat> beats;
    for (qsizetype i = 0; i < tempos.size() && beats.size() < kMaxBeats; ++i) {
        const RekordboxXmlTempo& node = tempos[i];
        if (node.bpm <= 0.0 || node.bpm > 1000.0 || node.positionSec < 0.0)
            continue;
        const double end = i + 1 < tempos.size() ? tempos[i + 1].positionSec
                                                 : std::max(durationSec, node.positionSec);
        const double step = 60.0 / node.bpm;
        int beat = std::clamp(node.beatInBar, 1, 4);
        for (int n = 0; beats.size() < kMaxBeats; ++n) {
            const double position = node.positionSec + n * step;
            if (n > 0 && position >= end - 1e-6)
                break;
            if (!beats.isEmpty() && position <= beats.last().positionSec + 1e-6)
                break;
            beats.append({position, beat, node.bpm});
            beat = beat % 4 + 1;
        }
    }
    return beats;
}

bool parseRekordboxXml(const QByteArray& data, RekordboxXmlLibrary& out, QString* error)
{
    out = {};
    QXmlStreamReader xml(data);
    bool sawRoot = false;
    bool inCollection = false;
    RekordboxXmlTrack* track = nullptr;
    // Stack of open playlist nodes; the first entry is the ROOT node.
    QList<RekordboxXmlNode> nodes;
    QList<bool> nodeIsKeyedByPath;

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const auto name = xml.name();
            const auto a = xml.attributes();
            if (name == QLatin1String("DJ_PLAYLISTS")) {
                sawRoot = true;
            } else if (name == QLatin1String("COLLECTION")) {
                inCollection = true;
            } else if (name == QLatin1String("TRACK") && inCollection) {
                out.tracks.append(readTrack(a));
                track = &out.tracks.last();
            } else if (name == QLatin1String("TEMPO") && track) {
                track->tempos.append({toDouble(a.value(QStringLiteral("Inizio"))),
                                      toDouble(a.value(QStringLiteral("Bpm"))),
                                      a.value(QStringLiteral("Battito")).toInt()});
            } else if (name == QLatin1String("POSITION_MARK") && track) {
                const int type = a.value(QStringLiteral("Type")).toInt();
                if (type != 0 && type != 4)
                    continue;   // fade/load markers have no BrockDJ counterpart
                RekordboxXmlMark mark;
                mark.name = a.value(QStringLiteral("Name")).toString();
                mark.startSec = toDouble(a.value(QStringLiteral("Start")));
                mark.loop = type == 4 && a.hasAttribute(QStringLiteral("End"));
                mark.endSec = mark.loop ? toDouble(a.value(QStringLiteral("End"))) : -1.0;
                if (mark.loop && mark.endSec <= mark.startSec)
                    mark.loop = false;
                const int num = a.hasAttribute(QStringLiteral("Num")) ? a.value(QStringLiteral("Num")).toInt() : -1;
                mark.hotCueIndex = num >= 0 && num < 8 ? num : -1;
                mark.color = colorFromRgb(a);
                track->marks.append(mark);
            } else if (name == QLatin1String("NODE")) {
                RekordboxXmlNode node;
                node.name = a.value(QStringLiteral("Name")).toString();
                node.folder = a.value(QStringLiteral("Type")) == QLatin1String("0");
                nodes.append(node);
                nodeIsKeyedByPath.append(a.value(QStringLiteral("KeyType")) == QLatin1String("1"));
            } else if (name == QLatin1String("TRACK") && !nodes.isEmpty()) {
                const QString key = a.value(QStringLiteral("Key")).toString();
                if (!key.isEmpty())
                    nodes.last().trackKeys.append(nodeIsKeyedByPath.last() ? rekordboxLocationToPath(key) : key);
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const auto name = xml.name();
            if (name == QLatin1String("COLLECTION")) {
                inCollection = false;
            } else if (name == QLatin1String("TRACK")) {
                track = nullptr;
            } else if (name == QLatin1String("NODE") && !nodes.isEmpty()) {
                RekordboxXmlNode done = nodes.takeLast();
                nodeIsKeyedByPath.removeLast();
                if (!nodes.isEmpty())
                    nodes.last().children.append(done);
                else
                    out.playlists = done.children;   // ROOT itself is not a playlist
            }
        }
    }
    if (xml.hasError() || !sawRoot) {
        if (error)
            *error = xml.hasError() ? xml.errorString() : QStringLiteral("Keine Rekordbox-XML-Datei");
        return false;
    }
    return true;
}

bool readRekordboxXml(const QString& path, RekordboxXmlLibrary& out, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return parseRekordboxXml(file.readAll(), out, error);
}

} // namespace brockdj::library
