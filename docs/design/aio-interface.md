# AIO interface design contract

BrockDJ prioritizes a hardware-controlled AIO workflow. Touch supplements track
browsing/loading, source selection, waveform tools and configuration. Existing
desktop controls and persisted preferences remain compatible.

## Shared visual language

`UiTheme.qml` owns working surfaces, text contrast, deck identity, touch geometry,
font sizing and display profiles. Working surfaces are opaque. Temporary
backdrops retain the existing bounded capture and software/pressure fallbacks.
Deck identity (A amber, B blue, C violet, D teal) is separate from playback,
headphone cue, sync, warning and clipping state. Always label important states.

Rectangular controls, buttons, panels, menus and badges have zero corner radius.
Inactive borders stay subtle; focus retains its distinct outline and active buttons
use their fill and bottom marker. Circular geometry is reserved for knobs and
turntable illustrations.

Use Button and ComboBox for shared interaction states and keyboard focus.
Performance action buttons derive from Button. UiIcon provides local vector
symbols for system status and settings navigation; no platform glyph dependency.
Text uses Inter; numeric font features are optional for Qt 6.4 compatibility.
Clock cells retain fixed widths and milliseconds have secondary visual weight.

Primary touch targets are at least 44 logical pixels, including at 80% UI scale.
The toolbar never drops below 44 pixels. Dense optional hardware/mixer controls
retain their established sizing; they are not a full touch mixing surface.

## Layout policy

Evaluate profiles on logical window dimensions divided by the user UI scale.
DPR is used only for raster alignment. Width and height are independent:

| Profile | Condition | Behavior |
| --- | --- | --- |
| Compact | Width <1100 or height <700 | Secondary panels overlay the waveform |
| Standard | Remaining widths <1600 | Reference 1280 x 800 layout; overlay panels |
| Wide | Width >=1600 and height >=700 | Bounded 232px side panels may dock |

Performance panels dock only when two panels, their handles and at least 900
scaled logical pixels of waveform fit. A closed panel releases its space. Low
ultrawide displays retain compact vertical geometry. Resizing never persists a
temporary profile or changes the user's panel-open preference.

The existing native waveform pixel split and desktop library height allocation
remain in place. Primary navigation exposes Live/Performance, Library and
Settings directly; Source remains reachable from the source status. Quick access
is supplemental. Development controls default to hidden for fresh profiles;
explicit saved preferences are restored.

## Verification and captures

Build the application and UI tests, then run:

```sh
ctest --test-dir build -R '^(qml_component|ui_layout|ui_scale|waveform_zoom|waveform_render_stability|startup_close_smoke|startup_early_close_smoke)$' --output-on-failure
BROCKDJ_UI_CAPTURE_DIR=/tmp/brockdj-ui-review ctest --test-dir build -R '^qml_component$' --output-on-failure
```

The optional capture directory contains deck information, sources and performance
panels at 1024x600, 1280x800, 1920x720 and 2560x1080. These fixtures use rendering
stubs for waveforms: they verify UI composition, not real waveform rendering or
hardware timing. Captures are generated artifacts, not cross-platform pixel
baselines. The runtime tests cover display-profile boundaries, text contrast,
readability floors, live resize, panel docking, existing cue safety and FX actions.

Before hardware release, inspect real two/four-deck waveforms, library, settings,
errors and dialogs on the target display at 80/100/140% scale and DPR
1/1.25/1.5/2. Check touch accuracy, long titles, USB removal, external input,
focus restoration and controller navigation. Compare identical loaded playback
scenarios with the prior release: p95 render time must not worsen by more than
5% and additional audio dropouts block acceptance. This hardware/latency check
cannot be established by software-rendered component captures.
