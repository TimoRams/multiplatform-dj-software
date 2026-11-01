# FLX10 display transport and FX research

Reviewed 2026-11-01.

## Current application behavior

Jog-screen contents use HID: `xx27` state, `xx36` waveform, cover uploads,
and initialization packets. Cue/loop markers are encoded into the HID PWV5
waveform. The experimental channel-16 MIDI main-cue needle has been removed;
its coexistence with this HID mode was never verified on hardware.

MIDI is still needed for the existing session SysEx activation/keepalive and
LED output. Keeping jog contents on HID does not mean removing that session
handshake. No MIDI time, BPM, or cue-needle output should be added to this
display mode without a separate compatibility test.

FX LCD contents are not implemented. The existing effect selectors control
application effects, but that input mapping does not define the output format
for the physical FX display.

## Evidence checked

- [VirtualDJ's FLX10 mixer documentation](https://virtualdj.com/manuals/hardware/pioneer/ddjflx10/layout/mixer.html)
  explicitly describes effect name, assigned channel, and beat value on the
  FX LCD. It establishes working software support, but does not specify packets.
- [The open FLX10 mapping/research project](https://github.com/Veezuhz/Mixxx_FLX10_Controller_Mapping)
  (commit `c668a0b0aa3d1230e19806380f1a711ea7a0f271`)
  was downloaded and searched, including screen JavaScript, Python helpers,
  and protocol notes. No FX LCD encoder was found. Its BeatFX selection
  handlers and software-effect scanner are not physical FX-screen output.
- The locally installed Rekordbox 7 resource
  `Contents/Resources/MidiMappings/DDJ-FLX10.midi.csv` describes FX control
  input, state indicators, and `OledBrightnessSetting` (`BF4A`). These are
  separate from an encoder for effect names/BPM/beat values. Brightness is a
  device setting, not proof of an FX-content message format; it was not sent.
- No FLX10 USB capture was found in the repository, the downloaded research
  archive, Downloads, or `/private/tmp` during this investigation.

## Capture needed for the next implementation

Record one functioning Rekordbox or VirtualDJ session with the FLX10. Preserve
USB descriptors, session initialization, all host-to-device HID/control/MIDI
transfers, and device responses. Do not assume FX uses the jog-screen endpoint
or 128-byte report layout before checking the trace.

Keep track playback paused to reduce unrelated changes and record timestamps
or a short video of the FX LCD during these separate actions:

1. Leave the initial FX display unchanged for a few seconds.
2. Switch ECHO to REVERB, then back to ECHO.
3. Change only the beat value: 1 to 1/2, then back to 1.
4. Change only the assigned channel: CH1 to CH2, then MASTER.
5. Change only the FX BPM between two known values, including decimal values.
6. Toggle FX on/off, then change level/depth independently.

Compare repeated transitions to distinguish content from counters, periodic
refresh, and initialization. Validate each inferred field by changing only
that field before integrating it into the application.

## Performance requirements for an FX implementation

Encode from a small cached FX state on the controller owner thread. Coalesce
changes and suppress identical output. Reuse the bounded HID writer if the
capture confirms that transport, and preserve priority for jog state. Add a
heartbeat only if measured device behavior requires it. No FX display work,
allocation, process launch, or USB I/O belongs on the audio callback.
