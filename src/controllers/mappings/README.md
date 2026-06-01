# Built-In Controller Mappings

Drop future bundled controller mappings here.

- `midi/` should hold normal MIDI mappings that feed `MidiControllerManager`.
- `hid/` should hold HID or vendor USB packet notes, schemas, and mappings.

The DDJ-FLX10 currently keeps normal control input on the existing MIDI mapping
system. Its HID screen support is implemented separately in `../flx10/`.

