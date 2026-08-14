#include "library/devices/DeviceLibraryManager.h"
#include "library/devices/rekordbox/RekordboxAnalysisReader.h"
#include "library/devices/rekordbox/RekordboxDeviceIdentity.h"
#include "library/devices/rekordbox/RekordboxDeviceSource.h"
#include "library/devices/rekordbox/RekordboxPdbReader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <iostream>

#ifdef Q_OS_LINUX
#include <sys/resource.h>

#ifdef BROCKDJ_HAVE_SQLCIPHER
#include <sqlite3.h>
#endif
#endif

namespace {

bool require(bool value, const char* message)
{
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

void putLe16(QByteArray& bytes, qsizetype offset, quint16 value)
{
    bytes[offset] = static_cast<char>(value & 0xffU);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xffU);
}

void putLe24(QByteArray& bytes, qsizetype offset, quint32 value)
{
    bytes[offset] = static_cast<char>(value & 0xffU);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xffU);
    bytes[offset + 2] = static_cast<char>((value >> 16) & 0xffU);
}

void putLe32(QByteArray& bytes, qsizetype offset, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<char>((value >> (i * 8)) & 0xffU);
}

void putBe16(QByteArray& bytes, qsizetype offset, quint16 value)
{
    bytes[offset] = static_cast<char>((value >> 8) & 0xffU);
    bytes[offset + 1] = static_cast<char>(value & 0xffU);
}

void putBe32(QByteArray& bytes, qsizetype offset, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<char>((value >> ((3 - i) * 8)) & 0xffU);
}

QByteArray deviceSqlString(const QString& text)
{
    const QByteArray latin = text.toLatin1();
    const bool latin1 = QString::fromLatin1(latin) == text;
    QByteArray encoded;
    if (latin1 && latin.size() <= 126) {
        encoded.reserve(latin.size() + 1);
        encoded.append(static_cast<char>(((latin.size() + 1) << 1) | 1));
        encoded.append(latin);
        return encoded;
    }
    if (latin1) {
        encoded.resize(4);
        encoded[0] = 0x40;
        putLe16(encoded, 1, static_cast<quint16>(latin.size() + 4));
        encoded.append(latin);
        return encoded;
    }

    QByteArray utf16;
    utf16.reserve(text.size() * 2);
    for (const QChar unit : text) {
        utf16.append(static_cast<char>(unit.unicode() & 0xffU));
        utf16.append(static_cast<char>((unit.unicode() >> 8) & 0xffU));
    }
    encoded.resize(4);
    encoded[0] = static_cast<char>(0x90);
    putLe16(encoded, 1, static_cast<quint16>(utf16.size() + 4));
    encoded.append(utf16);
    return encoded;
}

QByteArray nameRow(quint32 type, quint32 id, const QString& name)
{
    QByteArray row;
    if (type == 1 || type == 13) {
        row.resize(4);
        putLe32(row, 0, id);
        row.append(deviceSqlString(name));
    } else if (type == 5) {
        row.resize(8);
        putLe32(row, 0, id);
        putLe32(row, 4, id);
        row.append(deviceSqlString(name));
    } else if (type == 2) {
        row.resize(12);
        putLe32(row, 4, id);
        row[9] = 12;
        row.append(deviceSqlString(name));
    } else if (type == 3) {
        row.resize(24);
        putLe32(row, 12, id);
        row[21] = 24;
        row.append(deviceSqlString(name));
    }
    return row;
}

QByteArray colorRow(quint16 id, const QString& name)
{
    QByteArray row(8, '\0');
    putLe16(row, 5, id);
    row.append(deviceSqlString(name));
    return row;
}

QByteArray trackRow(quint32 id, const QString& title, const QString& audioPath,
                    const QString& analysisPath,
                    const QString& comment = QStringLiteral("Golden comment"))
{
    QByteArray row(136, '\0');
    putLe32(row, 28, 60);       // artwork
    putLe32(row, 32, 40);       // key
    putLe32(row, 48, 320);      // bitrate
    putLe32(row, 56, 12'800);   // BPM * 100
    putLe32(row, 60, 10);       // genre
    putLe32(row, 64, 30);       // album
    putLe32(row, 68, 20);       // artist
    putLe32(row, 72, id);
    putLe16(row, 84, 240);
    row[88] = 5;                // color
    row[89] = 4;                // rating

    const auto add = [&row](int slot, const QString& value) {
        const auto offset = static_cast<quint16>(row.size());
        putLe16(row, 94 + slot * 2, offset);
        row.append(deviceSqlString(value));
    };
    add(14, analysisPath);
    add(16, comment);
    add(17, title);
    add(20, audioPath);
    return row;
}

QByteArray playlistRow(quint32 parent, quint32 order, quint32 id, bool folder,
                       const QString& name)
{
    QByteArray row(20, '\0');
    putLe32(row, 0, parent);
    putLe32(row, 8, order);
    putLe32(row, 12, id);
    putLe32(row, 16, folder ? 1U : 0U);
    row.append(deviceSqlString(name));
    return row;
}

QByteArray playlistEntryRow(quint32 order, quint32 trackId, quint32 playlistId)
{
    QByteArray row(12, '\0');
    putLe32(row, 0, order);
    putLe32(row, 4, trackId);
    putLe32(row, 8, playlistId);
    return row;
}

struct TableFixture {
    quint32 type = 0;
    QVector<QByteArray> rows;
};

QByteArray pdbFixture(const QVector<TableFixture>& tables)
{
    constexpr qsizetype pageSize = 4096;
    struct PageFixture {
        quint32 type = 0;
        QVector<QByteArray> rows;
        quint32 page = 0;
        quint32 next = 0;
    };
    struct TablePages {
        quint32 type = 0;
        quint32 first = 0;
        quint32 last = 0;
    };

    QVector<PageFixture> pages;
    QVector<TablePages> tablePages;
    quint32 nextPage = 1;
    for (const auto& table : tables) {
        TablePages refs{table.type, nextPage, nextPage};
        for (qsizetype firstRow = 0; firstRow < table.rows.size(); firstRow += 16) {
            PageFixture page;
            page.type = table.type;
            page.page = nextPage++;
            const qsizetype end = std::min(firstRow + 16, table.rows.size());
            for (qsizetype row = firstRow; row < end; ++row)
                page.rows.append(table.rows.at(row));
            pages.append(page);
        }
        refs.last = nextPage - 1;
        for (qsizetype i = refs.first - 1; i + 1 < refs.last; ++i)
            pages[i].next = pages[i].page + 1;
        if (refs.last > 0)
            pages[refs.last - 1].next = refs.last;
        tablePages.append(refs);
    }

    QByteArray pdb(static_cast<qsizetype>(nextPage) * pageSize, '\0');
    putLe32(pdb, 4, pageSize);
    putLe32(pdb, 8, static_cast<quint32>(tablePages.size()));
    for (qsizetype i = 0; i < tablePages.size(); ++i) {
        const qsizetype base = 28 + i * 16;
        putLe32(pdb, base, tablePages[i].type);
        putLe32(pdb, base + 8, tablePages[i].first);
        putLe32(pdb, base + 12, tablePages[i].last);
    }

    for (const auto& page : pages) {
        const qsizetype start = static_cast<qsizetype>(page.page) * pageSize;
        const qsizetype end = start + pageSize;
        putLe32(pdb, start + 4, page.page);
        putLe32(pdb, start + 8, page.type);
        putLe32(pdb, start + 12, page.next);
        putLe24(pdb, start + 24, static_cast<quint32>(page.rows.size()));
        quint16 present = 0;
        qsizetype cursor = 0;
        for (qsizetype i = 0; i < page.rows.size(); ++i) {
            const QByteArray& row = page.rows.at(i);
            std::memcpy(pdb.data() + start + 40 + cursor, row.constData(), row.size());
            const qsizetype group = i / 16;
            const qsizetype item = i % 16;
            const qsizetype indexBase = end - group * 0x24;
            putLe16(pdb, indexBase - (6 + item * 2), static_cast<quint16>(cursor));
            present |= static_cast<quint16>(1U << item);
            putLe16(pdb, indexBase - 4, present);
            cursor += row.size();
        }
    }
    return pdb;
}

QByteArray section(const char tag[5], const QByteArray& body)
{
    QByteArray value(12, '\0');
    std::memcpy(value.data(), tag, 4);
    putBe32(value, 4, 12);
    putBe32(value, 8, static_cast<quint32>(12 + body.size()));
    value.append(body);
    return value;
}

QByteArray cueEntry(quint32 hotCue, quint8 type, quint32 timeMs, quint32 loopMs)
{
    QByteArray value(56, '\0');
    std::memcpy(value.data(), "PCPT", 4);
    putBe32(value, 4, 16);
    putBe32(value, 8, value.size());
    putBe32(value, 12, hotCue);
    value[28] = static_cast<char>(type);
    putBe32(value, 32, timeMs);
    putBe32(value, 36, loopMs);
    return value;
}

QByteArray legacyCueSection(quint32 listType, const QVector<QByteArray>& cues)
{
    QByteArray body(12, '\0');
    putBe32(body, 0, listType);
    putBe16(body, 6, static_cast<quint16>(cues.size()));
    for (const auto& cue : cues)
        body.append(cue);
    return section("PCOB", body);
}

QByteArray extendedCueSection()
{
    QByteArray body(8, '\0');
    putBe32(body, 0, 1);
    putBe16(body, 4, 1);
    const QString label = QStringLiteral("Drop");
    QByteArray comment;
    for (const QChar unit : label + QChar('\0')) {
        comment.append(static_cast<char>((unit.unicode() >> 8) & 0xffU));
        comment.append(static_cast<char>(unit.unicode() & 0xffU));
    }
    QByteArray cue(48 + comment.size(), '\0');
    std::memcpy(cue.data(), "PCP2", 4);
    putBe32(cue, 4, 44);
    putBe32(cue, 8, cue.size());
    putBe32(cue, 12, 1);
    cue[16] = 1;
    putBe32(cue, 20, 30'250);
    putBe32(cue, 24, 0xffffffffU);
    putBe32(cue, 40, comment.size());
    std::memcpy(cue.data() + 44, comment.constData(), comment.size());
    const qsizetype color = 44 + comment.size();
    cue[color] = 1;
    cue[color + 1] = static_cast<char>(0x12);
    cue[color + 2] = static_cast<char>(0x34);
    cue[color + 3] = static_cast<char>(0x56);
    body.append(cue);
    return section("PCO2", body);
}

QByteArray analysisFile(const QVector<QByteArray>& sections)
{
    QByteArray value(12, '\0');
    std::memcpy(value.data(), "PMAI", 4);
    putBe32(value, 4, 12);
    for (const auto& item : sections)
        value.append(item);
    putBe32(value, 8, value.size());
    return value;
}

QByteArray datFixture()
{
    QByteArray beatBody(12 + 4 * 8, '\0');
    putBe32(beatBody, 8, 4);
    const quint32 times[] {500, 1000, 1500, 2000};
    for (int i = 0; i < 4; ++i) {
        const qsizetype offset = 12 + i * 8;
        putBe16(beatBody, offset, static_cast<quint16>(i + 1));
        putBe16(beatBody, offset + 2, 12'800);
        putBe32(beatBody, offset + 4, times[i]);
    }
    return analysisFile({
        section("PQTZ", beatBody),
        legacyCueSection(0, {cueEntry(0, 1, 64'000, 0xffffffffU),
                             cueEntry(0, 2, 90'000, 94'000)}),
        legacyCueSection(1, {cueEntry(1, 1, 30'250, 0xffffffffU)}),
        section("ZZZZ", {})
    });
}

bool saveFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool complete = file.write(data) == data.size();
    file.close();
    return complete;
}

void setTreeOwnerWritable(const QString& root)
{
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    QStringList paths;
    while (it.hasNext())
        paths.prepend(it.next());
    for (const QString& path : paths) {
        const QFileInfo info(path);
        QFile::setPermissions(path, info.isDir()
            ? QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
            : QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner);
}

#ifdef BROCKDJ_HAVE_SQLCIPHER
// Builds an encrypted device database the way the exporting software writes
// one, so the identity reader can be tested without shipping a real device
// export. This writes into the test's own temporary directory only.
bool writeIdentityFixture(const QString& path, const QString& deviceName, int colorType)
{
    static const char* const keyPragma =
        "PRAGMA key = 'r8gddnr4k847830ar6cqzbkk0el6qytmb3trbbx805jm74vez64i5o8fnrqryqls';";
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.toUtf8().constData(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    const QString script =
        QStringLiteral("CREATE TABLE property(deviceName varchar, dbVersion varchar,"
                       " numberOfContents integer, createdDate varchar,"
                       " backGroundColorType integer, myTagMasterDBID integer);"
                       "INSERT INTO property VALUES('%1', '1000', 3, '2026-01-01', %2, 1);")
            .arg(QString(deviceName).replace(QLatin1Char('\''), QLatin1String("''")))
            .arg(colorType);
    const bool wrote =
        sqlite3_exec(db, keyPragma, nullptr, nullptr, nullptr) == SQLITE_OK
        && sqlite3_exec(db, script.toUtf8().constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return wrote;
}
#endif

bool sourceReadOnlyAudit()
{
    const QString root = QStringLiteral(SOURCE_DIR "/src/library/devices/rekordbox");
    const QStringList forbidden {
        QStringLiteral("QIODevice::WriteOnly"), QStringLiteral("QIODevice::ReadWrite"),
        QStringLiteral("QSaveFile"), QStringLiteral("QFile::remove"),
        QStringLiteral("QFile::rename"), QStringLiteral("QFile::resize"),
        QStringLiteral(".mkdir("), QStringLiteral(".mkpath("),
        // The device database must never be opened in a mode that lets SQLite
        // write, create or journal anything on the connected medium.
        QStringLiteral("SQLITE_OPEN_READWRITE"), QStringLiteral("SQLITE_OPEN_CREATE"),
        QStringLiteral("sqlite3_open("), QStringLiteral("sqlite3_exec(db, \"INSERT"),
        QStringLiteral("journal_mode")
    };
    int explicitReadOnlyOpens = 0;
    QDirIterator it(root, {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (!file.open(QIODevice::ReadOnly))
            return false;
        const QString text = QString::fromUtf8(file.readAll());
        explicitReadOnlyOpens += text.count(QStringLiteral("file.open(QIODevice::ReadOnly)"));
        for (const QString& token : forbidden) {
            if (text.contains(token)) {
                std::cerr << "Forbidden Rekordbox source token: "
                          << token.toStdString() << '\n';
                return false;
            }
        }
    }
    return explicitReadOnlyOpens >= 2;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    QTemporaryDir temp;
    ok &= require(temp.isValid(), "temporary fixture root");
    const QString mount = temp.path() + QStringLiteral("/RB_USB");
    const QString rekordboxDir = mount + QStringLiteral("/PIONEER/rekordbox");
    const QString analysisDir = mount + QStringLiteral("/PIONEER/USBANLZ/000");
    const QString audioDir = mount + QStringLiteral("/Contents/Test");
    QDir().mkpath(rekordboxDir);
    QDir().mkpath(analysisDir);
    QDir().mkpath(audioDir);

    const QVector<TableFixture> tables {
        {0, {trackRow(1001, QStringLiteral("Test Track"),
                      QStringLiteral("/Contents/Test/test.mp3"),
                      QStringLiteral("/PIONEER/USBANLZ/000/ANLZ0000.DAT"),
                      QString(140, QLatin1Char('C'))),
             trackRow(1002, QStringLiteral("Second Ω Track"),
                      QStringLiteral("/Contents/Test/second.mp3"),
                      QStringLiteral("/PIONEER/USBANLZ/000/ANLZ0000.DAT"))}},
        {1, {nameRow(1, 10, QStringLiteral("Techno"))}},
        {2, {nameRow(2, 20, QStringLiteral("Test Artist"))}},
        {3, {nameRow(3, 30, QStringLiteral("Test Album"))}},
        {5, {nameRow(5, 40, QStringLiteral("8A"))}},
        {6, {colorRow(5, QStringLiteral("Blue"))}},
        // Deliberately store child before parent; parser output must follow the
        // explicit Rekordbox tree/order fields, not DeviceSQL heap order.
        {7, {playlistRow(500, 1, 501, false, QStringLiteral("Peak Time")),
             playlistRow(0, 0, 500, true, QStringLiteral("Techno"))}},
        {8, {playlistEntryRow(1, 1001, 501), playlistEntryRow(2, 1002, 501)}},
        {13, {nameRow(13, 60, QStringLiteral("/PIONEER/ART/cover.jpg"))}}
    };
    const QByteArray pdb = pdbFixture(tables);
    const QString pdbPath = rekordboxDir + QStringLiteral("/export.pdb");
    const QString datPath = analysisDir + QStringLiteral("/ANLZ0000.DAT");
    const QString extPath = analysisDir + QStringLiteral("/ANLZ0000.EXT");
    const QString twoExPath = analysisDir + QStringLiteral("/ANLZ0000.2EX");
    ok &= require(saveFile(pdbPath, pdb), "write synthetic PDB fixture");
    ok &= require(saveFile(datPath, datFixture()), "write synthetic DAT fixture");
    ok &= require(saveFile(extPath, analysisFile({extendedCueSection()})),
                  "write synthetic EXT fixture");
    ok &= require(saveFile(twoExPath, analysisFile({section("QQQQ", {})})),
                  "write synthetic 2EX fixture");
    ok &= require(saveFile(audioDir + QStringLiteral("/test.mp3"), QByteArray("audio")),
                  "write first dummy audio file");
    ok &= require(saveFile(audioDir + QStringLiteral("/second.mp3"), QByteArray("audio")),
                  "write second dummy audio file");
    QDir().mkpath(mount + QStringLiteral("/PIONEER/ART"));
    ok &= require(saveFile(mount + QStringLiteral("/PIONEER/ART/cover.jpg"), QByteArray("jpg")),
                  "write dummy artwork file");

    const auto golden = rekordbox::PdbReader{}.readReadOnly(pdbPath);
    ok &= require(golden.ok && golden.tracks.size() == 2, "PDB golden track count");
    ok &= require(golden.tracks.at(0).id == 1001
                      && golden.tracks.at(0).title == QStringLiteral("Test Track")
                      && golden.tracks.at(0).artist == QStringLiteral("Test Artist")
                      && golden.tracks.at(0).album == QStringLiteral("Test Album")
                      && golden.tracks.at(0).genre == QStringLiteral("Techno")
                      && golden.tracks.at(0).key == QStringLiteral("8A")
                      && golden.tracks.at(0).bpm == 128.0
                      && golden.tracks.at(0).durationSec == 240.0
                      && golden.tracks.at(0).rating == 4
                      && golden.tracks.at(0).color == QStringLiteral("Blue")
                      && golden.tracks.at(0).comment.size() == 140
                      && golden.tracks.at(1).title == QStringLiteral("Second Ω Track"),
                  "PDB golden metadata");
    ok &= require(golden.playlists.size() == 2
                      && golden.playlists.at(0).id == 500
                      && golden.playlists.at(1).id == 501
                      && golden.playlists.at(1).trackIds
                             == QVector<quint32>({1001, 1002}),
                  "playlist hierarchy and exact order");

    const auto anlz = rekordbox::AnalysisReader{}.readRelatedReadOnly(datPath);
    ok &= require(anlz.ok && anlz.analysis.beats.size() == 4,
                  "ANLZ beatgrid parsed");
    const QVector<double> expectedBeats{0.5, 1.0, 1.5, 2.0};
    for (qsizetype i = 0; i < expectedBeats.size() && i < anlz.analysis.beats.size(); ++i)
        ok &= require(anlz.analysis.beats.at(i).positionSec == expectedBeats.at(i),
                      "exact beat position");
    ok &= require(anlz.analysis.hotCues.size() == 1
                      && anlz.analysis.hotCues.at(0).positionSec == 30.25
                      && anlz.analysis.hotCues.at(0).label == QStringLiteral("Drop")
                      && anlz.analysis.hotCues.at(0).color == QStringLiteral("#123456"),
                  "hot cue merge");
    ok &= require(anlz.analysis.memoryCues.size() == 1
                      && anlz.analysis.memoryCues.at(0).positionSec == 64.0,
                  "memory cue");
    ok &= require(anlz.analysis.loops.size() == 1
                      && anlz.analysis.loops.at(0).positionSec == 90.0
                      && anlz.analysis.loops.at(0).loopEndSec == 94.0,
                  "loop cue");

    ok &= require(rekordbox::DeviceSource::resolveContainedPath(
                      mount, QStringLiteral("/Contents/Test/test.mp3"))
                      == QFileInfo(audioDir + QStringLiteral("/test.mp3")).canonicalFilePath(),
                  "mount-relative audio resolution");
    ok &= require(rekordbox::DeviceSource::resolveContainedPath(
                      mount, QStringLiteral("../../etc/passwd")).isEmpty(),
                  "path traversal rejected");
    const QString outsidePath = temp.path() + QStringLiteral("/outside.mp3");
    saveFile(outsidePath, QByteArray("outside"));
    QFile::link(outsidePath, mount + QStringLiteral("/Contents/Test/escape.mp3"));
    ok &= require(rekordbox::DeviceSource::resolveContainedPath(
                      mount, QStringLiteral("Contents/Test/escape.mp3")).isEmpty(),
                  "symlink escape rejected");

    QByteArray invalidPdb = pdb;
    putLe32(invalidPdb, 4, 123);
    const QString invalidPdbPath = temp.path() + QStringLiteral("/invalid.pdb");
    saveFile(invalidPdbPath, invalidPdb);
    ok &= require(!rekordbox::PdbReader{}.readReadOnly(invalidPdbPath).ok,
                  "invalid PDB page size rejected");
    QByteArray badOffset = pdb;
    putLe16(badOffset, 4096 + 40 + 94 + 17 * 2, 0xffffU);
    const QString badOffsetPath = temp.path() + QStringLiteral("/bad-offset.pdb");
    saveFile(badOffsetPath, badOffset);
    ok &= require(!rekordbox::PdbReader{}.readReadOnly(badOffsetPath).ok,
                  "invalid PDB string offset rejected");
    QByteArray hugeCount = pdb;
    putLe24(hugeCount, 4096 + 24, 0xffffffU);
    const QString hugeCountPath = temp.path() + QStringLiteral("/huge-count.pdb");
    saveFile(hugeCountPath, hugeCount);
    ok &= require(!rekordbox::PdbReader{}.readReadOnly(hugeCountPath).ok,
                  "huge PDB row count rejected");
    QVector<QByteArray> cycleRows;
    for (quint32 i = 0; i < 17; ++i)
        cycleRows.append(nameRow(1, 100 + i, QStringLiteral("Genre %1").arg(i)));
    QByteArray cyclicPdb = pdbFixture({TableFixture{1, std::move(cycleRows)}});
    putLe32(cyclicPdb, 4096 + 12, 1); // first page links to itself, last page is 2
    const QString cyclicPdbPath = temp.path() + QStringLiteral("/cyclic.pdb");
    saveFile(cyclicPdbPath, cyclicPdb);
    ok &= require(!rekordbox::PdbReader{}.readReadOnly(cyclicPdbPath).ok,
                  "cyclic PDB page chain rejected");
    const QString truncatedPdbPath = temp.path() + QStringLiteral("/truncated.pdb");
    saveFile(truncatedPdbPath, pdb.left(20));
    ok &= require(!rekordbox::PdbReader{}.readReadOnly(truncatedPdbPath).ok,
                  "truncated PDB rejected");
    QByteArray invalidAnlz = datFixture();
    putBe32(invalidAnlz, 12 + 8, 0xffffffffU);
    const QString invalidAnlzPath = temp.path() + QStringLiteral("/invalid.DAT");
    saveFile(invalidAnlzPath, invalidAnlz);
    ok &= require(!rekordbox::AnalysisReader{}.readReadOnly(invalidAnlzPath).ok,
                  "invalid ANLZ section length rejected");
    const QString truncatedAnlzPath = temp.path() + QStringLiteral("/truncated.DAT");
    saveFile(truncatedAnlzPath, datFixture().left(11));
    ok &= require(!rekordbox::AnalysisReader{}.readReadOnly(truncatedAnlzPath).ok,
                  "truncated ANLZ rejected");
    QByteArray trailingAnlz = datFixture();
    trailingAnlz.append("trailing-garbage");
    const QString trailingAnlzPath = temp.path() + QStringLiteral("/trailing.DAT");
    saveFile(trailingAnlzPath, trailingAnlz);
    ok &= require(!rekordbox::AnalysisReader{}.readReadOnly(trailingAnlzPath).ok,
                  "ANLZ bytes beyond declared length rejected");

    const QByteArray beforeHash = QCryptographicHash::hash(pdb, QCryptographicHash::Sha256);
    QDirIterator readOnlyIt(mount, QDir::AllEntries | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
    QStringList directories;
    while (readOnlyIt.hasNext()) {
        const QString path = readOnlyIt.next();
        const QFileInfo info(path);
        if (info.isDir()) {
            directories.prepend(path);
        } else {
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadGroup
                                            | QFileDevice::ReadOther);
        }
    }
    for (const QString& path : directories)
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ExeOwner
                                       | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                       | QFileDevice::ReadOther | QFileDevice::ExeOther);
    QFile::setPermissions(mount, QFileDevice::ReadOwner | QFileDevice::ExeOwner
                                   | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                   | QFileDevice::ReadOther | QFileDevice::ExeOther);

    const auto source = rekordbox::DeviceSource{}.readIndexReadOnly(
        mount, QStringLiteral("golden-device"));
    ok &= require(source.ok && source.index.tracks.size() == 2,
                  "0444/0555 device fixture parsed");
    ok &= require(source.index.tracks.at(0).audioPath.endsWith(
                      QStringLiteral("/Contents/Test/test.mp3"))
                      && source.index.tracks.at(0).analysisPath.endsWith(
                          QStringLiteral("/PIONEER/USBANLZ/000/ANLZ0000.DAT"))
                      && source.index.tracks.at(0).artworkPath.endsWith(
                          QStringLiteral("/PIONEER/ART/cover.jpg")),
                  "resolved audio, analysis and artwork paths");
    ok &= require(source.index.tracks.at(0).sourceAwareId
                      == QStringLiteral("rekordbox:golden-device:1001"),
                  "source-aware track id");

    const QString genericMount = temp.path() + QStringLiteral("/GENERIC_USB");
    const QString plusMount = temp.path() + QStringLiteral("/DLP_USB");
    QDir().mkpath(genericMount);
    QDir().mkpath(plusMount + QStringLiteral("/PIONEER/rekordbox"));
    const QString identityDb =
        plusMount + QStringLiteral("/PIONEER/rekordbox/exportLibrary.db");

    // The colour palette is fixed regardless of whether this build can open an
    // encrypted device database, so it is checked unconditionally. The mapping
    // was verified against a device exported with purple, which stores 8, and
    // one left on the default, which stores 0.
    using rekordbox::DeviceColor;
    using rekordbox::DeviceIdentityReader;
    ok &= require(DeviceIdentityReader::colorFromType(0) == DeviceColor::None
                      && DeviceIdentityReader::colorFromType(8) == DeviceColor::Purple
                      && DeviceIdentityReader::colorFromType(6) == DeviceColor::Aqua,
                  "background colour type maps to the palette");
    ok &= require(DeviceIdentityReader::colorFromType(999) == DeviceColor::Unknown
                      && DeviceIdentityReader::colorFromType(-1) == DeviceColor::Unknown
                      && DeviceIdentityReader::colorHex(DeviceColor::Unknown).isEmpty()
                      && DeviceIdentityReader::colorHex(DeviceColor::None).isEmpty(),
                  "unknown colour type stays neutral instead of crashing");
    ok &= require(DeviceIdentityReader::colorHex(DeviceColor::Purple)
                      == QStringLiteral("#AF52DE"),
                  "palette entry has a display colour");

    // A device that carries no identity at all — the reader must stay quiet
    // rather than invent a name.
    ok &= require(DeviceIdentityReader{}.readReadOnly(genericMount).name.isEmpty()
                      && DeviceIdentityReader{}
                             .readReadOnly(QStringLiteral("/nonexistent-mount"))
                             .name.isEmpty(),
                  "device without an identity database reads as empty");

#ifdef BROCKDJ_HAVE_SQLCIPHER
    ok &= require(writeIdentityFixture(identityDb, QStringLiteral("TIMO USB"), 8),
                  "write encrypted device identity fixture");
    {
        const rekordbox::DeviceIdentity identity =
            DeviceIdentityReader{}.readReadOnly(plusMount);
        ok &= require(identity.name == QStringLiteral("TIMO USB"),
                      "device name is read from the identity database");
        ok &= require(identity.color == DeviceColor::Purple && identity.rawColorType == 8,
                      "background colour is read and the raw value preserved");
    }

    // The reader must work on a medium that is genuinely not writable, and must
    // not leave a journal, WAL or temporary file behind next to the database.
    {
        const QString dbDir = plusMount + QStringLiteral("/PIONEER/rekordbox");
        QFile::setPermissions(identityDb, QFile::ReadOwner | QFile::ReadGroup);
        QFile::setPermissions(dbDir, QFile::ReadOwner | QFile::ExeOwner
                                         | QFile::ReadGroup | QFile::ExeGroup);
        const rekordbox::DeviceIdentity identity =
            DeviceIdentityReader{}.readReadOnly(plusMount);
        const QStringList leftovers =
            QDir(dbDir).entryList(QDir::Files | QDir::Hidden | QDir::System);
        QFile::setPermissions(dbDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                         | QFile::ReadGroup | QFile::ExeGroup);
        QFile::setPermissions(identityDb, QFile::ReadOwner | QFile::WriteOwner
                                              | QFile::ReadGroup);
        ok &= require(identity.name == QStringLiteral("TIMO USB"),
                      "identity is readable from a read-only medium");
        ok &= require(leftovers == QStringList {QStringLiteral("exportLibrary.db")},
                      "no journal, WAL or temporary file is created on the device");
    }
#else
    ok &= require(saveFile(identityDb, QByteArray("read-only detection fixture")),
                  "write Device Library Plus signature fixture");
    ok &= require(!DeviceIdentityReader::isSupported(),
                  "identity reader reports itself unavailable without SQLCipher");
#endif
    {
        DeviceLibraryManager systemDeviceManager(false, nullptr);
        QVariantMap systemVolume {
            {QStringLiteral("device"), QStringLiteral("/dev/brockdj-test1")},
            {QStringLiteral("name"), QStringLiteral("TEST USB")},
            {QStringLiteral("fileSystemType"), QStringLiteral("vfat")},
            {QStringLiteral("objectPath"),
             QStringLiteral("/org/freedesktop/UDisks2/block_devices/brockdj_test1")},
            {QStringLiteral("driveObjectPath"),
             QStringLiteral("/org/freedesktop/UDisks2/drives/brockdj_test")},
            {QStringLiteral("canPowerOff"), true},
            {QStringLiteral("ejectable"), true}
        };
        systemDeviceManager.inspectTestSystemDevices(QVariantList {systemVolume});
        const QVariantMap unmounted = systemDeviceManager.devices().front().toMap();
        const QString systemDeviceId = unmounted.value(QStringLiteral("id")).toString();
        systemDeviceManager.chooseDevice(systemDeviceId);
        ok &= require(!unmounted.value(QStringLiteral("mounted")).toBool()
                          && unmounted.value(QStringLiteral("canMount")).toBool()
                          && !unmounted.value(QStringLiteral("canEject")).toBool()
                          && unmounted.value(QStringLiteral("actionLabel"))
                                 == QStringLiteral("MOUNT")
                          && !systemDeviceManager.selectedDeviceReady(),
                      "unmounted USB is visible with a mount action");

        systemVolume.insert(QStringLiteral("mountPath"), genericMount);
        systemDeviceManager.inspectTestSystemDevices(QVariantList {systemVolume});
        const QVariantMap mounted = systemDeviceManager.devices().front().toMap();
        ok &= require(mounted.value(QStringLiteral("id")).toString() == systemDeviceId
                          && mounted.value(QStringLiteral("mounted")).toBool()
                          && !mounted.value(QStringLiteral("canMount")).toBool()
                          && mounted.value(QStringLiteral("canEject")).toBool()
                          && mounted.value(QStringLiteral("actionLabel"))
                                 == QStringLiteral("EJECT")
                          && systemDeviceManager.selectedDeviceReady(),
                      "mounted USB keeps its stable identity and exposes eject");
    }
    {
        DeviceLibraryManager classificationManager(false, nullptr);
        classificationManager.inspectTestMounts({genericMount, plusMount});
        bool foundGeneric = false;
        bool foundPlus = false;
        bool profilePublished = false;
        bool profileFallback = false;
        for (const QVariant& value : classificationManager.devices()) {
            const QVariantMap device = value.toMap();
            // The identity fixture sits on the Device Library Plus mount, so
            // the generic one doubles as the "never named" case.
            if (device.value(QStringLiteral("libraryType")) == QStringLiteral("genericUsb")) {
                profileFallback =
                    device.value(QStringLiteral("libraryName")).toString().isEmpty()
                    && device.value(QStringLiteral("color")).toString().isEmpty()
                    && device.value(QStringLiteral("name")).toString()
                           == device.value(QStringLiteral("volumeLabel")).toString()
                    && device.value(QStringLiteral("name")) == QStringLiteral("GENERIC_USB");
            } else {
                // The name from the exporting software wins over the volume
                // label, but both stay available to the UI.
                profilePublished =
                    device.value(QStringLiteral("volumeLabel")) == QStringLiteral("DLP_USB")
                    && (DeviceIdentityReader::isSupported()
                            ? (device.value(QStringLiteral("libraryName"))
                                   == QStringLiteral("TIMO USB")
                               && device.value(QStringLiteral("name"))
                                   == QStringLiteral("TIMO USB")
                               && device.value(QStringLiteral("color"))
                                   == QStringLiteral("#AF52DE"))
                            : (device.value(QStringLiteral("libraryName")).toString().isEmpty()
                               && device.value(QStringLiteral("name"))
                                   == QStringLiteral("DLP_USB")));
            }
            foundGeneric = foundGeneric
                || (device.value(QStringLiteral("badge")) == QStringLiteral("USB")
                    && device.value(QStringLiteral("libraryType"))
                           == QStringLiteral("genericUsb"));
            foundPlus = foundPlus
                || (device.value(QStringLiteral("badge")) == QStringLiteral("REKORDBOX")
                    && device.value(QStringLiteral("libraryType"))
                           == QStringLiteral("rekordboxDeviceLibraryPlus")
                    && device.value(QStringLiteral("status")).toString().contains(
                           QStringLiteral("not yet supported"), Qt::CaseInsensitive));
        }
        ok &= require(foundGeneric, "generic USB classification");
        ok &= require(foundPlus, "Device Library Plus detection and safe status");
        ok &= require(profilePublished,
                      "device identity name and colour reach the device list");
        ok &= require(profileFallback,
                      "device without an identity shows its volume label");
    }

    DeviceLibraryManager manager(false, nullptr);
    int removalCount = 0;
    QObject::connect(&manager, &DeviceLibraryManager::deviceRemoved,
                     [&removalCount](const QString&) { ++removalCount; });
    manager.inspectTestMounts({mount});
    ok &= require(manager.devices().size() == 1
                      && manager.devices().at(0).toMap().value(QStringLiteral("badge"))
                             == QStringLiteral("REKORDBOX"),
                  "hotplug Rekordbox classification");
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 5000
           && manager.devices().at(0).toMap().value(QStringLiteral("trackCount")).toInt() != 2) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    const QString managerDeviceId = manager.devices().at(0).toMap()
                                        .value(QStringLiteral("id")).toString();
    manager.chooseDevice(managerDeviceId);
    ok &= require(manager.currentTracks().isEmpty()
                      && manager.selectedViewName() == manager.selectedDeviceName()
                      && manager.currentPlaylists().size() == 2,
                  "device root exposes All Tracks and Playlists navigation");
    manager.chooseTracks();
    ok &= require(manager.currentTracks().size() == 2
                      && manager.selectedViewName() == QStringLiteral("All Tracks"),
                  "All Tracks child publishes the device track model");
    manager.choosePlaylists();
    ok &= require(manager.currentTracks().isEmpty()
                      && manager.selectedViewName() == QStringLiteral("Playlists"),
                  "Playlists child opens without retaining a stale track table");
    manager.choosePlaylist(QStringLiteral("501"));
    ok &= require(manager.currentTracks().size() == 2
                      && manager.currentTracks().at(0).toMap()
                             .value(QStringLiteral("trackId")).toString().endsWith(
                                 QStringLiteral(":1001"))
                      && manager.currentTracks().at(1).toMap()
                             .value(QStringLiteral("trackId")).toString().endsWith(
                                 QStringLiteral(":1002")),
                  "playlist model preserves Rekordbox order");
    manager.setFilterText(QStringLiteral("second"));
    ok &= require(manager.currentTracks().size() == 1
                      && manager.currentTracks().front().toMap()
                             .value(QStringLiteral("title")) == QStringLiteral("Second Ω Track"),
                  "device metadata search filter");
    manager.setFilterText(QString());
    manager.setSort(QStringLiteral("title"), true);
    ok &= require(manager.currentTracks().size() == 2
                      && manager.currentTracks().front().toMap()
                             .value(QStringLiteral("title")) == QStringLiteral("Second Ω Track"),
                  "device UI-side sorting");
    ok &= require(manager.currentArtists().size() == 1
                      && manager.currentAlbums().size() == 1
                      && manager.currentGenres().size() == 1
                      && manager.currentFolders().size() == 1,
                  "device artist/album/genre/folder views");

    bool deckLoadReady = false;
    QVariantMap deckRequest;
    QObject::connect(&manager, &DeviceLibraryManager::deckLoadReady,
                     [&deckLoadReady, &deckRequest](const QString& deck,
                                                    const QVariantMap& request) {
        if (deck == QStringLiteral("B")) {
            deckLoadReady = true;
            deckRequest = request;
        }
    });
    const QString requestedTrack = QStringLiteral("rekordbox:%1:1001").arg(managerDeviceId);
    manager.requestDeckLoad(requestedTrack, QStringLiteral("B"));
    wait.restart();
    while (!deckLoadReady && wait.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    ok &= require(deckLoadReady
                      && deckRequest.value(QStringLiteral("readOnlyExternal")).toBool()
                      && deckRequest.value(QStringLiteral("filePath")).toString().endsWith(
                          QStringLiteral("/Contents/Test/test.mp3"))
                      && deckRequest.value(QStringLiteral("beats")).toList().size() == 4
                      && deckRequest.value(QStringLiteral("hotCues")).toList().size() == 1
                      && deckRequest.value(QStringLiteral("memoryCues")).toList().size() == 1
                      && deckRequest.value(QStringLiteral("loops")).toList().size() == 1,
                  "indexed track publishes direct read-only deck load request");
    manager.inspectTestMounts({});
    ok &= require(manager.devices().isEmpty() && manager.currentTracks().isEmpty()
                      && manager.selectedDeviceId().isEmpty() && removalCount == 1,
                  "hot-unplug clears device and selected rows");
    // Re-open through one persistent handle for an unambiguous mutation check.
    QFile verifyPdb(pdbPath);
    const bool verifyOpened = verifyPdb.open(QIODevice::ReadOnly);
    ok &= require(verifyOpened, "re-open export.pdb for mutation check");
    ok &= require(verifyOpened
                      && QCryptographicHash::hash(verifyPdb.readAll(), QCryptographicHash::Sha256)
                             == beforeHash,
                  "device scan did not mutate export.pdb");
    ok &= require(sourceReadOnlyAudit(), "Rekordbox module read-only source audit");

    // A parser-only 10k-track fixture verifies bounded linear indexing without
    // placing copyrighted Rekordbox data in the repository.
    QVector<QByteArray> manyTracks;
    manyTracks.reserve(10'000);
    for (quint32 i = 0; i < 10'000; ++i) {
        manyTracks.append(trackRow(10'000 + i, QStringLiteral("Track %1").arg(i),
                                   QStringLiteral("/Contents/Test/test.mp3"),
                                   QStringLiteral("/PIONEER/USBANLZ/000/ANLZ0000.DAT")));
    }
    const QString largePdbPath = temp.path() + QStringLiteral("/large.pdb");
    saveFile(largePdbPath, pdbFixture({TableFixture{0, std::move(manyTracks)}}));
    QElapsedTimer parseTimer;
    parseTimer.start();
    const auto large = rekordbox::PdbReader{}.readReadOnly(largePdbPath);
    const qint64 parseMs = parseTimer.elapsed();
    std::cout << "synthetic 10000-track PDB parse: " << parseMs << " ms\n";
#ifdef Q_OS_LINUX
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        std::cout << "test process peak RSS: " << usage.ru_maxrss << " KiB\n";
#endif
    ok &= require(large.ok && large.tracks.size() == 10'000,
                  "10k-track bounded parser fixture");
    ok &= require(parseMs < 15'000, "10k-track parse exceeded safety budget");

    setTreeOwnerWritable(mount);
    return ok ? 0 : 1;
}
