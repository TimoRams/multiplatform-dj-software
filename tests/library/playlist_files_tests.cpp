#include "library/PlaylistFiles.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

using namespace brockdj::library;

namespace {

void require(bool value, const QString& message)
{
    if (!value) {
        std::cerr << "FAIL: " << message.toStdString() << '\n';
        std::exit(1);
    }
}

void writeRaw(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), QStringLiteral("open ") + path);
    file.write(bytes);
}

QList<PlaylistFileEntry> read(const QString& path)
{
    QList<PlaylistFileEntry> entries;
    QString error;
    require(readPlaylistFile(path, entries, &error), QStringLiteral("read ") + path + QStringLiteral(": ") + error);
    return entries;
}

void roundTrip(const QDir& dir, const QString& name, const QList<PlaylistFileEntry>& source)
{
    const QString path = dir.filePath(name);
    QString error;
    require(writePlaylistFile(path, source, &error), QStringLiteral("write ") + name + QStringLiteral(": ") + error);
    const QList<PlaylistFileEntry> back = read(path);
    require(back.size() == source.size(), name + QStringLiteral(" count"));
    for (qsizetype i = 0; i < source.size(); ++i) {
        require(back[i].path == source[i].path, name + QStringLiteral(" path ") + back[i].path);
        require(back[i].title == source[i].title, name + QStringLiteral(" title ") + back[i].title);
        require(back[i].durationSec == source[i].durationSec,
                name + QStringLiteral(" duration ") + QString::number(back[i].durationSec));
    }
}

} // namespace

int main()
{
    QTemporaryDir temp;
    require(temp.isValid(), QStringLiteral("temp dir"));
    const QDir dir(temp.path());
    dir.mkpath(QStringLiteral("music/sub"));

    require(playlistFormatForPath(QStringLiteral("a.M3U8")) == PlaylistFileFormat::M3U, QStringLiteral("m3u8 fmt"));
    require(playlistFormatForPath(QStringLiteral("a.pls")) == PlaylistFileFormat::PLS, QStringLiteral("pls fmt"));
    require(playlistFormatForPath(QStringLiteral("a.xspf")) == PlaylistFileFormat::XSPF, QStringLiteral("xspf fmt"));
    require(localPathFromUrlOrPath(QStringLiteral("file:///tmp/a%20b.m3u")) == QStringLiteral("/tmp/a b.m3u"),
            QStringLiteral("file url"));

    const QList<PlaylistFileEntry> source{
        {dir.filePath(QStringLiteral("music/Ärger & Söhne.mp3")), QStringLiteral("Ärger & <Söhne>"),
         QStringLiteral("Bänd"), 245},
        {dir.filePath(QStringLiteral("music/sub/track, two.flac")), QStringLiteral("Track, Two"), QString(), 61},
    };
    roundTrip(dir, QStringLiteral("set.m3u8"), source);
    roundTrip(dir, QStringLiteral("set.pls"), source);
    roundTrip(dir, QStringLiteral("set.xspf"), source);

    // Hand-written M3U: BOM, CRLF, relative and URL entries, remote stream.
    writeRaw(dir.filePath(QStringLiteral("hand.m3u8")),
             QByteArray("\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:123,Artist - Title\r\nmusic/a.mp3\r\n"
                        "\r\nhttp://radio.example/stream\r\nfile://") +
                 QFile::encodeName(dir.filePath(QStringLiteral("music/b%20c.mp3"))) + "\r\n");
    QList<PlaylistFileEntry> hand = read(dir.filePath(QStringLiteral("hand.m3u8")));
    require(hand.size() == 2, QStringLiteral("hand count ") + QString::number(hand.size()));
    require(hand[0].path == dir.filePath(QStringLiteral("music/a.mp3")), QStringLiteral("relative ") + hand[0].path);
    require(hand[0].durationSec == 123, QStringLiteral("extinf duration"));
    require(hand[0].title == QStringLiteral("Title") && hand[0].artist == QStringLiteral("Artist"),
            QStringLiteral("extinf title ") + hand[0].title);
    require(hand[1].path == dir.filePath(QStringLiteral("music/b c.mp3")), QStringLiteral("url entry ") + hand[1].path);

    // Latin-1 .m3u from older tools.
    writeRaw(dir.filePath(QStringLiteral("old.m3u")), QByteArray("music/K\xF6ln.mp3\n"));
    hand = read(dir.filePath(QStringLiteral("old.m3u")));
    require(hand.size() == 1 && hand[0].path == dir.filePath(QStringLiteral("music/Köln.mp3")),
            QStringLiteral("latin1 ") + (hand.isEmpty() ? QString() : hand[0].path));

    // Windows-style separators in a relative entry.
    writeRaw(dir.filePath(QStringLiteral("win.m3u")), QByteArray("music\\sub\\x.mp3\n"));
    hand = read(dir.filePath(QStringLiteral("win.m3u")));
    require(hand.size() == 1 && hand[0].path == dir.filePath(QStringLiteral("music/sub/x.mp3")),
            QStringLiteral("windows sep ") + (hand.isEmpty() ? QString() : hand[0].path));

    // PLS with relative File entry, out-of-order keys.
    writeRaw(dir.filePath(QStringLiteral("hand.pls")),
             "[playlist]\nTitle2=Second\nFile1=music/a.mp3\nFile2=music/b.mp3\nLength1=10\nNumberOfEntries=2\n");
    hand = read(dir.filePath(QStringLiteral("hand.pls")));
    require(hand.size() == 2 && hand[1].title == QStringLiteral("Second") && hand[0].durationSec == 10,
            QStringLiteral("pls parse"));

    QList<PlaylistFileEntry> none;
    require(!readPlaylistFile(dir.filePath(QStringLiteral("missing.m3u")), none), QStringLiteral("missing file"));

    std::cout << "playlist_files: all checks passed\n";
    return 0;
}
