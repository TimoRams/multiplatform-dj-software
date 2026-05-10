pragma Singleton
import QtQuick

QtObject {
    // ── Surfaces ──────────────────────────────────────────────────────────
    readonly property color bg0:  "#0a0a0a"   // deepest: channel headers, label rows
    readonly property color bg1:  "#111111"   // main deck/panel background
    readonly property color bg2:  "#181818"   // control rows, inner panels
    readonly property color bg3:  "#1e1e1e"   // buttons, knobs, sliders at rest
    readonly property color bg4:  "#252525"   // hover state
    readonly property color bg5:  "#2d2d2d"   // pressed / active background

    // ── Dividers & borders ─────────────────────────────────────────────────
    readonly property color divider:      "#1c1c1c"  // thin section separators
    readonly property color border:       "#333333"  // control border at rest
    readonly property color borderHover:  "#555555"  // control border on hover
    readonly property color borderActive: "#666666"  // focused / active border

    // ── Text ──────────────────────────────────────────────────────────────
    readonly property color textPrimary:   "#e8e8e8"  // main content text
    readonly property color textSecondary: "#999999"  // artist, secondary info
    readonly property color textDim:       "#666666"  // labels, dim text
    readonly property color textMuted:     "#444444"  // disabled / very dim

    // ── Accent – functional ────────────────────────────────────────────────
    readonly property color green:      "#4dd98a"  // play active, loop, cue active
    readonly property color greenDim:   "#0d2018"  // green tinted background
    readonly property color blue:       "#5bb6ff"  // key, live BPM
    readonly property color blueDim:    "#0a1a2e"  // blue tinted background
    readonly property color orange:     "#ffaa00"  // positive pitch, warm highlight

    // ── Accent – deck identity ─────────────────────────────────────────────
    readonly property color deckA:  "#ff9900"  // deck A orange
    readonly property color deckB:  "#00ccff"  // deck B cyan
    readonly property color deckC:  "#cc44ff"  // deck C purple
    readonly property color deckD:  "#44ddaa"  // deck D teal

    // ── Knob ──────────────────────────────────────────────────────────────
    readonly property color knobTrack:  "#252525"  // dim background arc
    readonly property color knobHandle: "#c8c8c8"  // pointer line
    readonly property real  knobArcW:   0.08       // arc line-width as fraction of knob diameter
}
