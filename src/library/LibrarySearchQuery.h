#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

// Compiles the library search box text into a parameterised SQL condition
// over `Tracks t JOIN Locations l`.
//
// Syntax (all case-, accent- and punctuation-insensitive for text):
//   house "deep tech"          plain words / phrases across title, artist,
//                              album, genre, comment, notes, path and tags
//   artist:daft  title:=Intro  field contains / whole-field match
//   genre:""                   empty field
//   bpm:120-128  bpm:>=124     numeric: bpm, rating, energy, played,
//   duration:<5m duration:3:30 bitrate, duration (m:ss, 5m, seconds)
//   added:<7d  lastplayed:>2w  relative age (d/w/m/y) or a date (2026-01-31)
//   key:8A  ~key:8A            exact Camelot key / harmonic neighbours
//   ~bpm:124                   tempo within 6 % (also half/double tempo)
//   ~deck                      mixes with the decks' current keys/tempos
//   color:red  color:""        track colour by name (en/de) or hex
//   is:analyzed is:unanalyzed is:played is:unplayed is:rated is:unrated
//   is:duplicate is:noinfo is:lowbitrate is:long is:missing
//   health:noinfo              is:<check> minus tracks ignored for that check
//   -term                      exclude, `|` or `OR` between alternatives
namespace brockdj::library {

struct LibrarySearchContext {
    // Unix seconds used for relative dates; 0 = current time.
    qint64 nowSecs = 0;
    // Keys/BPMs of the loaded decks for `~deck`.
    QStringList deckKeys;
    QList<double> deckBpms;
};

struct LibrarySearchSql {
    // Empty when the text has no effective terms.
    QString condition;
    QVariantMap bindings;
    // True when a loosened (one-letter-off) retry could find more.
    bool canLoosen = false;
};

LibrarySearchSql compileLibrarySearch(const QString& text,
                                      const LibrarySearchContext& context = {},
                                      bool loosen = false);

// Lower-case, strip accents and search-irrelevant punctuation.
QString foldSearchText(const QString& text);

// Camelot keys that mix with `key` (itself, ±1 and the relative major/minor).
QStringList harmonicCamelotKeys(const QString& key);

} // namespace brockdj::library
