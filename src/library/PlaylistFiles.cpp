#include "library/PlaylistFiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSaveFile>
#include <QStringDecoder>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace brockdj::library {
namespace {

void setError(QString* error, const QString& message)
{
    if (error)
        *error = message;
}

QString decodeText(const QByteArray& bytes)
{
    QByteArray data = bytes;
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8.decode(data);
    // Classic .m3u files from Windows tools are often Latin-1.
    if (utf8.hasError())
        text = QString::fromLatin1(data);
    return text;
}

bool isRemote(const QString& location)
{
    const qsizetype scheme = location.indexOf(QStringLiteral("://"));
    return scheme > 1 && !location.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive);
}

QString resolveLocation(QString location, const QDir& base)
{
    location = location.trimmed();
    if (location.isEmpty() || isRemote(location))
        return {};
    if (location.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
        return QDir::cleanPath(QUrl(location).toLocalFile());
    // Windows separators in relative entries.
    if (QDir::separator() == u'/' && !QDir::isAbsolutePath(location))
        location.replace(u'\\', u'/');
    return QDir::cleanPath(base.absoluteFilePath(location));
}

void splitArtistTitle(const QString& text, PlaylistFileEntry& entry)
{
    const qsizetype dash = text.indexOf(QStringLiteral(" - "));
    if (dash > 0) {
        entry.artist = text.left(dash).trimmed();
        entry.title = text.mid(dash + 3).trimmed();
    } else {
        entry.title = text.trimmed();
    }
}

QList<PlaylistFileEntry> readM3u(const QString& text, const QDir& base)
{
    QList<PlaylistFileEntry> entries;
    PlaylistFileEntry pending;
    bool hasInfo = false;
    for (QString line : text.split(u'\n')) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QStringLiteral("#EXTINF:"), Qt::CaseInsensitive)) {
            const QString info = line.mid(8);
            const qsizetype comma = info.indexOf(u',');
            pending = {};
            bool ok = false;
            // Attributes (tvg-… etc.) may follow the duration before the comma.
            const int duration = info.left(comma < 0 ? info.size() : comma)
                                     .section(u' ', 0, 0).toInt(&ok);
            pending.durationSec = ok && duration >= 0 ? duration : -1;
            if (comma >= 0)
                splitArtistTitle(info.mid(comma + 1), pending);
            hasInfo = true;
            continue;
        }
        if (line.startsWith(u'#'))
            continue;
        const QString path = resolveLocation(line, base);
        if (!path.isEmpty()) {
            PlaylistFileEntry entry = hasInfo ? pending : PlaylistFileEntry{};
            entry.path = path;
            entries.append(entry);
        }
        hasInfo = false;
        pending = {};
    }
    return entries;
}

QList<PlaylistFileEntry> readPls(const QString& text, const QDir& base)
{
    QMap<int, PlaylistFileEntry> byIndex;
    for (QString line : text.split(u'\n')) {
        line = line.trimmed();
        const qsizetype eq = line.indexOf(u'=');
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed().toLower();
        const QString value = line.mid(eq + 1).trimmed();
        for (const QString prefix : {QStringLiteral("file"), QStringLiteral("title"), QStringLiteral("length")}) {
            if (!key.startsWith(prefix))
                continue;
            bool ok = false;
            const int index = key.mid(prefix.size()).toInt(&ok);
            if (!ok)
                break;
            PlaylistFileEntry& entry = byIndex[index];
            if (prefix == QStringLiteral("file")) {
                entry.path = resolveLocation(value, base);
            } else if (prefix == QStringLiteral("title")) {
                splitArtistTitle(value, entry);
            } else {
                const int length = value.toInt(&ok);
                entry.durationSec = ok && length >= 0 ? length : -1;
            }
            break;
        }
    }
    QList<PlaylistFileEntry> entries;
    for (const PlaylistFileEntry& entry : std::as_const(byIndex)) {
        if (!entry.path.isEmpty())
            entries.append(entry);
    }
    return entries;
}

QList<PlaylistFileEntry> readXspf(const QByteArray& data, const QDir& base, QString* error)
{
    QList<PlaylistFileEntry> entries;
    QXmlStreamReader xml(data);
    PlaylistFileEntry current;
    bool inTrack = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QStringLiteral("track")) {
                inTrack = true;
                current = {};
            } else if (inTrack && name == QStringLiteral("location") && current.path.isEmpty()) {
                const QString location = xml.readElementText().trimmed();
                const QUrl url(location);
                if (url.isLocalFile())
                    current.path = QDir::cleanPath(url.toLocalFile());
                else if (url.isRelative())
                    current.path = resolveLocation(QUrl::fromPercentEncoding(location.toUtf8()), base);
            } else if (inTrack && name == QStringLiteral("title")) {
                current.title = xml.readElementText().trimmed();
            } else if (inTrack && name == QStringLiteral("creator")) {
                current.artist = xml.readElementText().trimmed();
            } else if (inTrack && name == QStringLiteral("duration")) {
                bool ok = false;
                const qint64 ms = xml.readElementText().trimmed().toLongLong(&ok);
                current.durationSec = ok && ms >= 0 ? static_cast<int>(ms / 1000) : -1;
            }
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("track")) {
            inTrack = false;
            if (!current.path.isEmpty())
                entries.append(current);
        }
    }
    if (xml.hasError()) {
        setError(error, QStringLiteral("XSPF: %1").arg(xml.errorString()));
        return {};
    }
    return entries;
}

QByteArray writeM3u(const QList<PlaylistFileEntry>& entries)
{
    QString out = QStringLiteral("#EXTM3U\n");
    for (const PlaylistFileEntry& e : entries) {
        const QString label = e.artist.isEmpty() ? e.title : e.artist + QStringLiteral(" - ") + e.title;
        out += QStringLiteral("#EXTINF:%1,%2\n").arg(e.durationSec >= 0 ? e.durationSec : -1).arg(label);
        out += QDir::toNativeSeparators(e.path) + u'\n';
    }
    return out.toUtf8();
}

QByteArray writePls(const QList<PlaylistFileEntry>& entries)
{
    QString out = QStringLiteral("[playlist]\n");
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const PlaylistFileEntry& e = entries[i];
        const QString label = e.artist.isEmpty() ? e.title : e.artist + QStringLiteral(" - ") + e.title;
        out += QStringLiteral("File%1=%2\n").arg(i + 1).arg(QDir::toNativeSeparators(e.path));
        out += QStringLiteral("Title%1=%2\n").arg(i + 1).arg(label);
        out += QStringLiteral("Length%1=%2\n").arg(i + 1).arg(e.durationSec >= 0 ? e.durationSec : -1);
    }
    out += QStringLiteral("NumberOfEntries=%1\nVersion=2\n").arg(entries.size());
    return out.toUtf8();
}

QByteArray writeXspf(const QList<PlaylistFileEntry>& entries)
{
    QByteArray data;
    QXmlStreamWriter xml(&data);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("playlist"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1"));
    xml.writeAttribute(QStringLiteral("xmlns"), QStringLiteral("http://xspf.org/ns/0/"));
    xml.writeStartElement(QStringLiteral("trackList"));
    for (const PlaylistFileEntry& e : entries) {
        xml.writeStartElement(QStringLiteral("track"));
        xml.writeTextElement(QStringLiteral("location"),
                             QUrl::fromLocalFile(e.path).toString(QUrl::FullyEncoded));
        if (!e.title.isEmpty())
            xml.writeTextElement(QStringLiteral("title"), e.title);
        if (!e.artist.isEmpty())
            xml.writeTextElement(QStringLiteral("creator"), e.artist);
        if (e.durationSec >= 0)
            xml.writeTextElement(QStringLiteral("duration"), QString::number(qint64(e.durationSec) * 1000));
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return data;
}

} // namespace

PlaylistFileFormat playlistFormatForPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("m3u") || suffix == QStringLiteral("m3u8"))
        return PlaylistFileFormat::M3U;
    if (suffix == QStringLiteral("pls"))
        return PlaylistFileFormat::PLS;
    if (suffix == QStringLiteral("xspf"))
        return PlaylistFileFormat::XSPF;
    return PlaylistFileFormat::Unknown;
}

QString localPathFromUrlOrPath(const QString& urlOrPath)
{
    if (urlOrPath.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
        return QUrl(urlOrPath).toLocalFile();
    return urlOrPath;
}

bool readPlaylistFile(const QString& path, QList<PlaylistFileEntry>& entries, QString* error)
{
    entries.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    const QDir base = QFileInfo(path).absoluteDir();
    switch (playlistFormatForPath(path)) {
    case PlaylistFileFormat::PLS:
        entries = readPls(decodeText(data), base);
        return true;
    case PlaylistFileFormat::XSPF: {
        QString xmlError;
        entries = readXspf(data, base, &xmlError);
        if (!xmlError.isEmpty()) {
            setError(error, xmlError);
            return false;
        }
        return true;
    }
    case PlaylistFileFormat::M3U:
        entries = readM3u(decodeText(data), base);
        return true;
    case PlaylistFileFormat::Unknown:
        break;
    }
    setError(error, QStringLiteral("Unbekanntes Playlist-Format"));
    return false;
}

bool writePlaylistFile(const QString& path, const QList<PlaylistFileEntry>& entries, QString* error)
{
    QByteArray data;
    switch (playlistFormatForPath(path)) {
    case PlaylistFileFormat::PLS: data = writePls(entries); break;
    case PlaylistFileFormat::XSPF: data = writeXspf(entries); break;
    case PlaylistFileFormat::M3U:
    case PlaylistFileFormat::Unknown: data = writeM3u(entries); break;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

} // namespace brockdj::library
