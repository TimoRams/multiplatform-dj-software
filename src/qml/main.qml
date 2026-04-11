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
    property bool libraryExpanded: false
    readonly property real baseWaveformHeight: 150
    readonly property real baseDeckMixerHeight: baseUiHeight - baseWaveformHeight

    function _isTextInputFocused() {
        var focused = activeFocusItem
        if (!focused)
            return false
        return (typeof focused.echoMode !== "undefined")
            || (typeof focused.inputMask === "string")
            || (typeof focused.cursorPosition === "number")
            || (typeof focused.inputMethodComposing === "boolean")
    }

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

    // ── Global font scaling (responsive / non-transformed areas only) ───────
    readonly property real _refHeight: 800
    readonly property real responsiveFontScale: Math.max(0.92, Math.min(1.30, window.height / _refHeight))

    function _snapScaleToPhysicalPixels(rawScale) {
        var dpr = Math.max(1.0, window.devicePixelRatio)
        // Keep scaled design width aligned to whole physical pixels.
        var scaledPhysicalWidth = Math.round(window.baseUiWidth * rawScale * dpr)
        return scaledPhysicalWidth / (window.baseUiWidth * dpr)
    }

    function _scaledFontSize(basePx) {
        var scale = responsiveFontScale
        var scaled = basePx * scale

        // Keep tiny labels legible while avoiding aggressive jumps.
        if (basePx <= 10)
            scaled *= 1.08

        var dpr = Math.max(1.0, window.devicePixelRatio)
        var snapped = Math.round(scaled * dpr) / dpr
        return Math.max(1, snapped)
    }

    // Standard scaling for non-transformed areas (header, FX bar, library).
    function sp(basePx) {
        return Math.round(_scaledFontSize(basePx))
    }

    // Viewport-aware sizing for the transformed top deck area.
    // Intentionally independent from uiScale/height to prevent text layout
    // jumps while resizing or scaling the viewport.
    function spViewport(basePx) {
        var logicalPx = basePx

        // Keep tiny labels readable without dynamic relayout.
        if (basePx <= 8)
            logicalPx = basePx + 2
        else if (basePx <= 10)
            logicalPx = basePx + 1

        var dpr = Math.max(1.0, window.devicePixelRatio)
        var snapped = Math.round(logicalPx * dpr) / dpr
        return Math.max(1, Math.round(snapped))
    }

    // Globaler Waveform-Zoom (beide Decks synchron, wie in professioneller DJ-Software)
    // Zusätzliche Rauszoom-Stufen am Anfang; 0.22 (vorher weitester Rauszoom) bleibt als neuer Default.
    readonly property var waveformZoomLevels: [0.10, 0.14, 0.18, 0.22, 0.29, 0.38, 0.52, 0.70, 0.95, 1.30, 1.80, 2.50, 3.50, 5.00, 7.20]
    readonly property int  zoomStepMin: 0
    readonly property int  zoomStepMax: waveformZoomLevels.length - 1
    // Default ist jetzt der bisher weiteste Rauszoom.
    property int  waveformZoomStep: waveformZoomLevels.indexOf(0.22)
    readonly property real waveformZoom: waveformZoomLevels[waveformZoomStep]

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

    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (window._isTextInputFocused())
                return
            window.libraryExpanded = !window.libraryExpanded
        }
    }

    // -------------------------------------------------------------------------
    // VIEWPORT SCALING
    // Referenzbreite, auf die das gesamte obere UI-Design ausgelegt ist.
    // uiScale passt alles proportional an, wenn das Fenster schmaler/breiter wird.
    // -------------------------------------------------------------------------
    readonly property real baseUiWidth: 1600
    readonly property real rawUiScale: width / baseUiWidth
    readonly property real uiScale: _snapScaleToPhysicalPixels(rawUiScale)
    readonly property int scaledWaveformHeight: Math.round(window.baseWaveformHeight * window.uiScale)
    readonly property int scaledDeckMixerHeight: Math.round(window.baseDeckMixerHeight * window.uiScale)
    // Keep header height fixed to prevent resize jitter and control shifts.
    readonly property int topBarHeight: 34
    readonly property int fxBarHeight: Math.max(36, Math.round(40 * (window.height / 800)))

    // Referenz height of the top section at baseUiWidth (waveforms + decks + mixer).
    // The deck/mixer block is intentionally kept about 25% shorter so the library
    // can use more vertical space.
    readonly property real baseUiHeight: 150 + (baseUiWidth / 5.0) + 4

    ColumnLayout {
        id: mainLayout
        anchors.fill: parent
        spacing: 0
        visible: false

        // --------------------------------------------------------------------
        // GLOBAL HEADER (Traktor-Style)
        // --------------------------------------------------------------------
        TopHeader {
            id: topHeader
            Layout.fillWidth: true
            Layout.minimumHeight: window.topBarHeight
            Layout.preferredHeight: window.topBarHeight
            Layout.maximumHeight: window.topBarHeight
            z: 10
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.minimumHeight: 1
            Layout.preferredHeight: 1
            Layout.maximumHeight: 1
            color: "#7a7a7a"
        }

        // Viewport wrapper: reserves the scaled height in the ColumnLayout.
        Item {
            id: waveformViewport
            Layout.fillWidth: true
            Layout.minimumHeight: window.scaledWaveformHeight
            Layout.preferredHeight: window.scaledWaveformHeight
            Layout.maximumHeight: window.scaledWaveformHeight
            clip: true

            // Fixed-size design canvas; scaled down/up to match the window width.
            Item {
                id: waveformCanvas
                width:  window.baseUiWidth
                height: window.baseWaveformHeight
                scale:  window.uiScale
                transformOrigin: Item.TopLeft
                x: Math.round((waveformViewport.width - (width * scale)) * 0.5)
                y: 0

                ColumnLayout {
                    id: waveformSection
                    anchors.fill: parent
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
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.minimumHeight: 1
            Layout.preferredHeight: 1
            Layout.maximumHeight: 1
            color: "#7a7a7a"
        }

        Item {
            id: deckMixerViewport
            Layout.fillWidth: true
            Layout.minimumHeight: window.libraryExpanded ? 0 : window.scaledDeckMixerHeight
            Layout.preferredHeight: window.libraryExpanded ? 0 : window.scaledDeckMixerHeight
            Layout.maximumHeight: window.libraryExpanded ? 0 : window.scaledDeckMixerHeight
            visible: !window.libraryExpanded
            clip: true

            Item {
                id: deckMixerCanvas
                width:  window.baseUiWidth
                height: window.baseDeckMixerHeight
                scale:  window.uiScale
                transformOrigin: Item.TopLeft
                x: Math.round((deckMixerViewport.width - (width * scale)) * 0.5)
                y: 0

                RowLayout {
                    id: deckRow
                    anchors.fill: parent
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    spacing: 0

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
            id: fxBarSection
            Layout.fillWidth: true
            Layout.minimumHeight: window.libraryExpanded ? 0 : window.fxBarHeight
            Layout.preferredHeight: window.libraryExpanded ? 0 : window.fxBarHeight
            Layout.maximumHeight: window.libraryExpanded ? 0 : window.fxBarHeight
            visible: !window.libraryExpanded
        }

        // --------------------------------------------------------------------
        // UNTERER BEREICH: TRACK LIBRARY
        // fillHeight: true → schluckt jeden vertikalen Restplatz.
        // --------------------------------------------------------------------
        Library {
            id: librarySection
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
