import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

Window {
    id: window
    width: 1280
    height: 800
    visible: true
    title: "Multiplatform DJ Software (Qt 6 + JUCE)"
    color: "#1e1e19"

    // ── Global font scaling ──────────────────────────────────────────────────
    // Dampened scaling: fonts grow/shrink at ~half the rate of the window.
    // At 800px (ref) → factor 1.0, at 400px → 0.85, at 1600px → 1.19.
    // This keeps text readable and proportional to fixed-size buttons.
    readonly property real _refHeight: 800
    function sp(basePx) {
        var ratio = window.height / _refHeight
        // Square-root dampening: sqrt(ratio) changes much slower than ratio
        var dampened = Math.sqrt(ratio)
        var result = Math.round(basePx * dampened)
        // Clamp: never smaller than 70% or larger than 140% of the design size
        return Math.max(Math.round(basePx * 0.7), Math.min(result, Math.round(basePx * 1.4)))
    }

    // Globaler Waveform-Zoom (beide Decks synchron, wie in Serato/Rekordbox)
    property real waveformZoom: 3.0
    readonly property real zoomMin: 0.8
    readonly property real zoomMax: 12.0
    readonly property real zoomFactor: 1.3

    // Ctrl+ = Reinzoomen (mehr Detail, weniger Sekunden sichtbar)
    Shortcut {
        sequence: "Ctrl+="
        onActivated: {
            window.waveformZoom = Math.min(window.zoomMax, window.waveformZoom * window.zoomFactor)
        }
    }
    Shortcut {
        sequence: "Ctrl++"
        onActivated: {
            window.waveformZoom = Math.min(window.zoomMax, window.waveformZoom * window.zoomFactor)
        }
    }
    // Ctrl- = Rauszoomen (weniger Detail, mehr Sekunden sichtbar)
    Shortcut {
        sequence: "Ctrl+-"
        onActivated: {
            window.waveformZoom = Math.max(window.zoomMin, window.waveformZoom / window.zoomFactor)
        }
    }

    // -------------------------------------------------------------------------
    // VIEWPORT SCALING
    // Referenzbreite, auf die das gesamte obere UI-Design ausgelegt ist.
    // uiScale passt alles proportional an, wenn das Fenster schmaler/breiter wird.
    // -------------------------------------------------------------------------
    readonly property real baseUiWidth: 1600
    readonly property real uiScale: width / baseUiWidth

    // Referenz height of the top section at baseUiWidth (waveforms + decks + mixer).
    // Waveforms: 150 px  |  Decks: baseUiWidth / 3.8 ≈ 421 px  |  spacing: 4 px
    readonly property real baseUiHeight: 150 + (baseUiWidth / 3.8) + 4

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --------------------------------------------------------------------
        // GLOBAL HEADER (Traktor-Style)
        // --------------------------------------------------------------------
        TopHeader {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            Layout.maximumHeight: 40
            z: 10
        }

        // Viewport wrapper: reserves the scaled height in the ColumnLayout.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: window.baseUiHeight * window.uiScale
            Layout.maximumHeight: window.baseUiHeight * window.uiScale
            Layout.topMargin: 2

            // Fixed-size design canvas; scaled down/up to match the window width.
            Item {
                id: uiViewport
                width:  window.baseUiWidth
                height: window.baseUiHeight
                scale:  window.uiScale
                transformOrigin: Item.TopLeft

                // ----------------------------------------------------------------
                // OBERER BEREICH: SCROLLING WAVEFORMS
                // ----------------------------------------------------------------
                ColumnLayout {
                    id: waveformSection
                    anchors.top:   parent.top
                    anchors.left:  parent.left
                    anchors.right: parent.right
                    height: 150
                    spacing: 1

                    EnlargedWaveform {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckA
                        backgroundColor: "#222"
                        waveformZoom: window.waveformZoom
                    }

                    EnlargedWaveform {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckB
                        backgroundColor: "#252525"
                        waveformZoom: window.waveformZoom
                    }
                }

                // ----------------------------------------------------------------
                // MITTLERER BEREICH: DECK A + MIXER + DECK B
                // ----------------------------------------------------------------
                RowLayout {
                    id: deckRow
                    anchors.top:    waveformSection.bottom
                    anchors.bottom: parent.bottom
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    anchors.topMargin: 2
                    spacing: 2

                    DeckControl {
                        deckName: "A"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckA
                    }

                    // MIXER SECTION
                    MixerSection {
                        Layout.preferredWidth: 180
                        Layout.fillHeight: true
                        engineA: deckA
                        engineB: deckB
                    }

                    DeckControl {
                        deckName: "B"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckB
                    }
                }
            }
        }

        // --------------------------------------------------------------------
        // UNTERER BEREICH: TRACK LIBRARY
        // fillHeight: true → schluckt jeden vertikalen Restplatz.
        // --------------------------------------------------------------------
        Library {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}