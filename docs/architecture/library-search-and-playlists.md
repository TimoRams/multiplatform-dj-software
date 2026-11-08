# Library search syntax and playlist files

## Search syntax

`src/library/LibrarySearchQuery.*` compiles the library search field into a
parameterised SQL condition (`compileLibrarySearch`). `LibraryTableModel`
runs it for every page; if a strict query returns nothing and fuzzy matching
is possible, the page is requested once more with loosened words.

| Syntax | Meaning |
|---|---|
| `word`, `"two words"` | title, artist, album, genre, comment, notes, path, tags (case/accent/punctuation insensitive; `ft`/`feat`, `&`/`and`/`und` are synonyms) |
| `artist:x`, `title:=Exact`, `genre:""` | field contains / equals / is empty (`album`, `comment`, `notes`, `path`, `tag` too) |
| `bpm:120-128`, `rating:>=4`, `energy:<5`, `played:0`, `bitrate:<192`, `duration:>5m` | numeric comparisons and ranges |
| `added:<2w`, `lastplayed:>1y`, `added:2024-01-01-2024-06-30` | relative ages (`d`/`w`/`m`/`y`) or ISO dates |
| `key:8A`, `~key:8A`, `~bpm:124`, `~deck` | key, harmonically compatible keys, ±6 % tempo (incl. half/double), match against loaded decks |
| `color:rot`, `color:#4090e8` | track colour (German/English names or hex) |
| `is:unanalyzed`, `is:unplayed`, `is:unrated`, `is:cued`, `is:long`, `is:lowbitrate`, `is:noinfo`, `is:duplicate`, `is:missing` | collection checks |
| `health:noinfo` (`missing`, `duplicate`, `unanalyzed`, `lowbitrate`) | the same check minus tracks marked "in Ordnung" for it |
| `-term`, `a \| b`, `a OR b` | negation and alternatives |

Words with four or more letters may differ by one letter when the strict
search finds nothing (title, artist and album only).

Tests: `tests/library/library_search_tests.cpp` (`ctest -R library_search`).

## Playlist files

`src/library/PlaylistFiles.*` reads and writes M3U/M3U8 (`#EXTINF`), PLS and
XSPF without touching the database. Reading accepts UTF-8 (BOM stripped) with
a Latin-1 fallback, CRLF, `file://` URLs, Windows separators and paths
relative to the playlist folder; remote streams are skipped. Writing uses
absolute paths, UTF-8 and `QSaveFile`; the format follows the extension.

`LibraryDatabase::exportPlaylistToFile` / `importPlaylistFile`
(`src/library/LibraryPlaylistFiles.cpp`) connect this to the library. An
import creates a playlist named after the file; files that are not in the
library yet are added from their tags (TagLib, falling back to playlist info
or the file name) with the same track id that loading on a deck would
produce (`library/TrackIdentity.h`). Missing files are reported, not added.

UI: the "↓" button next to "+" in the playlist sidebar imports a file; the
playlist context menu has "Exportieren…" and "Hier importieren…" (as a
sub-crate).

Tests: `tests/library/playlist_files_tests.cpp` (`ctest -R playlist_files`).

## Collection health

"Sammlung prüfen" in the library sidebar (`src/qml/library/CollectionHealthView.qml`)
shows one card per check with its count, "Anzeigen" (switches to the library
with a `health:<check>` search) and a bulk fix where one exists. The track
context menu offers "Als in Ordnung markieren" while a `health:` search is
active; ignored tracks can be restored per card.

Data (schema 19): `MissingFiles(file_path)` holds the result of the last scan,
`HealthIgnores(track_id, check_name)` the per-check ignore marks. Both feed the
search compiler, so counts, the table and the fixes share one definition.

`LibraryHealth.cpp` runs the slow parts off the UI thread (QtConcurrent) and
applies results on the main thread; one job at a time (`healthBusy`), each
ending with `healthTaskFinished(task, result)`:

| Job | What it does |
|---|---|
| `scanMissingFiles()` | checks every location; runs when the view opens and the last scan is older than 10 min |
| `relocateMissingTracks(folder)` | searches a folder recursively for the missing file names; unique matches (or the one in a same-named parent folder) are repointed and keep their waveform cache; paths already in the library are skipped |
| `refreshTagsFromFiles()` | re-reads artist/title for `health:noinfo` tracks with TagLib, falling back to "Artist - Title" file names; only placeholder values are replaced |
| `removeMissingTracks()` | (synchronous, confirmed in the UI) removes tracks whose every location is missing |

Unanalysed tracks use `libraryAnalyzer.analyzeAll()`. The pure helpers
(`CollectionHealth.*`: filename parsing, relocation matching) are covered by
`library_search_tests`; the database round trip (scan, ignore, relocate) by the
CI runtime smoke test.

## Rekordbox XML import

`File > Export Collection in xml format` from Rekordbox can be imported via the
playlist import button (choose "Rekordbox-Sammlung (*.xml)").

- `src/library/RekordboxXml.*` is a pure `QXmlStreamReader` parser
  (`DJ_PLAYLISTS/COLLECTION/TRACK`, `TEMPO`, `POSITION_MARK`, `PLAYLISTS/NODE`).
  It converts `file://localhost/…` locations to local paths, Rekordbox ratings
  (0/51/…/255) to 0–5 stars, `Colour` to `#rrggbb` and `Tonality` to Camelot.
  Tested by `BrockDJ_rekordbox_xml_tests`.
- `LibraryDatabase::importRekordboxXml()` (`LibraryRekordboxImport.cpp`) parses
  in the background and applies in one transaction on the main thread. It runs as
  the health task `"rekordbox"` and reports through `healthTaskFinished`.
- Merge rules: tracks are matched by file path. Unknown tracks are added, including
  missing files, and a missing-file scan follows so that *Sammlung prüfen* can
  relocate them. Existing values are never overwritten: rating, colour, comment,
  genre, album, key and BPM are only filled when empty, and play count takes the
  maximum. Hot cues go into free slots, the first memory cue becomes the main cue
  if none is set, and loops go into free saved-loop slots.
- Beatgrids: `TEMPO` nodes are expanded into beats and stored as a **locked**
  grid, only for tracks that are not analysed and have no user grid. The track
  stays `is_analyzed = 0`, so batch analysis still produces the waveform, key and
  phrases while the locked grid is preserved. `DjEngine` also accepts a locked
  grid on unanalysed tracks when a deck loads them.
- Playlists are recreated below a top-level folder named "Rekordbox". Both
  `KeyType` 0 (TrackID) and 1 (location) playlists are supported.
