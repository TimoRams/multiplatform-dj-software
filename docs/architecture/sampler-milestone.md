# Recording shortcut and sampler milestone

The top pull-down menu contains a recording toggle with HH:MM:SS duration and a
Sampler shortcut. The fixed header does not resize when the menu opens. The
Sampler and FX have dedicated top-bar tabs and full workspace pages in desktop
and AIO modes. Opening either page hides the performance/library surface without
stopping playback; no bottom sampler dock remains. Smaller surfaces scroll their
page controls. New controls retain square corners.

## Recording

SettingsManager persists the shared recording directory. Bootstrap publishes it
to SetRecorder, so settings, quick access and MIDI use the same destination.
`toggleRecording()` and `requestStopRecording()` never join the writer on UI stop;
the `stopping` property disables repeated toggles until the writer drains. The
blocking `stopRecording()` remains the explicit shutdown/test barrier. Existing
WAV format, track marks, split files and failure reporting remain in force.

## Sampler ownership and audio

AudioEngine owns SamplerAudioEngine separately from its single registered preview
endpoint. SamplerController is the control-thread QML/MIDI facade, exposed as
`sampler`; DjEngine is only consulted when taking a deck capture snapshot.

Four global banks contain eight slots each. Up to eight voices may play at once,
one per slot. A full sampler replaces the oldest one-shot; eight playing loops
reject another start with BUSY state. Bank changes and closing the panel preserve
playback. One-shots retrigger and loops toggle; STOP ALL preserves command order.
Slot generation checks discard presses queued for a replaced/cleared asset.

The sampler page has an Electribe-inspired eight-track sequencer with 16, 32, 48
or 64 sixteenth-note steps per bank, displayed in sixteen-step pages. Each step
stores velocity (1-127; zero disables it), applied to actual program and cue PCM.
Click a step to toggle/select it, or hold to select it without toggling; the
velocity slider edits that selected step. Track Mute/Solo filters sequenced
voices, fading existing sequenced playback without silencing manually played
pads. Swing delays alternating sixteenth-note steps. Internal tempo is adjustable
from 20-400 BPM; an active deck or Link clock takes priority.

The editing/selected bank is distinct from the audio thread's running bank.
The running pattern button pulses from the audio playhead even while another bank
is selected. That other bank is marked NEXT and starts at step one only after the
running pattern completes its full configured length. The latest request wins;
selecting the running bank cancels NEXT. Length edits also take effect at the next
cycle. Stop clears queued playback; STOP ALL also disables the sequencer.
Sequencing starts at the next sixteenth-note boundary with local step one,
without resetting the shared DJ clock. Master/Link clock jumps preserve the local
playhead and queued selection rather than forcing a pattern switch.

AUTO FILL (off, every 1/2/4/8 bars, 0-100 % density) adds extra hits to empty
steps in the last beat before the chosen downbeat. Only one-shot tracks that
already contain hits take part; fill velocity follows the track's peak and
grows towards the downbeat. The decision is a hash of bar, step and track
(`SamplerAudioEngine::fillVelocity`), so a fill is deterministic and needs no
allocation or random state on the audio thread. A LED shows when the running
bar is filling. ROLL (1/16, 1/8, 1/4, 1/2) is a momentary beat repeat: while
held, the step lookup loops from the engage point while the real playhead keeps
running, so releasing drops back in time. Fills pause during a roll. Fill
settings persist in the manifest (`fillEvery`, `fillDensity`); the roll does not
and is cleared when the sequencer stops.

Pattern COPY asks for confirmation and copies steps, length and mute/solo to
another bank, not its samples. Changes save through the existing QSaveFile
manifest before audio publication. Version 2 persists velocity arrays, lengths,
track settings and internal tempo; version 1 masks migrate to sixteen steps at
full velocity. Playback transport and queued requests are not restored.
Step scheduling and starts run in the callback using fixed-size atomics and
preallocated voices. Rendering splits at grid boundaries so a change inside a
callback cannot cut off the old pattern early; no UI timer triggers audio.
This is a sample sequencer, not a full ESX/ES2 emulator: synthesis, motion
recording and song-mode pattern chains are not implemented here.

All samples are immutable resident stereo float PCM. Imports reserve from a
shared 256 MiB budget before decoding (including concurrent import buffers), with
a 30-second limit and mono duplication. QtConcurrent performs decoding, waveforms
and managed WAV creation. File imports and deck captures create unique local
24-bit WAV assets, independent of removable media. Deck capture snapshots a raw
loop or grid-aligned beat range, before deck FX/fader/tempo. Bank metadata uses a
versioned QSaveFile manifest. Failed imports keep previous audio; metadata must
commit before replacing an existing slot or deleting its old managed asset.

Publication uses a control-side gate and reader counter covering render and DSP
seed reads. The callback never waits: it skips sampler rendering during a slot
publication, while deck playback continues. PCM reclamation happens off callback.
Commands use a bounded SPSC queue (GUI/MIDI dispatch is serialized on the owner
thread). Voice and scratch/DSP storage is prepared with the audio device. Start,
stop, retrigger, level and cue transitions are ramped; loop seams are blended.

Sampler program is added before final master FX/gain/mono/limiting and recording.
Pre-fader sampler CUE is added alongside preview before headphone gain/limiting,
independently of CUE/Master blend. The same private-output check suppresses both
when headphones are missing or overlap master/booth. Program remains independent
of deck faders/crossfader. Recorded WAVs include sampler program exclusively.

## Timing and controllers

Quantization is Off, 1 beat or 4 beats; synced loops default to one beat. One-shots
start immediately. A monotonic timestamped clock selects the existing sync master
or active Link, otherwise the first usable forward-playing deck A–D, otherwise
free-runs internally at the last tempo (120 BPM initially). Link is sampled at the
current instant rather than using its slower UI polling snapshot. Pending starts
are placed inside the audio block; a reference change reschedules a pending start.
Playing loops preserve position and follow phase gradually without a hard seek.
Their preallocated tempo timeline preserves already scheduled read-ahead positions
across clock updates. New targets ramp over at least 25 ms; this avoids pitch-map
and audio-position jumps during rapid tempo moves or master handovers.

Each voice reuses TimeStretchProcessor with a resident read-ahead source for
pitch-preserving sync. Tempo ratios are bounded to 0.25–4; extreme ratios outside
this range cannot follow the clock exactly. The existing worker seed/bridge
behavior applies at keylock entry; this is not a guarantee of instant spectral
settling or of live performance on unmeasured target hardware.

Existing FLX10 sampler pad notes trigger global sampler slots even without a deck
connection/track; Key Shift and hot-cue banks keep their separate routes. Sampler
LED updates send only changed values. No speculative hardware CC/note mappings
are added. Generic mappable IDs: `sampler_pad1`–`sampler_pad8`,
`sampler_bank1`–`sampler_bank4`, `sampler_bank_next`, `sampler_bank_previous`,
`sampler_gain` (normalized 0–1 maps to 0–1.5), `sampler_cue`, `sampler_stop_all`.
Existing recording command IDs remain unchanged.

The live page/editor and documented physical Sampler CUE/Volume mapping are
extended in [live editing and FLX10 recovery](sampler-live-editing.md), including
private editor preview, frame trim, audio-derived telemetry and bounded HID retry.

## Effects page

The FX page uses the existing FxManager and the same two units as MIDI/sidebars.
It never creates a second effect chain or silently changes deck assignments on
entry. FX Settings exposes effect choice, independent A–D assignment, wet/dry,
sync, quantization, beat division, Reverb room size and Sound Color character.

Touch FX maps X to wet mix and Y to synchronized beat division (timing effects)
or room size (Reverb). Timing effects require an assigned deck with BPM. It is
momentary: press snapshots enabled/mix/parameter/division/sync/quantize, and release
or cancellation restores them. Page/unit/section/manager changes, effect selection,
window deactivation and destruction cancel the gesture. Sampler voices and deck
transport are independent of page navigation. The editor's library/drop shortcuts
open the sampler page; the selected main page restores on restart.

## Validation and remaining roadmap

`sampler_audio` checks PCM output, pre-fader cue, retrigger/stop, voice limits,
sample-rate conversion, sample-position quantization, swung sequencer triggers,
sample-accurate queued bank transitions, 64-step playback, velocity, Mute/Solo,
clock handovers and pitch preservation at device callback cadence. It checks DSP realtime counters
for forbidden callback preparation, growth, locks and inline seeds.
`deck_audio_graph` checks simultaneous
preview/sampler routing and reads actual recorded PCM, then runs four decks,
eight sampler voices, preview and recording together across mixed callback sizes.
`ci_runtime_smoke` exercises
imports, trimming, failed replacement, source deletion, bank restoration, deck
capture and MIDI triggering without a connected deck. `qml_component` exercises
real recording-button and sampler-panel input at 800/1280/1920 widths. Optional
`BROCKDJ_TEST_CAPTURE_DIR` saves a panel image for visual inspection.

For this sequencer update, Linux RelWithDebInfo sampler, QML and audio-routing
tests pass, as do Debug application startup and persistence/migration checks.
The Debug sampler executable passes the new sequencing checks but retains a
pre-existing pitch-preservation failure: approximately 528 Hz instead of 440 Hz
at 144 BPM. The unchanged HEAD sampler sources/tests, compiled with the same
Debug flags and linked dependencies, reproduce the same failure. It is not
claimed fixed by this update; real audio-device acceptance remains separate.

On the macOS development host, sandbox-launched startup checks intermittently
timed out after LinkManager initialization, before the system-monitor/cache
startup messages. The same early-close and full runtime/package/startup checks
passed through CLion's Debug run configuration. The precise cause of those
environment-sensitive startup stalls is not established or claimed fixed here.

The previous tempo-sweep defects are resolved; `keylock-stability.md` documents
the fix and unchanged acceptance limits. The API source contract failure remains
enabled and unresolved. Real-interface/USB testing and
extended four-deck/eight-voice recording stress remain hardware acceptance work.

Still open after this milestone:

- Touch Cue and Slicer.
- Further touch browser/search/filter/sort and large-library preparation workflows.
- Stems separation, cache/preparation, pad and mixer control.
- Additional Touch FX mappings and dedicated effect assignments (including sampler-specific FX).
- Streaming service integrations and cloud library/availability handling.
- Music-synchronized lighting.
- Independent zone output, microphone processing and talkover.
- Obsolete API source assertion, USB-removal and
  long-duration recording acceptance, environment-sensitive startup stalls,
  and measured target hardware audio load.
