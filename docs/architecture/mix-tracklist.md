# Mix tracklist & now playing

`MixTracklistController` (`src/app/`, QML: `mixTracklist`) decides which track
the audience is hearing and records it.

## Audible track detection

Pure logic lives in `src/audio/MixTracklist.h` (`AudibleTrackTracker`, tested by
`tests/audio/mix_tracklist_tests.cpp`).

- Polled at 4 Hz. Per deck: playing + loaded ⇒ weight =
  `AudioEngine::deckAirGain()` (actual channel-fader gain × crossfader gain,
  published by the audio callback). Not playing ⇒ 0.
- The loudest deck above 0.05 is *dominant*; the current deck gets a 5 % bonus
  so equal-gain blends do not flap.
- A deck must stay dominant for **8 s** before it becomes "now playing"; the
  entry is back-dated to when it first became dominant. Quick cuts, scratches
  and cue checks are therefore ignored.
- The same file on another deck does not create a duplicate entry.

## Set tracklist

While `SetRecorder` runs, every new entry is appended (time relative to the
recording start) and `<recording>.cue` + `<recording>.txt` are rewritten
atomically (`QSaveFile`) next to the WAV, so a crash still leaves a valid list.
A track already on air when recording starts is listed at `00:00:00`.

- `.txt`: `hh:mm:ss  Artist - Title`
- `.cue`: single-file cue sheet (`INDEX 01 mm:ss:ff`, 75 frames/s), first index
  forced to `00:00:00`.

Limitation: when the WAV is split (TRACK MARK or the 2 GiB part limit) the cue
sheet still references the first file with set-relative times; the `.txt` is
the authoritative list.

## Now-playing export (stream overlays)

Settings → Live → *Now playing export* (`UI/nowPlaying/exportEnabled`,
`UI/nowPlaying/exportFolder`; default folder `<config>/NowPlaying`). On every
now-playing change the controller atomically writes:

| File | Content |
| --- | --- |
| `nowplaying.txt` | `Artist - Title` (OBS *Text* source, "read from file") |
| `nowplaying.json` | `{title, artist, deck, bpm, key, updatedAt}` for bots/scripts |
| `nowplaying.js` | `window.brockdjNowPlaying = {...};` |
| `overlay.html` | Transparent lower-third card for an OBS *Browser* source (local file) |

`overlay.html` re-injects `nowplaying.js` every second because browsers block
`fetch()` on `file://`. It is only created when missing, so users can restyle it.
The last track stays visible while nothing new has become dominant.
