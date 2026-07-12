# Thread Ownership

Last updated: 2026-07-12

| Object/state | Owner/lifetime | Allowed callers | Blocking/allocation | Audio-callback rule |
|---|---|---|---|---|
| `AudioDeviceService` | `ApplicationRuntime`; created before decks, destroyed after decks/master bus | Qt application/control thread | enumeration, device open/close, JACK probing and configuration may block/allocate | never call service configuration/enumeration from callback |
| `AudioDeviceService::ConfigurationSnapshot` | service, Qt owner thread | service apply/device-notification path; deck/QML readers on Qt thread | trivial | callback does not read it |
| `DjMasterBus` routing/master atomics | master bus/static atomics; callback unregistered before destruction | service/bootstrap publish on Qt thread; audio callback reads | publishing is trivial atomic work | callback read-only, no device query |
| `DjEngine` facade | runtime, one per deck; destroyed before service | QML/MIDI/control thread unless method says atomic/audio-only | transport/device changes outside callback | `getAudioSource`/PFL and DSP processing follow explicit audio path |
| Deck audio graph (`MixerDspSource`, time stretch, scratch bridge) | each deck | prepare/configure on control/device callback boundary; process on audio callback | preparation may allocate; processing must not | realtime rules apply strictly |
| Device enumeration/query helpers | service/control path | Qt owner thread only | may scan hardware, spawn probe manager, lock caches | forbidden |
| Track loader/analyzer | deck-owned jobs/workers | loader/analyzer workers plus guarded Qt callbacks | file/decoder work allowed | forbidden |
| Sync global registry | currently static `DjEngine`; future `SyncCoordinator` | Qt 4 ms control/UI with `s_syncMutex` | short mutex only outside callback | forbidden from callback |

Shutdown order: stop new UI changes → prepare decks → unregister master callback → close service device → release deck readers → destroy decks → destroy master bus → destroy service. Qt connections from the service to decks disconnect automatically when each deck is destroyed.
