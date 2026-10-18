#include "library/devices/DeviceLibraryManager.h"
#include "library/devices/rekordbox/RekordboxDeviceSource.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>

#ifdef BROCKDJ_HAVE_SQLCIPHER
#include <sqlite3.h>
#endif

namespace {
bool check(bool value, const char* message)
{
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

#ifdef BROCKDJ_HAVE_SQLCIPHER
const char* key =
    "PRAGMA key='r8gddnr4k847830ar6cqzbkk0el6qytmb3trbbx805jm74vez64i5o8fnrqryqls';";

bool execute(const QString& path, const char* sql)
{
    sqlite3* db = nullptr;
    bool ok = sqlite3_open(path.toUtf8().constData(), &db) == SQLITE_OK;
    if (ok)
        ok = sqlite3_exec(db, key, nullptr, nullptr, nullptr) == SQLITE_OK
            && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ok)
        std::cerr << "Fixture SQL: " << sqlite3_errmsg(db) << '\n';
    sqlite3_close(db);
    return ok;
}

QMap<QString, QByteArray> snapshot(const QString& root)
{
    QMap<QString, QByteArray> result;
    QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (file.open(QIODevice::ReadOnly))
            result.insert(file.fileName(), QCryptographicHash::hash(
                file.readAll(), QCryptographicHash::Sha256)
                + QByteArray::number(QFileInfo(file).lastModified().toMSecsSinceEpoch())
                + QByteArray::number(static_cast<int>(file.permissions())));
    }
    return result;
}
#endif
}

bool runRekordboxModernTests()
{
#ifndef BROCKDJ_HAVE_SQLCIPHER
    return true;
#else
    QTemporaryDir fixture(QDir::currentPath() + QStringLiteral("/rekordbox-modern-XXXXXX"));
    if (!check(fixture.isValid(), "create modern fixture in project directory"))
        return false;
    const QString mount = fixture.path();
    QDir().mkpath(mount + QStringLiteral("/PIONEER/rekordbox"));
    QDir().mkpath(mount + QStringLiteral("/Contents/音楽"));
    QDir().mkpath(mount + QStringLiteral("/PIONEER/USBANLZ/001"));
    const QString audio = mount + QStringLiteral("/Contents/音楽/100% #?.mp3");
    const QString analysis = mount + QStringLiteral("/PIONEER/USBANLZ/001/ANLZ0000.DAT");
    const QString art = mount + QStringLiteral("/PIONEER/art.jpg");
    for (const auto& path : {audio, analysis, art}) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7)
            return check(false, "create modern media fixture");
    }
    const QString path = mount + QStringLiteral("/PIONEER/rekordbox/exportLibrary.db");
    // Minimal columns from pyrekordbox's documented OneLibrary schema, with
    // deliberately shuffled IDs and fractional BPM/duration.
    bool ok = check(execute(path,
        "CREATE TABLE property(deviceName TEXT,backGroundColorType INTEGER);"
        "INSERT INTO property VALUES('Modern USB',8);"
        "CREATE TABLE artist(artist_id INTEGER PRIMARY KEY,name TEXT);"
        "CREATE TABLE album(album_id INTEGER PRIMARY KEY,name TEXT);"
        "CREATE TABLE genre(genre_id INTEGER PRIMARY KEY,name TEXT);"
        "CREATE TABLE \"key\"(key_id INTEGER PRIMARY KEY,name TEXT);"
        "CREATE TABLE color(color_id INTEGER PRIMARY KEY,name TEXT);"
        "CREATE TABLE image(image_id INTEGER PRIMARY KEY,path TEXT);"
        "INSERT INTO artist VALUES(1,'アーティスト');"
        "INSERT INTO album VALUES(2,'Album');"
        "INSERT INTO genre VALUES(3,'House');"
        "INSERT INTO \"key\" VALUES(4,'8A');"
        "INSERT INTO color VALUES(5,'Green');"
        "INSERT INTO image VALUES(6,'/PIONEER/art.jpg');"
        "CREATE TABLE content(content_id INTEGER PRIMARY KEY,title TEXT,"
        "artist_id_artist INTEGER,album_id INTEGER,genre_id INTEGER,key_id INTEGER,"
        "color_id INTEGER,image_id INTEGER,djComment TEXT,bpmx100 INTEGER,length INTEGER,"
        "bitrate INTEGER,rating INTEGER,path TEXT,analysisDataFilePath TEXT);"
        "INSERT INTO content VALUES(10,'First',1,2,3,4,5,6,'Comment',12845,240123,"
        "320,4,'/Contents/音楽/100% #?.mp3','/PIONEER/USBANLZ/001/ANLZ0000.DAT');"
        "INSERT INTO content VALUES(20,'Missing',NULL,NULL,NULL,NULL,NULL,NULL,NULL,"
        "0,0,0,0,'/Contents/missing.mp3',NULL);"
        "CREATE TABLE playlist(playlist_id INTEGER PRIMARY KEY,playlist_id_parent INTEGER,"
        "sequenceNo INTEGER,attribute INTEGER,name TEXT);"
        "INSERT INTO playlist VALUES(9,0,2,0,'Root');"
        "INSERT INTO playlist VALUES(8,NULL,1,1,'Folder');"
        "INSERT INTO playlist VALUES(7,8,2,0,'Child');"
        "INSERT INTO playlist VALUES(6,8,1,4,'Smart');"
        "CREATE TABLE playlist_content(playlist_id INTEGER,content_id INTEGER,sequenceNo INTEGER);"
        "INSERT INTO playlist_content VALUES(7,10,2),(7,20,1),(6,10,1);"),
        "create encrypted OneLibrary fixture");
    if (!ok)
        return false;
    QFile encrypted(path);
    ok &= check(encrypted.open(QIODevice::ReadOnly)
                    && !encrypted.read(16).startsWith("SQLite format 3"),
                "fixture really is SQLCipher encrypted");
    encrypted.close();
    const auto permissions = QFile::permissions(path);
    ok &= check(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadGroup
                                              | QFileDevice::ReadOther),
                "make modern database read-only");
    const auto before = snapshot(mount);
    auto result = rekordbox::DeviceSource{}.readIndexReadOnly(mount, QStringLiteral("usb"));
    ok &= check(result.ok, qPrintable(result.error));
    if (result.ok) {
        const auto& index = result.index;
        const auto& track = index.tracks.front();
        ok &= check(index.tracks.size() == 2 && track.id == 10
                        && track.sourceAwareId == QStringLiteral("rekordbox:usb:10")
                        && index.trackBySourceAwareId.value(track.sourceAwareId, -1) == 0,
                    "modern track identity and lookup");
        ok &= check(track.artist == QStringLiteral("アーティスト")
                        && track.album == QStringLiteral("Album")
                        && track.genre == QStringLiteral("House")
                        && track.key == QStringLiteral("8A")
                        && track.comment == QStringLiteral("Comment")
                        && track.color == QStringLiteral("Green")
                        && track.bpm == 128.45 && track.durationSec == 240.123
                        && track.rating == 4 && track.bitrateKbps == 320,
                    "modern metadata joins and units");
        ok &= check(track.audioPath == audio && track.analysisPath == analysis
                        && track.artworkPath == art
                        && index.tracks[1].audioPath.isEmpty()
                        && index.tracks[1].analysisPath.isEmpty(),
                    "modern literal Unicode paths and unavailable media");
        ok &= check(index.playlists.size() == 4 && index.playlists[0].id == 8
                        && index.playlists[0].folder && index.playlists[1].id == 6
                        && !index.playlists[1].folder && index.playlists[2].id == 7
                        && index.playlists[3].id == 9
                        && index.playlists[2].trackIds == QVector<quint32>{20, 10},
                    "parent-first folders, smart playlists and membership order");
    }
    {
        DeviceLibraryManager manager(false, nullptr);
        manager.inspectTestMounts({mount});
        QElapsedTimer wait;
        wait.start();
        while (manager.busy() && wait.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        const auto devices = manager.devices();
        ok &= check(!manager.busy() && devices.size() == 1
                        && devices[0].toMap().value(QStringLiteral("status")).toString()
                               .contains(QStringLiteral("2 tracks")),
                    "modern-only device is discovered and indexed asynchronously");
        if (!devices.isEmpty()) {
            manager.chooseDevice(devices[0].toMap().value(QStringLiteral("id")).toString());
            manager.chooseTracks();
            ok &= check(manager.currentTracks().size() == 2
                            && manager.currentArtists().size() == 2,
                        "modern device track and artist browsing");
            manager.choosePlaylist(QStringLiteral("7"));
            ok &= check(manager.currentTracks().size() == 2
                            && manager.currentTracks()[0].toMap().value(QStringLiteral("title"))
                                   == QStringLiteral("Missing"),
                        "manager preserves modern playlist order");
        }
    }
    ok &= check(snapshot(mount) == before,
                "modern browsing changes no bytes and creates no USB sidecars");
    ok &= check(QFile::setPermissions(path, permissions), "restore fixture permissions");
    QFile pending(path + QStringLiteral("-wal"));
    ok &= check(pending.open(QIODevice::WriteOnly) && pending.write("pending") == 7,
                "create unfinished export fixture");
    pending.close();
    result = rekordbox::DeviceSource{}.readIndexReadOnly(mount, "usb");
    ok &= check(!result.ok && result.error.contains(QStringLiteral("recovery")),
                "unfinished export is not silently read as a stale immutable snapshot");
    ok &= check(pending.remove(), "remove unfinished export fixture");
    ok &= check(execute(path, "UPDATE content SET path='../outside.mp3' WHERE content_id=20;"),
                "prepare escaping track path");
    result = rekordbox::DeviceSource{}.readIndexReadOnly(mount, QStringLiteral("usb"));
    ok &= check(result.ok && result.index.tracks[1].audioPath.isEmpty(),
                "modern audio path cannot escape USB");
    ok &= check(execute(path,
        "UPDATE content SET path='\\Contents\\音楽\\100% #?.mp3' WHERE content_id=20;"),
        "prepare backslash device path");
    result = rekordbox::DeviceSource{}.readIndexReadOnly(mount, "usb");
    ok &= check(result.ok && result.index.tracks[1].audioPath == audio,
                "modern Windows separators resolve consistently across hosts");
    const struct { const char* sql; const char* message; } failures[] = {
        {"UPDATE playlist SET attribute=99 WHERE playlist_id=7;",
         "unsupported modern playlist attribute fails explicitly"},
        {"UPDATE playlist SET attribute=0 WHERE playlist_id=7;"
         "UPDATE playlist SET playlist_id_parent=8 WHERE playlist_id=8;",
         "cyclic modern hierarchy fails explicitly"},
        {"UPDATE playlist SET playlist_id_parent=0 WHERE playlist_id=8;"
         "INSERT INTO playlist_content VALUES(7,999,3);",
         "dangling modern membership fails explicitly"},
        {"DELETE FROM playlist_content WHERE content_id=999;"
         "UPDATE content SET content_id=4294967296 WHERE content_id=20;",
         "out-of-range modern track ID fails explicitly"},
        {"DROP TABLE genre;", "unknown modern schema fails explicitly"}
    };
    for (const auto& failure : failures) {
        ok &= check(execute(path, failure.sql), "prepare invalid modern fixture");
        const auto invalid = rekordbox::DeviceSource{}.readIndexReadOnly(mount, "usb");
        ok &= check(!invalid.ok && !invalid.error.isEmpty(), failure.message);
    }
    // An empty but structurally valid legacy index remains a usable fallback.
    QByteArray pdb(4096, '\0');
    pdb[5] = 0x10; // 4096-byte page size, little endian.
    pdb[8] = 1;    // One unknown/unused table, which legacy readers skip.
    pdb[28] = 99;
    QFile legacy(mount + QStringLiteral("/PIONEER/rekordbox/export.pdb"));
    ok &= check(legacy.open(QIODevice::WriteOnly) && legacy.write(pdb) == pdb.size(),
                "create legacy fallback fixture");
    legacy.close();
    result = rekordbox::DeviceSource{}.readIndexReadOnly(mount, "usb");
    ok &= check(result.ok && result.index.warning.contains(QStringLiteral("legacy export.pdb"))
                    && result.index.warning.contains(QStringLiteral("OneLibrary")),
                "unreadable modern schema falls back with explicit diagnostic");
    ok &= check(execute(path,
        "CREATE TABLE genre(genre_id INTEGER PRIMARY KEY,name TEXT);"
        "INSERT INTO genre VALUES(3,'House');"
        "UPDATE content SET content_id=20 WHERE content_id=4294967296;"),
        "restore supported modern schema");
    result = rekordbox::DeviceSource{}.readIndexReadOnly(mount, "usb");
    ok &= check(result.ok && result.index.tracks.size() == 2 && result.index.warning.isEmpty(),
                "supported OneLibrary takes precedence over simultaneous legacy index");
    return ok;
#endif
}
