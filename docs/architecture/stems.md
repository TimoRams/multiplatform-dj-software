# Stems

With the optional model installed, BrockDJ separates a loaded track into
**VOCALS, MELODY, BASS and DRUMS** in the background. On the deck's **STEMS**
pad page, pads 1-4 mute/unmute and pads 5-8 solo the corresponding stem; press
the active solo again to restore all four. An FLX10's physical pad banks use
the same actions while the STEMS page is selected in the UI. A physical
pad-mode button switches back to its normal mode.

Separation is local background processing, not instant streaming inference. While the
playhead's data is unavailable, the original mix remains audible and the
page shows `WAIT`: a mute or solo takes effect once that section is ready.
Seeking may briefly return to the original mix. Status and errors are shown on
the page; a failed job can be retried there.

## Setup

ONNX Runtime 1.31.1 is pinned as the `libs/onnxruntime-src` Git submodule.
CMake builds it as an isolated external project and installs it under
`<build>/onnxruntime` on Linux x64/ARM64, macOS Intel/Apple Silicon and Windows
x64. CPU inference is always included; macOS also builds CoreML support.
CMake 3.28+, Python 3.10+ and `patch` are required. The first build downloads runtime
source dependencies; it does not download the separation model.

Use `git submodule update --init --recursive` after updating the checkout.
`BROCKDJ_ENABLE_STEMS=OFF` omits the runtime and separation entirely.
`BROCKDJ_REQUIRE_STEMS=ON` rejects builds without stems support.

Open **Settings -> Stems -> Install model** once to install the 166 MB HT-Demucs
ONNX model. The download is checked against a pinned SHA-256 and atomically
installed under `AppLocalDataLocation/models/`; it is not committed to Git.
`BROCKDJ_STEM_MODEL=/path/model.onnx` selects an existing model instead. The
model and runtime are MIT-licensed; see `NOTICE` for attribution.

The **Separate stems automatically when a track is loaded** setting is on by
default. Enabling it, or installing the model, also starts separation for
tracks already loaded. With auto separation off, using a stem pad starts a job.

## Library preanalysis

Use **Stems Preanalysis** beside the ordinary analysis button to prepare the
current library, playlist, collection, USB view or folder. In narrow layouts
the button reads **Stems**. The track context menu prepares an individual
track. The button and a green line at the bottom of the top header show the
overall queue progress, including the current track. Clicking the running
button stops the queue without cancelling deck jobs.

Paths are deduplicated, and up to 4096 tracks can be queued. Already prepared
tracks are loaded and checksum-verified by the worker, without rerunning the
model; damaged caches are rebuilt. Decode, model and disk errors are reported,
counted as failed attempts, and do not prevent later tracks from being tried.
Live decks take priority between inference windows, so preanalysis can wait
while decks are separating. If a deck completes the same track in the
meantime, the waiting preanalysis reuses its cache instead of repeating the
remaining model windows.

A green **S** beside a track means a matching cache file exists. Badges are
derived from the cache inventory, not a permanent database flag; pruning,
clearing or deleting files removes them. The inventory checks headers and
file sizes; full payload integrity is checked when loading. Changing the
source file invalidates its cache key. A preanalysis job only succeeds when
its completed cache has been written and retained within the disk budget.

## Optional GPU acceleration

**Settings -> Stems -> Experimental GPU acceleration** is off by default.
Changes take effect after restarting BrockDJ. The settings page reports the
backend actually selected after model loading and any CPU-fallback reason.
GPU initialization, model compilation or inference failures fall back to CPU;
missing acceleration never disables ordinary CPU stems.

- **Intel GPU, Linux/Windows x64:** requires ONNX Runtime 1.23+ and the Intel
  OpenCL GPU compute driver. Install the optional OpenVINO execution-provider
  plugin with Python 3.9+ using `python3 scripts/fetch-stems-openvino.py` (Windows:
  `python scripts/fetch-stems-openvino.py`). This installs the SHA-256-pinned
  1.8.1 plugin, OpenVINO/TBB libraries and license notices in the per-user
  `AppLocalDataLocation/stems-providers/openvino/`, without pip or changes to
  the CPU runtime. `--destination` overrides the installer path;
  `BROCKDJ_STEM_EP_LIBRARY` selects an alternative plugin library. Existing
  provider installations are not overwritten. Linux requires glibc 2.28+.
- **macOS:** uses CoreML with CPU/GPU compute units if the installed ONNX
  Runtime exposes that provider. CoreML support is not assumed on older Intel
  runtime builds. Native macOS inference has not been benchmarked here.
- **AMD/NVIDIA and Windows DirectML:** not implemented by this optional
  backend. The CPU path continues to work.

Unsupported operations may remain on CPU, and CPU/GPU transfers or initial
model compilation can outweigh GPU gains. This is an experimental opt-in,
not a guaranteed speedup or a fully GPU-resident model. The optional OpenVINO
package is not bundled in release artifacts; CPU stems remain self-contained
after installing the model.

The development Core Ultra 5 125H laptop exposed no Intel GPU compute device
to OpenVINO, despite having a render device. CPU fallback with the real model
was verified, but an iGPU speedup could not be measured without the compute
driver. An OpenVINO **CPU** probe took about 59 s to compile and 4.1 s per
window; it is deliberately not substituted for the normal CPU backend.
Increasing the CPU constant-folding limit to 16 MiB reduced a window from
about 4.8 to 3.9 s but raised peak RSS from about 2.5 to 5.9 GiB; this
configuration was rejected to preserve laptop stability.

## Design and performance

- One background worker shares one ONNX Runtime session across the decks
  and the preanalysis queue
  and prioritizes the playhead and playing decks. Distant unfinished windows
  behind the playhead do not block urgent work on another deck; a preceding
  window needed for the current overlap remains urgent. Inference threads are
  capped at four and run at reduced priority where supported. Runtime memory
  arenas and oversized graph constant folding are disabled or bounded to avoid
  retaining multi-gigabyte intermediate tensors.
- Tracks are processed in overlapping 7.8-second windows. Readiness is tracked
  in 4096-sample blocks; unfinished blocks play the unmodified track. Newly
  available audio and gain changes fade in over approximately 15 ms.
- Three stems are stored as stereo int16. Melody is the original mix minus
  vocals, bass and drums, so unity gains reproduce the original mix exactly.
  At 48 kHz, the separated data alone uses about 33 MiB per track minute, per
  deck; the inference model and audio page cache require additional memory.
- The normal playback reader, keylock read-ahead and scratch resampler all use
  the stem mix. Scratch windows are refreshed and crossfaded on gain/readiness
  changes; no model inference, file I/O or blocking mutex runs in the callback.
- Completed tracks are cached in `CacheLocation/stems/*.bstem` and
  least-recently-used entries are pruned above 8 GiB. Cache loading also runs in
  the background; damaged entries are discarded and recomputed. The cache can
  be cleared in Settings.
- Loading/ejecting a track cancels its job and resets its gains. Track IDs
  prevent stale data from affecting the next track.

CPU separation can take several seconds and requires substantial RAM. On the
development Linux x86_64 machine, a 30-second real-model check took about 7 s
to load the model, another 5 s to the first separated audio and 24 s total to
separate the track after loading; peak memory was about 2.5 GiB for the test
process. Times and memory vary by runtime and hardware; slower CPUs, multiple
decks or seeking can leave the worker behind playback. This is not a claim of
latency or quality parity with Serato.

## Waveform display of muted stems

While any stem gain differs from unity, the scrolling and overview waveforms
draw the full mix in neutral grey and overlay the still-audible share in the
normal colours, so muted vocals or drums are visible at a glance.

- The separation worker accumulates a per-stem RMS envelope with one value
  per 64 samples (Melody is derived as mix minus the other stems). Blocks of
  4096 samples are published atomically and independently of audio readiness.
- The v3 cache stores and checksums both audio and envelopes. Library stem
  preanalysis prepares both, and cached envelopes are published with the ready
  audio rather than decoded after load. Existing v2 audio caches keep their
  identity: a one-time source decode adds the envelope without running inference
  again. Preanalysis can perform this upgrade in advance.
- The audible share per column is `sqrt(sum(g^2 * E) / sum(E))`, treating stems
  as uncorrelated. On RHI backends (OpenGL, Vulkan, Metal and Direct3D), scrolling
  tiles contain the unchanged mix. Per-column energies are prepared even at
  unity gains, and a shader contracts the coloured waveform over a fading grey
  ghost. Gain changes update uniforms, not the tile cache, texture uploads or
  energy geometry. Missing envelope regions continue to display the mix.
- Scrolling and overview gains use a 180 ms ease-out transition. A new toggle
  retargets the current interpolated value immediately; transitions are never
  queued. Overview column energies and line aggregates are cached so animation
  does not repeatedly scan the track. The software scene-graph backend retains
  the CPU raster path and does not have the GPU path's upload-free guarantee.

## Verification

`stem_core` tests chunk scheduling, overlap, resampling, cache inventory,
preanalysis priority, cancellation, write failures and cache-budget failures
with a fake model, plus persisted envelopes, v2 upgrades and rapid transition
retargeting. `stem_waveform_animation` captures actual GPU frames and checks a
partial transition, stem-specific grey shading, exact restoration after 40
rapid toggles, and unchanged texture-upload/energy-geometry counts. It requires
a window-system RHI backend and skips offscreen/software platforms; run it with
`QT_QPA_PLATFORM=xcb QSG_RHI_BACKEND=opengl` or `vulkan` on Linux.
Setting `BROCKDJ_STEM_MODEL` to the verified model also runs
the real inference check; add `BROCKDJ_STEM_TEST_GPU=1` to exercise accelerator
selection and require an explicit reason for any CPU fallback. QML runtime
tests check the rendered progress fractions, start/stop actions, badges and
compact button; the app smoke test checks queue deduplication, failures,
cache reuse, corrupted payloads and external cache deletion.
`python3 tests/stems/openvino_install_tests.py` checks the optional installer,
including SHA mismatches, archive paths, notices and overwrite refusal.
Audio regression tests exercise
normal playback, reverse/loops, keylock read-ahead and scratching. The FLX10
MIDI routing is exercised without physical hardware; actual controller behavior
still requires an on-device check.
