# Master loudness meter (LUFS)

BrockDJ measures the loudness of the master output following ITU-R BS.1770 / EBU R128
and shows a short-term LUFS readout next to the master VU in the top bar.

## Signal path

`AudioEngine::processChunk` → `MasterMixer::finalize` (limiter) → recorder capture →
**`KWeightedBlockMeter`** → output. The meter therefore sees exactly what goes to the speakers and to
recordings.

## Components

| Component | Thread | Responsibility |
|---|---|---|
| `audio/LoudnessMeter.h` `KWeightedBlockMeter` | audio | Applies the BS.1770 K-weighting (shelf plus high-pass, with coefficients derived for any sample rate) and emits the mean-square energy of each 100 ms block, summed over L+R. It never allocates or locks. |
| `LoudnessBlockQueue` (`AudioEngine::s_loudnessBlocks`) | audio → UI | A lock-free SPSC ring with 256 entries (25.6 s). If the UI stalls, blocks are dropped rather than blocking the audio thread. |
| `LoudnessAccumulator` | UI | Momentary (400 ms) and short-term (3 s) values, max short-term, and integrated loudness. Integration uses a histogram of 0.1 LU bins from -70 to +20 LUFS with an absolute gate at -70 LUFS and a relative gate at -10 LU, so memory is constant even over long sets. |
| `app/LoudnessMonitor` | UI | A QObject owned by `MixerControl` that drains the queue every 100 ms and is exposed to QML as `mixerControl.loudness`. |

## QML API (`mixerControl.loudness`)

- `momentary`, `shortTerm`, `integrated`, `maxShortTerm` hold LUFS values. A value equal to `floorLufs` (-70) means silence or no data yet.
- `hasSignal`
- `targetLufs` is the target loudness, default -9 LUFS for club use, clamped to -30..0. It is persisted in the UI state under `loudnessTargetLufs`.
- `reset()` restarts the integrated measurement.

## UI

`TopHeader.qml` (`masterLoudnessReadout`) shows the short-term value, coloured against the target:

| Short-term value | Colour |
|---|---|
| within ±1 LU of the target | green |
| more than 1 LU above the target | orange |
| more than 3 LU above the target | red |

Interaction:

- The tooltip shows M/S/I and max.
- A click resets the measurement.
- The mouse wheel moves the target by 1 LU.
- The readout is hidden in the very compact top bar.

## Tests

`tests/audio/loudness_meter_tests.cpp` (`ctest -R loudness_meter`) checks:

- The BS.1770 reference: a 0 dBFS 997 Hz sine in one channel reads -3.01 LUFS at 44.1, 48 and 96 kHz.
- Stereo and -20 dB levels.
- Absolute and relative gating.
- Queue ordering and overflow.
