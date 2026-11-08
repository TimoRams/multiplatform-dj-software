#include "library/CollectionHealth.h"
#include "library/LibrarySearchQuery.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

#include <cstdlib>
#include <iostream>

using brockdj::library::artistTitleFromFileName;
using brockdj::library::compileLibrarySearch;
using brockdj::library::matchRelocations;
using brockdj::library::normalizeHealthCheck;
using brockdj::library::LibrarySearchContext;

namespace {

constexpr qint64 kNow = 1'800'000'000;

void require(bool value, const QString& message)
{
    if (!value) {
        std::cerr << "FAIL: " << message.toStdString() << '\n';
        std::exit(1);
    }
}

void exec(QSqlDatabase& db, const QString& statement)
{
    QSqlQuery q(db);
    require(q.exec(statement), statement + QStringLiteral(" -> ") + q.lastError().text());
}

struct Track {
    const char* id;
    const char* title;
    const char* artist;
    double bpm;
    const char* key;
    int durationSec;
    int bitrate;
    int rating;
    int playCount;
    qint64 dateAdded;
    const char* color;
    const char* genre;
    const char* path;
};

void seed(QSqlDatabase& db)
{
    exec(db, QStringLiteral(
        "CREATE TABLE Tracks (id TEXT PRIMARY KEY, title TEXT, artist TEXT, duration_sec INTEGER,"
        " bitrate_kbps INTEGER, bpm REAL, key TEXT, is_analyzed INTEGER, genre TEXT, album TEXT,"
        " comment TEXT, rating INTEGER, energy INTEGER, color TEXT, notes TEXT, play_count INTEGER,"
        " last_played INTEGER, date_added INTEGER)"));
    exec(db, QStringLiteral("CREATE TABLE Locations (track_id TEXT, file_path TEXT)"));
    exec(db, QStringLiteral("CREATE TABLE Tags (id INTEGER PRIMARY KEY, name TEXT)"));
    exec(db, QStringLiteral("CREATE TABLE TrackTags (track_id TEXT, tag_id INTEGER)"));
    exec(db, QStringLiteral("CREATE TABLE CuePoints (track_id TEXT, cue_index INTEGER)"));
    exec(db, QStringLiteral("CREATE TABLE MissingFiles (file_path TEXT PRIMARY KEY)"));
    exec(db, QStringLiteral("CREATE TABLE HealthIgnores (track_id TEXT, check_name TEXT)"));

    const Track tracks[] = {
        {"ac", "Thunderstruck", "AC/DC", 134.0, "8A", 292, 320, 5, 3, kNow - 2 * 86400, "#e84040", "Rock", "/m/acdc.mp3"},
        {"of", "Saman", "\u00d3lafur Arnalds", 128.4, "08B", 260, 1411, 0, 0, kNow - 40 * 86400, "", "Ambient", "/m/of.flac"},
        {"nf", "Says", "Nils Frahm", 64.0, "9A", 1500, 128, 2, 1, kNow - 400 * 86400, "#4090e8", "Ambient", "/m/nf.mp3"},
        {"jz", "Empire State of Mind", "Jay-Z feat. Alicia Keys", 87.0, "3A", 276, 256, 4, 0, kNow - 10 * 86400, "", "Hip Hop", "/m/jz.mp3"},
        {"d1", "Intro", "Daft Punk", 120.0, "1B", 60, 320, 0, 0, kNow, "", "House", "/m/d1.mp3"},
        {"d2", "intro", "daft punk", 124.0, "12A", 62, 320, 0, 0, kNow, "", "House", "/m/d2.mp3"},
        {"ut", "Track 01", "", 0.0, "", 200, 320, 0, 0, kNow, "", "", "/m/ut.mp3"},
    };
    for (const Track& t : tracks) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO Tracks VALUES (:id,:title,:artist,:dur,:kbps,:bpm,:key,:an,:genre,'','',"
            ":rating,0,:color,'',:plays,:last,:added)"));
        q.bindValue(QStringLiteral(":id"), QString::fromUtf8(t.id));
        q.bindValue(QStringLiteral(":title"), QString::fromUtf8(t.title));
        q.bindValue(QStringLiteral(":artist"), QString::fromUtf8(t.artist));
        q.bindValue(QStringLiteral(":dur"), t.durationSec);
        q.bindValue(QStringLiteral(":kbps"), t.bitrate);
        q.bindValue(QStringLiteral(":bpm"), t.bpm);
        q.bindValue(QStringLiteral(":key"), QString::fromUtf8(t.key));
        q.bindValue(QStringLiteral(":an"), t.bpm > 0.0 ? 1 : 0);
        q.bindValue(QStringLiteral(":genre"), QString::fromUtf8(t.genre));
        q.bindValue(QStringLiteral(":rating"), t.rating);
        q.bindValue(QStringLiteral(":color"), QString::fromUtf8(t.color));
        q.bindValue(QStringLiteral(":plays"), t.playCount);
        q.bindValue(QStringLiteral(":last"), t.playCount > 0 ? kNow - 86400 : 0);
        q.bindValue(QStringLiteral(":added"), t.dateAdded);
        require(q.exec(), QStringLiteral("insert ") + q.lastError().text());
        QSqlQuery l(db);
        l.prepare(QStringLiteral("INSERT INTO Locations VALUES (:id, :path)"));
        l.bindValue(QStringLiteral(":id"), QString::fromUtf8(t.id));
        l.bindValue(QStringLiteral(":path"), QString::fromUtf8(t.path));
        require(l.exec(), QStringLiteral("insert location"));
    }
    exec(db, QStringLiteral("INSERT INTO Tags VALUES (1, 'Peak Time')"));
    exec(db, QStringLiteral("INSERT INTO TrackTags VALUES ('ac', 1)"));
    exec(db, QStringLiteral("INSERT INTO CuePoints VALUES ('nf', 0)"));
    exec(db, QStringLiteral("INSERT INTO MissingFiles VALUES ('/m/jz.mp3'), ('/m/d2.mp3')"));
    exec(db, QStringLiteral("INSERT INTO HealthIgnores VALUES ('d2', 'duplicate'), ('jz', 'noinfo')"));
}

QStringList run(QSqlDatabase& db, const QString& text, bool loosen = false,
                const LibrarySearchContext& extra = {})
{
    LibrarySearchContext context = extra;
    context.nowSecs = kNow;
    const auto search = compileLibrarySearch(text, context, loosen);
    QString sql = QStringLiteral("SELECT t.id FROM Tracks t JOIN Locations l ON t.id = l.track_id");
    if (!search.condition.isEmpty())
        sql += QStringLiteral(" WHERE ") + search.condition;
    sql += QStringLiteral(" ORDER BY t.id");
    QSqlQuery q(db);
    require(q.prepare(sql), text + QStringLiteral(" prepare: ") + q.lastError().text());
    for (auto it = search.bindings.cbegin(); it != search.bindings.cend(); ++it)
        q.bindValue(it.key(), it.value());
    require(q.exec(), text + QStringLiteral(" exec: ") + q.lastError().text());
    QStringList ids;
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

void expect(QSqlDatabase& db, const QString& text, const QStringList& ids,
            bool loosen = false, const LibrarySearchContext& context = {})
{
    const QStringList actual = run(db, text, loosen, context);
    require(actual == ids, QStringLiteral("'%1' -> [%2], expected [%3]")
                               .arg(text, actual.join(u','), ids.join(u',')));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    db.setDatabaseName(QStringLiteral(":memory:"));
    require(db.open(), QStringLiteral("open"));
    seed(db);

    const QStringList all{"ac", "d1", "d2", "jz", "nf", "of", "ut"};
    expect(db, QString(), all);
    expect(db, QStringLiteral("   "), all);

    // Folding: case, accents, punctuation.
    expect(db, QStringLiteral("acdc"), {"ac"});
    expect(db, QStringLiteral("THUNDER"), {"ac"});
    expect(db, QStringLiteral("olafur"), {"of"});
    expect(db, QStringLiteral("\u00f3lafur"), {"of"});
    expect(db, QStringLiteral("jayz"), {"jz"});
    expect(db, QStringLiteral("ft"), {"jz"});
    expect(db, QStringLiteral("\"state of\""), {"jz"});
    expect(db, QStringLiteral("daft intro"), {"d1", "d2"});
    expect(db, QStringLiteral("peak"), {"ac"});
    expect(db, QStringLiteral("flac"), {"of"});

    // Fields, exact and empty.
    expect(db, QStringLiteral("artist:daft"), {"d1", "d2"});
    expect(db, QStringLiteral("title:=Intro"), {"d1", "d2"});
    expect(db, QStringLiteral("title:intr"), {"d1", "d2"});
    expect(db, QStringLiteral("title:=intr"), {});
    expect(db, QStringLiteral("artist:\"\""), {"ut"});
    expect(db, QStringLiteral("genre:\"hip hop\""), {"jz"});
    expect(db, QStringLiteral("tag:peak"), {"ac"});
    expect(db, QStringLiteral("tag:\"\""), {"d1", "d2", "jz", "nf", "of", "ut"});

    // Negation and OR.
    expect(db, QStringLiteral("ambient -frahm"), {"of"});
    expect(db, QStringLiteral("acdc | jayz"), {"ac", "jz"});
    expect(db, QStringLiteral("acdc OR genre:ambient"), {"ac", "nf", "of"});

    // Numbers.
    expect(db, QStringLiteral("bpm:128"), {"of"});
    expect(db, QStringLiteral("bpm:120-125"), {"d1", "d2"});
    expect(db, QStringLiteral("bpm:>130"), {"ac"});
    expect(db, QStringLiteral("rating:>=4"), {"ac", "jz"});
    expect(db, QStringLiteral("duration:<1:30"), {"d1", "d2"});
    expect(db, QStringLiteral("duration:>20m"), {"nf"});
    expect(db, QStringLiteral("played:>0"), {"ac", "nf"});
    expect(db, QStringLiteral("bpm:abc"), {});

    // Dates.
    expect(db, QStringLiteral("added:<7d"), {"ac", "d1", "d2", "ut"});
    expect(db, QStringLiteral("added:>1y"), {"nf"});
    expect(db, QStringLiteral("added:2w-8w"), {"of"});
    expect(db, QStringLiteral("lastplayed:<2d"), {"ac", "nf"});

    // Keys and tempo.
    expect(db, QStringLiteral("key:8a"), {"ac"});
    expect(db, QStringLiteral("key:8B"), {"of"});
    expect(db, QStringLiteral("~key:8A"), {"ac", "nf", "of"});
    expect(db, QStringLiteral("~key:1A"), {"d1", "d2"});
    expect(db, QStringLiteral("~bpm:124"), {"d1", "d2", "nf", "of"});
    LibrarySearchContext deck;
    deck.deckKeys = {QStringLiteral("12A")};
    deck.deckBpms = {122.0};
    expect(db, QStringLiteral("~deck"), {"d2"}, false, deck);
    expect(db, QStringLiteral("~deck"), all);

    // Colours and checks.
    expect(db, QStringLiteral("color:rot"), {"ac"});
    expect(db, QStringLiteral("color:blue"), {"nf"});
    expect(db, QStringLiteral("is:duplicate"), {"d1", "d2"});
    expect(db, QStringLiteral("is:noinfo"), {"ut"});
    expect(db, QStringLiteral("is:lowbitrate"), {"nf"});
    expect(db, QStringLiteral("is:long"), {"nf"});
    expect(db, QStringLiteral("is:unanalyzed"), {"ut"});
    expect(db, QStringLiteral("is:cued"), {"nf"});
    expect(db, QStringLiteral("-is:cued is:rated"), {"ac", "jz"});

    expect(db, QStringLiteral("is:missing"), {"d2", "jz"});

    // Health view: checks minus per-check ignores.
    expect(db, QStringLiteral("health:duplicate"), {"d1"});
    expect(db, QStringLiteral("health:dupe"), {"d1"});
    expect(db, QStringLiteral("health:missing"), {"d2", "jz"});
    expect(db, QStringLiteral("health:noinfo"), {"ut"});
    expect(db, QStringLiteral("health:nonsense"), {});

    // Health helpers.
    const auto at = artistTitleFromFileName(QStringLiteral("/x/01 - Daft Punk - One More Time (Radio Edit).mp3"));
    require(at.artist == QStringLiteral("Daft Punk") && at.title == QStringLiteral("One More Time (Radio Edit)"),
            QStringLiteral("filename parse: ") + at.artist + u'|' + at.title);
    const auto underscores = artistTitleFromFileName(QStringLiteral("B2_Moderat_-_Bad_Kingdom.flac"));
    require(underscores.artist == QStringLiteral("Moderat") && underscores.title == QStringLiteral("Bad Kingdom"),
            QStringLiteral("underscore parse: ") + underscores.artist + u'|' + underscores.title);
    const auto titleOnly = artistTitleFromFileName(QStringLiteral("Intro.wav"));
    require(titleOnly.artist.isEmpty() && titleOnly.title == QStringLiteral("Intro"), QStringLiteral("title only"));
    const auto band = artistTitleFromFileName(QStringLiteral("808 State - Pacific.mp3"));
    require(band.artist == QStringLiteral("808 State"), QStringLiteral("band number kept: ") + band.artist);
    const auto numeric = artistTitleFromFileName(QStringLiteral("1999.mp3"));
    require(numeric.title == QStringLiteral("1999"), QStringLiteral("number-only title kept: ") + numeric.title);
    require(normalizeHealthCheck(QStringLiteral("Unanalysed")) == QStringLiteral("unanalyzed"),
            QStringLiteral("alias"));

    const auto moves = matchRelocations(
        {QStringLiteral("/old/House/a.mp3"), QStringLiteral("/old/Techno/b.mp3"),
         QStringLiteral("/old/x/c.mp3"), QStringLiteral("/old/y/gone.mp3")},
        {QStringLiteral("/new/House/A.MP3"), QStringLiteral("/new/House/b.mp3"),
         QStringLiteral("/new/Techno/b.mp3"), QStringLiteral("/new/p/c.mp3"), QStringLiteral("/new/q/c.mp3")});
    require(moves.moves.value(QStringLiteral("/old/House/a.mp3")) == QStringLiteral("/new/House/A.MP3"),
            QStringLiteral("relocate case-insensitive"));
    require(moves.moves.value(QStringLiteral("/old/Techno/b.mp3")) == QStringLiteral("/new/Techno/b.mp3"),
            QStringLiteral("relocate parent tie-break"));
    require(moves.ambiguous == QStringList{QStringLiteral("/old/x/c.mp3")}, QStringLiteral("relocate ambiguous"));
    require(moves.notFound == QStringList{QStringLiteral("/old/y/gone.mp3")}, QStringLiteral("relocate not found"));

    // One-letter-off retry.
    expect(db, QStringLiteral("frahn"), {});
    require(compileLibrarySearch(QStringLiteral("frahn")).canLoosen, QStringLiteral("frahn loosenable"));
    require(!compileLibrarySearch(QStringLiteral("abc")).canLoosen, QStringLiteral("short words stay strict"));
    expect(db, QStringLiteral("frahn"), {"nf"}, true);
    expect(db, QStringLiteral("frhm"), {"nf"}, true);

    // Injection-shaped input stays data.
    expect(db, QStringLiteral("x');DROP TABLE Tracks;--"), {});
    expect(db, QStringLiteral("acdc"), {"ac"});

    std::cout << "library_search: all checks passed\n";
    return 0;
}
