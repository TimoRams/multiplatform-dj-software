# Using stems

[← Overview](../README.md) · [Installation](installation.md)


BrockDJ splits a track into **vocals, melody, bass and drums** with the
HT-Demucs model running locally through ONNX Runtime. On the deck's **STEMS**
pad page, pads 1–4 mute a stem and pads 5–8 solo it. Muted parts fade to grey
in the waveform, so you can see what is still audible.

Separation runs in the background and starts at the playhead when a track is
loaded. Sections that are not separated yet play the normal mix. The pad tab
shows `STEMS WAIT` until the playhead is covered, then the progress in percent. On a modern laptop CPU the
first separated audio is usually ready a few seconds after loading, while a
full track can take a few minutes, depending on the hardware.

For gigs, prepare tracks in advance: **Stems Preanalysis** in the library
separates the current view in the background, shows its progress in the bar
at the top and marks finished tracks with a green **S**. Results are cached on disk (up to 8 GB, oldest entries removed
first), so prepared tracks are ready immediately.

**Setting up stems:**

1. Before configuring CMake, download ONNX Runtime once:
   `./scripts/fetch-onnxruntime.sh`. In an existing build directory, run
   `cmake -S . -B build` again afterwards.
2. Start BrockDJ and open **Settings → DJ / Sync → Stems → Download**. This
   installs the 166 MB model once; it is not part of the repository.
3. Load a track and open the **STEMS** pad page.

Stems need several GB of RAM while separating. Without ONNX Runtime BrockDJ
still builds and runs normally, just without stems.

GPU acceleration is experimental and off by default (**Settings → DJ / Sync →
Stems → Experimental GPU acceleration**, takes effect after a restart). If it
fails, BrockDJ falls back to the CPU.

- **Intel iGPU on Linux:** install the OpenVINO plugin once with
  `python3 scripts/fetch-stems-openvino.py`.
- **macOS:** uses CoreML if the installed ONNX Runtime provides it.

Details are in [docs/architecture/stems.md](architecture/stems.md).
