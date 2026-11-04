# Recording shortcut and sampler milestone

The top pull-down menu contains a recording toggle with HH:MM:SS duration and a
Sampler shortcut. The fixed header does not resize when the menu opens. The
sampler docks beneath the workspace, which reserves its height; smaller surfaces
can scroll sampler controls. New controls retain square corners.

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

## Validation and remaining roadmap

`sampler_audio` checks PCM output, pre-fader cue, retrigger/stop, voice limits,
sample-rate conversion, sample-position quantization and pitch preservation at
device callback cadence. It checks DSP realtime counters for forbidden callback
preparation, growth, locks and inline seeds. `deck_audio_graph` checks simultaneous
preview/sampler routing and reads actual recorded PCM, then runs four decks,
eight sampler voices, preview and recording together across mixed callback sizes.
`ci_runtime_smoke` exercises
imports, trimming, failed replacement, source deletion, bank restoration, deck
capture and MIDI triggering without a connected deck. `qml_component` exercises
real recording-button and sampler-panel input at 800/1280/1920 widths. Optional
`BROCKDJ_TEST_CAPTURE_DIR` saves a panel image for visual inspection.

On the macOS development host, sandbox-launched startup checks intermittently
timed out after LinkManager initialization, before the system-monitor/cache
startup messages. The same early-close and full runtime/package/startup checks
passed through CLion's Debug run configuration. The precise cause of those
environment-sensitive startup stalls is not established or claimed fixed here.

The previous live milestone documents the pre-existing tempo-sweep and API source
contract failures; those tests remain enabled. Real-interface/USB testing and
extended four-deck/eight-voice recording stress remain hardware acceptance work.

Still open after this milestone:

- Touch Cue and Slicer.
- Further touch browser/search/filter/sort and large-library preparation workflows.
- Stems separation, cache/preparation, pad and mixer control.
- Touch FX and expanded dedicated effect assignments (including sampler-specific FX).
- Streaming service integrations and cloud library/availability handling.
- Music-synchronized lighting.
- Independent zone output, microphone processing and talkover.
- Existing tempo-sweep defects, obsolete API source assertion, USB-removal and
  long-duration recording acceptance, environment-sensitive startup stalls,
  and measured target hardware audio load.
