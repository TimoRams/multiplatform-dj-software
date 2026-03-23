import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    visible: true
    title: "RamsbrockDJ"
    color: "#0a0a0a"

    // Timer to hide the loading indicator and show the main content
    Timer {
        id: loadingTimer
        interval: 2000 // 2 seconds
        running: true
        repeat: false
        onTriggered: {
            loadingIndicator.running = false
            loadingIndicator.visible = false
            mainLayout.visible = true
        }
    }

    BusyIndicator {
        id: loadingIndicator
        anchors.centerIn: parent
        running: true
        visible: true
    }

    // ── Global font scaling ──────────────────────────────────────────────────
    // Small font sizes get an additional readability boost because many labels
    // in the UI use 7-10px design sizes.
    readonly property real _refHeight: 800
    property real fontScaleBoost: 1.18
    function sp(basePx) {
        var ratio = window.height / _refHeight
        var dampened = Math.sqrt(ratio)

        // Keep micro-labels readable on high-res displays.
        var smallTextBoost = 1.0
        if (basePx <= 8)
            smallTextBoost = 1.30
        else if (basePx <= 10)
            smallTextBoost = 1.16
        else if (basePx <= 12)
            smallTextBoost = 1.08

        var result = Math.round(basePx * dampened * fontScaleBoost * smallTextBoost)

        var minReadable = basePx <= 8 ? 10
                        : basePx <= 10 ? 11
                        : Math.round(basePx * 0.9)
        var minScaleClamp = Math.round(basePx * 0.9)
        var maxScaleClamp = Math.round(basePx * 1.9)

        return Math.max(Math.max(minReadable, minScaleClamp), Math.min(result, maxScaleClamp))
    }

    // Globaler Waveform-Zoom (beide Decks synchron, wie in Serato/Rekordbox)
    // Zoom wird als diskreter Schrittzähler gespeichert, damit Reinzoomen und
    // Rauszoomen sich exakt aufheben (kein Float-Rundungsfehler beim Klemmen).
    readonly property real zoomBase:   1.5    // pixelsPerPoint bei step=0 (~8.5s @ 1920px)
    readonly property real zoomFactor: 1.3
    readonly property int  zoomStepMin: -5    // max. rauszoomen  → 1.5/1.3^5 ≈ 0.38 ppp
    readonly property int  zoomStepMax:  7    // max. reinzoomen  → 1.5*1.3^7 ≈ 10.1 ppp
    property int  waveformZoomStep: 0
    readonly property real waveformZoom: zoomBase * Math.pow(zoomFactor, waveformZoomStep)

    // Ctrl+ = Reinzoomen (mehr Detail, weniger Sekunden sichtbar)
    Shortcut {
        sequence: "Ctrl+="
        onActivated: {
            window.waveformZoomStep = Math.min(window.zoomStepMax, window.waveformZoomStep + 1)
        }
    }
    Shortcut {
        sequence: "Ctrl++"
        onActivated: {
            window.waveformZoomStep = Math.min(window.zoomStepMax, window.waveformZoomStep + 1)
        }
    }
    // Ctrl- = Rauszoomen (weniger Detail, mehr Sekunden sichtbar)
    Shortcut {
        sequence: "Ctrl+-"
        onActivated: {
            window.waveformZoomStep = Math.max(window.zoomStepMin, window.waveformZoomStep - 1)
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
        id: mainLayout
        anchors.fill: parent
        spacing: 2
        visible: false

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
                    spacing: 0

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
        // FX RACK  –  horizontale Effekt-Leiste in der Mitte des Bildschirms
        // (zwischen Decks/Mixer-Sektion und Library)
        // --------------------------------------------------------------------
        FxBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            Layout.maximumHeight:   40
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
