#include "library/devices/rekordbox/RekordboxDeviceIdentity.h"

#include "library/devices/rekordbox/RekordboxDeviceSource.h"

#include <QFileInfo>
#include <QUrl>

#ifdef BROCKDJ_HAVE_SQLCIPHER
#include <sqlite3.h>
#endif

namespace rekordbox {
namespace {

#ifdef BROCKDJ_HAVE_SQLCIPHER

// The device database of the modern export format (the one the exporting
// software calls OneLibrary; older releases and parts of this codebase still
// say Device Library Plus).
const QString& identityDatabaseRelativePath()
{
    static const QString path = QStringLiteral("PIONEER/rekordbox/exportLibrary.db");
    return path;
}

// The device database is encrypted with SQLCipher using a fixed key that is
// neither licence- nor machine-dependent — every device written by the
// exporting software uses the same one. It is public knowledge and is only
// needed to *read* the device's own name and colour here.
// Applied as a pragma rather than through sqlite3_key(), because the latter is
// only declared when SQLCipher is built with the codec macro exposed.
const char* const kCipherKeyPragma =
    "PRAGMA key = 'r8gddnr4k847830ar6cqzbkk0el6qytmb3trbbx805jm74vez64i5o8fnrqryqls';";

// Guard against a device whose database claims to be something else. The
// identity is only trusted when the table exists with exactly the two columns
// we read, so an unrelated file that happens to sit at this path is ignored
// rather than misinterpreted.
constexpr const char* kIdentityQuery =
    "SELECT deviceName, backGroundColorType FROM property LIMIT 1";

// A file: URI that opens the database strictly for reading. `mode=ro` refuses
// any write, and `immutable` additionally promises SQLite the file will not
// change underneath it, which suppresses the -wal and -shm sidecar files it
// would otherwise want to create next to the database on the device.
QString readOnlyUri(const QString& path)
{
    const QByteArray escaped = QUrl::toPercentEncoding(path, QByteArrayLiteral("/"));
    return QStringLiteral("file:") + QString::fromLatin1(escaped)
         + QStringLiteral("?mode=ro&immutable=1");
}

bool applyKey(sqlite3* db)
{
    if (sqlite3_exec(db, kCipherKeyPragma, nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;
    // Belt and braces: even a mode=ro handle is told explicitly that no
    // statement may modify the database.
    return sqlite3_exec(db, "PRAGMA query_only = 1;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

#endif // BROCKDJ_HAVE_SQLCIPHER

} // namespace

bool DeviceIdentityReader::isSupported()
{
#ifdef BROCKDJ_HAVE_SQLCIPHER
    return true;
#else
    return false;
#endif
}

DeviceColor DeviceIdentityReader::colorFromType(int rawColorType)
{
    // Verified against a device exported with the colour set to purple, which
    // stores 8, and against a device left on the default, which stores 0. The
    // numbering is the same one the exporting software uses for track colours.
    switch (rawColorType) {
    case 0: return DeviceColor::None;
    case 1: return DeviceColor::Pink;
    case 2: return DeviceColor::Red;
    case 3: return DeviceColor::Orange;
    case 4: return DeviceColor::Yellow;
    case 5: return DeviceColor::Green;
    case 6: return DeviceColor::Aqua;
    case 7: return DeviceColor::Blue;
    case 8: return DeviceColor::Purple;
    default: return DeviceColor::Unknown;
    }
}

QString DeviceIdentityReader::colorHex(DeviceColor color)
{
    switch (color) {
    case DeviceColor::Pink:   return QStringLiteral("#FF4FA3");
    case DeviceColor::Red:    return QStringLiteral("#FF3B30");
    case DeviceColor::Orange: return QStringLiteral("#FF9500");
    case DeviceColor::Yellow: return QStringLiteral("#FFD60A");
    case DeviceColor::Green:  return QStringLiteral("#34C759");
    case DeviceColor::Aqua:   return QStringLiteral("#32D6D6");
    case DeviceColor::Blue:   return QStringLiteral("#2E8BFF");
    case DeviceColor::Purple: return QStringLiteral("#AF52DE");
    case DeviceColor::None:
    case DeviceColor::Unknown:
        break;
    }
    return {};
}

DeviceIdentity DeviceIdentityReader::readReadOnly(const QString& mountPath) const
{
    DeviceIdentity identity;

#ifdef BROCKDJ_HAVE_SQLCIPHER
    const QString path =
        DeviceSource::resolveContainedPath(mountPath, identityDatabaseRelativePath());
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return identity;

    sqlite3* db = nullptr;
    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(readOnlyUri(path).toUtf8().constData(), &db, flags, nullptr)
        != SQLITE_OK) {
        sqlite3_close(db);
        return identity;
    }

    if (applyKey(db)) {
        sqlite3_stmt* statement = nullptr;
        // A wrong key or an unexpected schema fails right here at prepare time,
        // which is exactly the "not a device database we understand" case.
        if (sqlite3_prepare_v2(db, kIdentityQuery, -1, &statement, nullptr) == SQLITE_OK) {
            if (sqlite3_step(statement) == SQLITE_ROW) {
                if (const unsigned char* name = sqlite3_column_text(statement, 0))
                    identity.name = QString::fromUtf8(reinterpret_cast<const char*>(name)).trimmed();
                identity.rawColorType = sqlite3_column_int(statement, 1);
                identity.color = colorFromType(identity.rawColorType);
            }
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(db);
#else
    Q_UNUSED(mountPath);
#endif

    return identity;
}

} // namespace rekordbox
