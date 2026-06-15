pragma Singleton
import QtQuick

QtObject {
    // ── Surfaces (Pioneer CDJ matte chassis) ───────────────────────────────
    readonly property color bgDeep:     "#040404"
    readonly property color bg0:        "#060606"
    readonly property color bg1:        "#0a0a0a"
    readonly property color bg2:        "#101010"
    readonly property color bg3:        "#161616"
    readonly property color bg4:        "#1c1c1c"
    readonly property color bg5:        "#242424"
    readonly property color bgDisplay:  "#050508"

    // ── Bezels & hardware edges ────────────────────────────────────────────
    readonly property color bezelOuter:   "#2e2e2e"
    readonly property color bezelInner:   "#080808"
    readonly property color bezelHighlight: "#3a3a3a"
    readonly property color bezelShadow:  "#020202"
    readonly property color panelInset:   "#0d0d0d"

    // ── Dividers & borders ─────────────────────────────────────────────────
    readonly property color divider:       "#181818"
    readonly property color dividerStrong: "#222222"
    readonly property color border:        "#2a2a2a"
    readonly property color borderHover:   "#444444"
    readonly property color borderActive:  "#555555"

    // ── Text (silk-screen + LCD readouts) ──────────────────────────────────
    readonly property color textPrimary:   "#ececec"
    readonly property color textSecondary: "#9a9a9a"
    readonly property color textLabel:     "#5c5c5c"
    readonly property color textDim:       "#484848"
    readonly property color textMuted:     "#333333"

    // ── Functional accents ─────────────────────────────────────────────────
    readonly property color green:       "#3de87a"
    readonly property color greenBright:  "#5dffa0"
    readonly property color greenDim:    "#0a1a10"
    readonly property color greenGlow:   "#1a4028"
    readonly property color blue:        "#5bb6ff"
    readonly property color blueDim:     "#0a1420"
    readonly property color masterBlue:  "#0080c8"
    readonly property color orange:      "#ffaa00"
    readonly property color orangeDim:   "#2a1800"
    readonly property color red:         "#e03535"
    readonly property color playhead:    "#f0f0f0"

    // ── Deck identity ──────────────────────────────────────────────────────
    readonly property color deckA:  "#ff8c00"
    readonly property color deckB:  "#00b8e6"
    readonly property color deckC:  "#b855ff"
    readonly property color deckD:  "#3de8a8"

    // ── Knob / fader hardware ──────────────────────────────────────────────
    readonly property color knobTrack:   "#1a1a1a"
    readonly property color knobFace:    "#141414"
    readonly property color knobHandle:  "#d0d0d0"
    readonly property color faderTrack:  "#0a0a0a"
    readonly property color faderFill:   "#3a3a3a"
    readonly property color faderCap:    "#c8c8c8"
    readonly property real  knobArcW:    0.08

    // ── Performance pads ───────────────────────────────────────────────────
    readonly property color padEmpty:    "#121212"
    readonly property color padBorder:   "#1e1e1e"
    readonly property color padBorderHi: "#333333"

    function deckColor(name) {
        switch (name) {
        case "A": return deckA
        case "B": return deckB
        case "C": return deckC
        case "D": return deckD
        default:  return deckA
        }
    }

    function deckTint(name) {
        switch (name) {
        case "A": return orangeDim
        case "B": return blueDim
        case "C": return "#1a0a28"
        case "D": return "#0a2018"
        default:  return orangeDim
        }
    }

    function buttonBg(active, hovered, pressed) {
        if (pressed)  return bg5
        if (active)   return bg4
        if (hovered)  return bg4
        return bg3
    }
}
