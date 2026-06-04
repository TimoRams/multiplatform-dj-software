# Built-In Controller Mappings

Bundled controller mappings live here.

- `midi/` should hold normal MIDI mappings that feed `MidiControllerManager`.
- `hid/` should hold HID or vendor USB packet notes, schemas, and mappings.

The DDJ-FLX10 keeps normal control input on the existing MIDI mapping system.
Its bundled MIDI map uses BrockDJ's native XML format. The status/control
numbers were referenced from repo, but the
runtime loader only parses BrockDJ mapping entries. HID screen support is
implemented separately in `../flx10/`.
