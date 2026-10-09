pragma Singleton
import QtQuick

QtObject {
    readonly property string uiFontFamily: "Inter"
    readonly property string numericFontFamily: "Inter"
    readonly property bool numericPreferTypoMetrics: true
    readonly property real scale: (typeof uiScaleController !== "undefined" && uiScaleController)
                                  ? uiScaleController.scale : 1.0

    function px(value) { return Math.round(value * scale) }

    function applyNumericFontMetrics(textItem) {
        // This optional font property was introduced in Qt 6.8.
        if ("preferTypoLineMetrics" in textItem.font)
            textItem.font.preferTypoLineMetrics = Qt.binding(function() {
                return UiTheme.numericPreferTypoMetrics
            })
    }

    readonly property int space1: px(2)
    readonly property int space2: px(4)
    readonly property int space3: px(6)
    readonly property int space4: px(8)
    readonly property int space5: px(12)
    readonly property int space6: px(16)

    readonly property int fontMicro: px(9)
    readonly property int fontLabel: px(10)
    readonly property int fontBody: px(12)
    readonly property int fontValue: px(14)
    readonly property int fontDeckTitle: px(15)
    readonly property int fontBpm: px(21)
    readonly property int fontTime: px(17)

    readonly property int controlHeightSmall: px(22)
    readonly property int controlHeightNormal: px(30)
    readonly property int toolbarHeight: px(48)
    readonly property int toolbarPullExtra: px(76)
    readonly property int deckHeaderHeight: px(42)
    readonly property int transportStripHeight: px(62)
    readonly property int waveformMinimumHeight: px(120)
    readonly property int mixerMinimumWidth: px(260)
    readonly property int mixerPreferredWidth: px(308)
    readonly property int dividerWidth: Math.max(1, px(1))
    readonly property int libraryRowHeightCompact: px(24)
    readonly property int libraryRowHeightNormal: px(30)
    readonly property int performancePadSize: px(34)

    // Canonical design-token names. Existing aliases below remain during the
    // component migration; new UI code uses these semantic names.
    readonly property color surface:           "#e6212730"
    readonly property color surfaceRaised:     "#e62c3440"
    readonly property color surfaceInset:      "#151b24"
    readonly property color displayBackground: "#10151d"
    readonly property color borderSubtle:      "#28313e"
    readonly property color borderStrong:      "#465366"
    readonly property color warning:           "#e6a019"
    readonly property color error:             "#e03535"
    readonly property color play:              "#43d17b"
    readonly property color cue:               "#f2b134"
    readonly property color sync:              "#56a8ff"

    // Tinted translucency, without refraction, gloss or faux-3D.
    readonly property color panel:         surface
    readonly property color panelDeep:     surfaceInset
    readonly property color panelInset:    "#bd1a212b"
    readonly property color panelRaised:   surfaceRaised

    // Legacy aliases — keep call sites working with the flat palette
    readonly property color bgDeep:        panelDeep
    readonly property color bg0:           panelDeep
    readonly property color bg1:           panel
    readonly property color bg2:           panelRaised
    readonly property color bg3:           panelRaised
    readonly property color bg4:           "#384657"
    readonly property color bg5:           "#46566b"
    readonly property color bgDisplay:     displayBackground

    // ── Separators (soft grey, never pitch-black) ─────────────────────────
    readonly property color separator:       "#364253"
    readonly property color separatorSubtle: borderSubtle
    readonly property color divider:         separatorSubtle
    readonly property color dividerStrong:   separator

    // Legacy bezel tokens → flat separators (highlights/shadows disabled)
    readonly property color bezelOuter:      separatorSubtle
    readonly property color bezelInner:      separatorSubtle
    readonly property color bezelHighlight:  separatorSubtle
    readonly property color bezelShadow:     separatorSubtle

    // ── Controls ──────────────────────────────────────────────────────────
    readonly property color border:        "#3b4758"
    readonly property color borderHover:   "#61738b"
    readonly property color borderActive:  "#83a4ce"

    // ── Text ──────────────────────────────────────────────────────────────
    readonly property color textPrimary:   "#eef2f8"
    readonly property color textSecondary: "#b0bdce"
    readonly property color textLabel:     "#8e9db1"
    readonly property color textDim:       "#718096"
    readonly property color textMuted:     "#58667a"

    // ── Functional accents ────────────────────────────────────────────────
    readonly property color green:       "#62cf9e"
    readonly property color greenBright:  "#5dffa0"
    readonly property color greenDim:    "#243c36"
    readonly property color greenGlow:   "#2c5144"
    readonly property color blue:        "#89b8f5"
    readonly property color blueDim:     "#26364e"
    readonly property color masterBlue:  blue
    readonly property color orange:      "#e9b16c"
    readonly property color orangeDim:   "#3b3229"
    readonly property color red:         "#e03535"
    readonly property color playhead:    "#ffffff"

    // ── Deck identity ─────────────────────────────────────────────────────
    readonly property color deckA:  "#e9b16c"
    readonly property color deckB:  "#89b8f5"
    readonly property color deckC:  "#be9bea"
    readonly property color deckD:  "#62cfb6"

    // ── Knob / fader ──────────────────────────────────────────────────────
    readonly property color knobTrack:   "#303c4c"
    readonly property color knobFace:    "#202935"
    readonly property color knobHandle:  "#d3deec"
    readonly property color faderTrack:  "#101720"
    readonly property color faderFill:   "#7289a6"
    readonly property color faderCap:    "#d3deec"
    readonly property real  knobArcW:    0.08

    // ── VU meters (shared across mixer + header for a consistent look) ──────
    readonly property color vuLow:   "#2f9e44"   // quiet — deep green
    readonly property color vuMid:   "#52c463"   // nominal — bright green
    readonly property color vuHigh:  "#e6a019"   // hot — amber
    readonly property color vuClip:  red          // clipping — red
    readonly property color vuPeak:  "#ffffff"   // peak-hold marker
    readonly property color vuOff:   knobTrack    // unlit segment

    // ── Performance pads ──────────────────────────────────────────────────
    readonly property color padEmpty:    panelInset
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
        case "C": return "#332d43"
        case "D": return "#243c38"
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
