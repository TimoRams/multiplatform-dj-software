# Sampler live controls, editor and FLX10 recovery

The sampler retains four banks, eight performance voices and the version-1 bank
file. The live page adds clip waveforms, audio-derived position/loop progress,
quantized-start countdowns, activity counts across banks and independent Stop/Edit
hit areas. Every sampler/editor action has a 44-logical-pixel touch target;
controls and text fields keep square corners and the shared dark theme.

## Audio and editor ownership

`SamplerAudioEngine` publishes per-slot generation, state, clip position,
duration, progress and pending beats through preallocated atomic cells. Readers
make at most two snapshot attempts; a busy read retains the controller's last
snapshot. UI notification runs in ControlClock's existing 10 Hz statistics group,
separate from metadata notifications and MIDI LED changes. Reading progress never
advances transport. The displayed length is the trimmed PCM length, not the
untrimmed asset's duration.

`SamplerController` binds each editor session to an absolute slot and generation.
Bank changes cannot redirect saves. Capture prepares a draft; only Save publishes
it. Cancel invalidates pending operations, while stale results dispose their new
assets without replacing the old slot. Loading, decode, RAM and persistence
failures retain the prior sample and expose a fixed-footer error. A successful
replacement uses the existing fade/retirement policy; it does not auto-trigger.

The overview remains bounded. Detail requests cover at most 30 seconds and produce
1024 peak bins using 4096-frame decoder chunks on workers. Panning/zooming cancels
superseded jobs. Trim positions round to source frames, with a minimum two-frame
selection. Start/End selection disambiguates overlapping touch handles. Metadata,
loading/capture controls and fixed completion actions adapt to the viewport.

Editor preview uses a separate resident PCM voice inside the sampler, without
consuming any performance voice or registering another AudioEngine auxiliary
endpoint. It plays at source tempo, respects the draft's loop mode and uses short
fades. Its signal goes only into the headphone bus, independently of sampler
program level and sampler CUE; headphone gain/limiting still apply. It never enters
master, booth or recorded PCM. An active device and non-overlapping headphone
outputs are required. Source/range changes, cancellation, saving and closing stop
preview and invalidate pending loads. Preview PCM shares the 256 MiB budget and
is reclaimed off the callback after its fade completes. No decoding, persistence,
allocation, preparation or blocking lock is added to rendering.

## FLX10 sampler controls

The [official MIDI message list](https://www.pioneerdj.com/-/media/pioneerdj/software-info/controller/ddj-flx10/ddj-flx10_midi_message_list_e1.pdf)
defines normal sampler CUE as channel-7 Note `0x69` (`0x96`) and sampler volume as
channel-7 14-bit CC `0x03/0x23` (`0xB6`). Both pair orders use the existing MIDI
accumulator. CUE dispatch is momentary/edge-filtered; releases and repeated note-on
messages cannot toggle twice. Its LED follows the actual SamplerController CUE
state, including touch changes, rejected routing requests and feedback resync.

Volume maps to the existing 0–1.5 program gain range. Pickup is armed after a
connection or software level change; acquisition occurs within `1/127` normalized
travel or when crossing the software value. The pending indication is shown only
after a physical knob has been observed. Gain persistence is debounced for 250 ms
of inactivity and flushed at clean shutdown. CUE remains pre-fader, so program
volume zero still allows monitoring. MIDI controls do not depend on HID display
connection success.

## Display recovery

`ConnectionPolicy` distinguishes absence, incompatible HID, denied access,
ambiguous devices and transient failure. A passive five-second probe enumerates
stable device identities without opening any endpoint. Unchanged absence or
configuration errors never restart the controller. A changed identity/interface
set permits one attempt; transient failures receive at most two additional
attempts, after five and fifteen seconds. Manual Retry resets the budget;
disable/shutdown cancels it. Unsupported platforms keep their existing capability
gate. macOS never detaches audio/MIDI drivers.

Internal transport cleanup does not announce that the feature was disabled.
Quiet retries retain the last error, so identical failures do not alternate with
Starting/Disabled messages. Settings show a stable display-specific state and a
manual retry action. This addresses retry/log churn; it does not establish support
for a vendor-driver configuration that hides the screen's HID service.

## Validation

- `flx10_display_protocol`: stable-error suppression across 1000 polls, changed
  identity, retry deadlines/budget, manual reset and disable.
- `sampler_audio`: real progress, pending-master rescheduling, pickup, private
  preview at full voice capacity, source-rate conversion, bounded replacement
  tails (including two-frame clips), reclamation and concurrent replacement with
  guaranteed audio callbacks.
- `deck_audio_graph`: editor/library preview plus sampler CUE remain private;
  actual recorded WAV contains only program audio; overlapping routes remain
  muted. Existing tempo-sweep and realtime limits remain unchanged.
- `qml_component`: live controls and editor at 800×600, 1280×800 and 1920×1080,
  trim dragging, fixed footer/error visibility, no accidental triggers and stable
  editor target across bank changes. Optional `BROCKDJ_TEST_CAPTURE_DIR` captures
  the live page and editor for visual inspection.
- Application smoke: async editor import/save, cancellation, failed save/clear,
  draft-only capture, persistence, actual headphone output and real MIDI dispatch
  through the built-in mapping without hardware access.

Physical MIDI/LED behavior, driver-specific HID recovery and full-device callback
headroom still require the hardware acceptance matrix. Existing environment-
sensitive startup stalls are tracked separately; software checks do not imply
that those or target-device latency limits are resolved.

On the macOS development host, the original missing-HID scenario was checked
again in the native debugger across successive hotplug timer callbacks. The
failure stayed `Absent`, retries stayed zero and no attempt was scheduled
(`due = -1`). The log contained one Starting message and one missing-HID error,
with zero reconnect/Disabled churn. The complete application/package/startup
smoke passed through CLion; direct sandbox-launched startup retained the known
intermittent LinkManager-stage timeout. These are separate results.
