# State Ownership

A snapshot or atomic used only for thread transfer is not a second product owner.

| State | Authoritative owner | Read-only/transfer views |
| --- | --- | --- |
| Track path, identity, cache handle | Deck playback / `DeckTrackLoader` | `DjEngine` metadata facade |
| Source, audible, slip position | `DeckTransport` | atomic audio playhead |
| Loop and reverse | `DeckTransport` | cache and render-mode commands |
| Scratch position | Scratch processor | platter/display snapshot |
| Direct/Keylock/Scratch mode | `RenderModeRouter` | `DjEngine` status |
| Tempo value | Deck playback | stretch/render-mode commands |
| Trim, EQ, filter, Color FX | `DeckChannelProcessor` | coherent audio snapshot |
| Deck FX and tail lifecycle | `DeckChannelProcessor` / persistent `FxProcessor` | `FxManager` commands |
| Channel fader | `DeckChannelProcessor` | `MixerControl` command facade |
| Crossfader value/curve/assignment | `MasterMixer` | `AudioParameterStore` snapshot |
| Master FX, gain, limiter | `MasterMixer` | `AudioParameterStore` snapshot |
| PFL selection and headphone mix/gain | `HeadphoneBus` | `AudioParameterStore` snapshot |
| Master meter | `MasterMixer` | atomic UI meter values |
| Physical output assignments | `AudioOutputRouter` | persisted settings |
| Audio device | `AudioDeviceService` | settings and deck query facade |
| Waveform and beatgrid | `TrackData` / `WaveformStore` | immutable rendering snapshots |
| Library records/playlists | `LibraryDatabase` | model values |
| UI display state | QML/UI facade | controller feedback snapshots |

`DjEngine`, `MixerControl`, MIDI, HID, and QML publish commands and display snapshots. They
do not own DSP state or create alternate audio paths.
