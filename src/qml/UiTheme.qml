pragma Singleton
import QtQuick

QtObject {
    // ── Flat panel surfaces (no faux-3D) ───────────────────────────────────
    readonly property color panel:         "#181818"
    readonly property color panelDeep:     "#141414"
    readonly property color panelInset:    "#161616"
    readonly property color panelRaised:   "#1c1c1c"

    // Legacy aliases — keep call sites working with the flat palette
    readonly property color bgDeep:        panelDeep
    readonly property color bg0:           panelDeep
    readonly property color bg1:           panel
    readonly property color bg2:           panelRaised
    readonly property color bg3:           panelRaised
    readonly property color bg4:           "#222222"
    readonly property color bg5:           "#282828"
    readonly property color bgDisplay:     "#121214"

    // ── Separators (soft grey, never pitch-black) ─────────────────────────
    readonly property color separator:       "#2a2a2a"
    readonly property color separatorSubtle: "#222222"
    readonly property color divider:         separatorSubtle
    readonly property color dividerStrong:   separator

    // Legacy bezel tokens → flat separators (highlights/shadows disabled)
    readonly property color bezelOuter:      separatorSubtle
    readonly property color bezelInner:      separatorSubtle
    readonly property color bezelHighlight:  separatorSubtle
    readonly property color bezelShadow:     separatorSubtle

    // ── Controls ──────────────────────────────────────────────────────────
    readonly property color border:        "#303030"
    readonly property color borderHover:   "#454545"
    readonly property color borderActive:  "#555555"

    // ── Text ──────────────────────────────────────────────────────────────
    readonly property color textPrimary:   "#ececec"
    readonly property color textSecondary: "#9a9a9a"
    readonly property color textLabel:     "#5c5c5c"
    readonly property color textDim:       "#484848"
    readonly property color textMuted:     "#383838"

    // ── Functional accents ────────────────────────────────────────────────
    readonly property color green:       "#3acc3a"
    readonly property color greenBright:  "#5dffa0"
    readonly property color greenDim:    "#1a241a"
    readonly property color greenGlow:   "#243828"
    readonly property color blue:        "#5bb6ff"
    readonly property color blueDim:     "#141820"
    readonly property color masterBlue:  "#0080c8"
    readonly property color orange:      "#ffaa00"
    readonly property color orangeDim:   "#241808"
    readonly property color red:         "#e03535"
    readonly property color playhead:    "#f0f0f0"

    // ── Deck identity ─────────────────────────────────────────────────────
    readonly property color deckA:  "#ff8c00"
    readonly property color deckB:  "#00b8e6"
    readonly property color deckC:  "#b855ff"
    readonly property color deckD:  "#3de8a8"

    // ── Knob / fader ──────────────────────────────────────────────────────
    readonly property color knobTrack:   "#1e1e1e"
    readonly property color knobFace:    "#181818"
    readonly property color knobHandle:  "#c8c8c8"
    readonly property color faderTrack:  "#161616"
    readonly property color faderFill:   "#404040"
    readonly property color faderCap:    "#c8c8c8"
    readonly property real  knobArcW:    0.08

    // ── VU meters (shared across mixer + header for a consistent look) ──────
    readonly property color vuLow:   "#2f9e44"   // quiet — deep green
    readonly property color vuMid:   "#52c463"   // nominal — bright green
    readonly property color vuHigh:  "#e6a019"   // hot — amber
    readonly property color vuClip:  red          // clipping — red
    readonly property color vuPeak:  "#ffffff"   // peak-hold marker
    readonly property color vuOff:   knobTrack    // unlit segment

    // ── Performance pads ──────────────────────────────────────────────────
    readonly property color padEmpty:    "#161616"
    readonly property color padBorder:   separatorSubtle
    readonly property color padBorderHi: separator

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
        if (hovered)  return bg3
        return panelRaised
    }
}
