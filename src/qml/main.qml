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
    color: "#2a2a2a"
    font.hintingPreference: Font.PreferFullHinting
    property bool libraryExpanded: false
    property string linkedDeckName: ""
    property bool exitPromptVisible: false
    property bool exitShutdownInProgress: false
    property bool allowDirectClose: false
    property bool exitCleanupTriggered: false
    property real exitProgress: 0.0
    readonly property color unifiedGray: "#2a2a2a"
        function requestAppClose() {
            if (exitShutdownInProgress)
                return
            exitPromptVisible = true
        }

        function cancelAppClosePrompt() {
            if (exitShutdownInProgress)
                return
            exitPromptVisible = false
        }

        function confirmAppClose() {
            if (exitShutdownInProgress)
                return

            exitShutdownInProgress = true
            exitCleanupTriggered = false
            exitProgress = 0.0
            exitShutdownTimer.restart()
        }

        function finalizeAppClose() {
            allowDirectClose = true
            Qt.quit()
        }

        onClosing: function(close) {
            if (allowDirectClose) {
                close.accepted = true
                return
            }

            close.accepted = false
            requestAppClose()
        }

        Timer {
            id: exitShutdownTimer
            interval: 70
            repeat: true
            running: false
            onTriggered: {
                if (!window.exitShutdownInProgress) {
                    stop()
                    return
                }

                window.exitProgress = Math.min(1.0, window.exitProgress + 0.09)

                if (!window.exitCleanupTriggered && window.exitProgress >= 0.25) {
                    window.exitCleanupTriggered = true
                    if (typeof settingsManager !== "undefined" && settingsManager && settingsManager.flushToDisk)
                        settingsManager.flushToDisk()
                    if (typeof libraryDb !== "undefined" && libraryDb && libraryDb.shutdown)
                        libraryDb.shutdown()
                }

                if (window.exitProgress >= 1.0) {
                    stop()
                    window.finalizeAppClose()
                }
            }
        }

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

    // ── Global font sizing (non-transformed areas stay stable on resize) ─────
    readonly property real _refHeight: 800
    readonly property real responsiveFontScale: 1.0

    function _snapScaleToPhysicalPixels(rawScale) {
        var dpr = Math.max(1.0, window.devicePixelRatio)
        // Keep scaled design width aligned to whole physical pixels.
        var scaledPhysicalWidth = Math.round(window.baseUiWidth * rawScale * dpr)
        return scaledPhysicalWidth / (window.baseUiWidth * dpr)
    }

    function _scaledFontSize(basePx) {
        var scale = responsiveFontScale
        var scaled = basePx * scale

        // Keep tiny labels readable in dense UI areas.
        if (basePx <= 8)
            scaled *= 1.24
        else if (basePx <= 10)
            scaled *= 1.18
        else if (basePx <= 12)
            scaled *= 1.10

        var dpr = Math.max(1.0, window.devicePixelRatio)
        var snapped = Math.round(scaled * dpr) / dpr
        return Math.max(1, snapped)
    }

    // Standard scaling for non-transformed areas (header, FX bar, library).
    function sp(basePx) {
        return Math.round(_scaledFontSize(basePx))
    }

    // Viewport-aware sizing for the top deck area.
    // Keep it independent from width scaling to avoid text distortion.
    function spViewport(basePx) {
        var logicalPx = _scaledFontSize(basePx)

        // Keep tiny labels readable without large layout jumps.
        if (basePx <= 6)
            logicalPx += 1.4
        else if (basePx <= 8)
            logicalPx += 1.0
        else if (basePx <= 10)
            logicalPx += 0.6

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
            if (window.exitPromptVisible)
                return
            window.libraryExpanded = !window.libraryExpanded
        }
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (window._isTextInputFocused())
                return

            if (window.exitPromptVisible) {
                window.cancelAppClosePrompt()
                return
            }

            window.requestAppClose()
        }
    }

    Connections {
        target: (typeof linkManager !== "undefined" && linkManager !== null) ? linkManager : null
        function onEnabledChanged() {
            if (!linkManager.enabled)
                window.linkedDeckName = ""
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
    readonly property int fxBarHeight: 40

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
            color: "#000000"
        }

        // Viewport wrapper: reserves the scaled height in the ColumnLayout.
        Item {
            id: waveformViewport
            Layout.fillWidth: true
            Layout.minimumHeight: window.scaledWaveformHeight
            Layout.preferredHeight: window.scaledWaveformHeight
            Layout.maximumHeight: window.scaledWaveformHeight
            clip: true

            // Use direct viewport sizing instead of transform-scaling to keep text crisp.
            Item {
                id: waveformCanvas
                anchors.fill: parent

                ColumnLayout {
                    id: waveformSection
                    anchors.fill: parent
                    spacing: 0

                    EnlargedWaveform {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckA
                        backgroundColor: "#2a2a2a"
                        waveformZoom: window.waveformZoom
                    }

                    EnlargedWaveform {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckB
                        backgroundColor: "#2a2a2a"
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
            color: "#000000"
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
                anchors.fill: parent

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

    Rectangle {
        id: exitOverlay
        anchors.fill: parent
        z: 1000
        visible: window.exitPromptVisible
        color: window.unifiedGray
        focus: visible

        MouseArea {
            anchors.fill: parent
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width * 0.82, 460)
            spacing: 14

            Text {
                width: parent.width
                text: window.exitShutdownInProgress
                      ? "Programm wird beendet..."
                      : "Moechtest du das Programm wirklich beenden?"
                color: "#e5e5e5"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: window.sp(20)
                font.bold: true
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                visible: !window.exitShutdownInProgress

                Rectangle {
                    width: 118
                    height: 38
                    radius: 0
                    color: yesArea.pressed ? "#3b3b3b" : "#353535"
                    border.width: 1
                    border.color: "#000000"

                    Text {
                        anchors.centerIn: parent
                        text: "Yes"
                        color: "#f0f0f0"
                        font.pixelSize: window.sp(13)
                        font.bold: true
                    }

                    MouseArea {
                        id: yesArea
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: window.confirmAppClose()
                    }
                }

                Rectangle {
                    width: 118
                    height: 38
                    radius: 0
                    color: noArea.pressed ? "#3b3b3b" : "#353535"
                    border.width: 1
                    border.color: "#000000"

                    Text {
                        anchors.centerIn: parent
                        text: "No"
                        color: "#f0f0f0"
                        font.pixelSize: window.sp(13)
                        font.bold: true
                    }

                    MouseArea {
                        id: noArea
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: window.cancelAppClosePrompt()
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 6
                color: "#1f1f1f"
                border.width: 1
                border.color: "#000000"
                visible: window.exitShutdownInProgress

                Rectangle {
                    width: parent.width * window.exitProgress
                    height: parent.height
                    color: "#6f6f6f"
                }
            }

            Text {
                width: parent.width
                visible: window.exitShutdownInProgress
                text: "Datenbank und Einstellungen werden gespeichert..."
                color: "#cfcfcf"
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: window.sp(11)
            }
        }
    }
}
