# Controller Integrations

This folder is for native controller support that sits beside the generic MIDI
mapping system.

- `profiles/` contains controller capability metadata.
- `mappings/midi/` is reserved for built-in MIDI mapping files.
- `mappings/hid/` is reserved for HID/vendor-specific mapping notes and data.
- `flx10/` contains the first native DDJ-FLX10 HID display integration.

The existing MIDI mapping path still owns transport, mixer, jog, and pad
control. Native integrations should only handle device features that need HID,
vendor USB, display packets, or controller-specific handshakes.

Linux vendor USB access needs udev permission for non-root use. The bundled
DDJ-FLX10 rule lives at `packaging/linux/udev/70-brockdj-controllers.rules`.
