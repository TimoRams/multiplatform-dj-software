# State Ownership

The table distinguishes a single product owner from read-only, atomic, UI-coalesced, or persisted
copies. A copy used only to cross a thread boundary is not automatically a second product owner.

| State | Current owner | Other copies/readers | Risk / intended direction |
| --- | --- | --- | --- |
| Track path, identity, load generation | `DeckTrackLoader` -> `DjEngine` apply result | cache handle, `DeckTransport` generation | loader prepares; facade publishes metadata |
| Track metadata and cover URL | `DjEngine` Qt facade | QML, library DB | retain facade snapshot |
| Playback position and playing | `DeckTransport` | graph/JUCE position, atomic visual playhead | high; transport is product authority |
| Reverse and slip | `DeckTransport` | cached source and scratch bridge commands | high; keep one transport owner |
| Loop, hot cues, saved loops, main cue | `DeckCueLoopController` | `DjEngine` persistence/QML facade | controller owns domain state |
| Tempo/range/keylock | `DjEngine` facade | transport/bridge/stretch effective values | document write direction |
| Scratch session and platter state | `ScratchSession` / `ScratchController` | bridge display handoff | high; bridge is consumer |
| Sync enable/master/phase control | `SyncCoordinator` + `DeckSyncController` | `DjEngine` signals/properties | no engine-to-engine owner |
| Trim, EQ, filter, polarity | `MixerControl`/`DjEngine` command state | DSP atomics and prepared banks | designate one control truth |
| Channel fader | `MixerControl::ChannelMixState` | `DjEngine::m_volume`, mixer DSP fader | highest duplicate-state audit |
| Crossfader | `MixerControl` | applied channel multipliers | currently control-side routing |
| PFL and headphone mix | graph cue atomic / `DjMasterBus` settings | `DjEngine` compatibility properties | product scope needs documentation |
| Deck FX state | `FxManager` routing | `MixerDspSource`/`FxProcessor` audio state | manager routes, processor owns DSP state |
| Channel/master meters | audio DSP/bus atomics | Qt-throttled `DjEngine` notifications | display-coalesced copies |
| Waveform RGB/full/overview | `TrackData` | cache payload, FLX10 projection | high duplication cost |
| Waveform canonical lines | `TrackData::WaveformLineStore` | immutable Qt Quick snapshots | clear rendering owner |
| Beatgrid and analysis metadata | `TrackData` | immutable result and DB payload | result is transfer data |
| Library records/playlists | `LibraryDatabase` | model values | DB is data authority |
| Audio device configuration | `AudioDeviceService` | settings and deck query facade | service is sole device owner |

The next state-focused task should audit channel-fader authority (`MixerControl`, `DjEngine`, DSP),
then document rather than prematurely merge the tempo and waveform cross-thread copies.
