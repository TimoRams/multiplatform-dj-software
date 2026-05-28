import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    minimumWidth: 800
    minimumHeight: 600
    visible: true
    title: "BrockDJ"
    color: "#2a2a2a"
    font.hintingPreference: Font.PreferFullHinting
    property bool libraryExpanded: false
    property string linkedDeckName: ""
    property bool exitPromptVisible: false
    property bool exitShutdownInProgress: false
    property bool exitManualBackupRequested: false
    property bool allowDirectClose: false
    property bool exitCleanupTriggered: false
    property bool uncleanShutdownWarningVisible: false
    property real exitProgress: 0.0
    readonly property color unifiedGray: "#2a2a2a"

    property bool showWaveforms: true
    property bool showDeckA: true
    property bool showDeckB: true
    property bool showMixer: true
    property bool showFxBar: true
    property bool showLibrary: true
    property bool showCrossfader: true
    property bool fourDeckMode: false

    property int resizeThrottleCounter: 0
    property int lastProcessedWidth: width
    property int lastProcessedHeight: height

    onWidthChanged: {
        resizeThrottleCounter++
        if (resizeThrottleCounter >= 5) {
            lastProcessedWidth = width
            resizeThrottleCounter = 0
        }
    }

    onHeightChanged: {
        resizeThrottleCounter++
        if (resizeThrottleCounter >= 5) {
            lastProcessedHeight = height
            resizeThrottleCounter = 0
        }
    }

    function requestAppClose() {
        if (exitShutdownInProgress)
            return
        exitManualBackupRequested = false
        exitPromptVisible = true
    }

    function cancelAppClosePrompt() {
        if (exitShutdownInProgress)
            return
        exitManualBackupRequested = false
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
        exitPromptVisible = false
        window.close()
        Qt.callLater(Qt.quit)
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
                    libraryDb.shutdown(window.exitManualBackupRequested)
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

    function _handleLinkEnabledChanged() {
        if (typeof linkManager === "undefined" || linkManager === null)
            return
        if (!linkManager.enabled)
            window.linkedDeckName = ""
    }

    Component.onCompleted: {
        if (typeof linkManager !== "undefined" && linkManager !== null)
            linkManager.enabledChanged.connect(window._handleLinkEnabledChanged)
    }

    Component.onDestruction: {
        if (typeof linkManager !== "undefined" && linkManager !== null)
            linkManager.enabledChanged.disconnect(window._handleLinkEnabledChanged)
    }

    Timer {
        id: loadingTimer
        interval: 2000
        running: true
        repeat: false
        onTriggered: {
            loadingIndicator.running = false
            loadingIndicator.visible = false
            mainLayout.visible = true
            if (typeof appConfig !== "undefined" && appConfig && !appConfig.firstRunCompleted)
                welcomeOverlay.active = true
            if (typeof settingsManager !== "undefined" && settingsManager && settingsManager.previousRunUnclean) {
                uncleanShutdownWarning.visibleMessage = settingsManager.previousRunWarningMessage
                window.uncleanShutdownWarningVisible = true
            }
        }
    }

    Item {
        id: loadingIndicator
        property bool running: true
        anchors.centerIn: parent
        visible: true
        z: 1000

        width: 260
        height: 120

        Rectangle {
            anchors.fill: parent
            color: window.unifiedGray
        }

        Column {
            anchors.centerIn: parent
            spacing: 12

            Text {
                text: "BROCK DJ"
                color: "#e0e0e0"
                font.pixelSize: window.sp(18)
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                width: 220
            }

            Item {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 150
                height: 44

                Row {
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 6
                    height: 40

                    Repeater {
                        model: 5

                        Rectangle {
                            id: loadBar
                            required property int index
                            width: 16
                            height: 18
                            color: "#7a7a7a"
                            border.width: 1
                            border.color: "#000000"
                            radius: 0
                            anchors.bottom: parent.bottom

                            SequentialAnimation on height {
                                running: loadingIndicator.running
                                loops: Animation.Infinite
                                alwaysRunToEnd: true
                                PauseAnimation { duration: loadBar.index * 80 }
                                NumberAnimation { from: 18; to: 38; duration: 320; easing.type: Easing.InOutQuad }
                                NumberAnimation { from: 38; to: 18; duration: 320; easing.type: Easing.InOutQuad }
                                PauseAnimation { duration: Math.max(0, 240 - loadBar.index * 30) }
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 2
                    color: "#000000"
                }
            }

            Text {
                text: "Loading audio engine..."
                color: "#bcbcbc"
                font.pixelSize: window.sp(11)
                horizontalAlignment: Text.AlignHCenter
                width: 220
            }
        }
    }

    // ── Global font sizing ───────────────────────────────────────────────────
    readonly property real _refHeight: 800
    readonly property real responsiveFontScale: 1.0

    function _dpr() {
        var dpr = Screen.devicePixelRatio
        if (!isFinite(dpr) || dpr <= 0)
            dpr = 1.0
        return dpr
    }

    function _snapScaleToPhysicalPixels(rawScale) {
        if (!isFinite(rawScale) || rawScale <= 0)
            rawScale = 1.0
        var dpr = _dpr()
        var scaledPhysicalWidth = Math.round(window.baseUiWidth * rawScale * dpr)
        if (!isFinite(scaledPhysicalWidth) || scaledPhysicalWidth <= 0)
            return 1.0
        return scaledPhysicalWidth / (window.baseUiWidth * dpr)
    }

    function _scaledFontSize(basePx) {
        var scale = responsiveFontScale
        var scaled = basePx * scale

        if (basePx <= 8)
            scaled *= 1.24
        else if (basePx <= 10)
            scaled *= 1.18
        else if (basePx <= 12)
            scaled *= 1.10

        var dpr = _dpr()
        var snapped = Math.round(scaled * dpr) / dpr
        return Math.max(1, snapped)
    }

    function sp(basePx) {
        return Math.round(_scaledFontSize(basePx))
    }

    function spViewport(basePx) {
        var logicalPx = _scaledFontSize(basePx)

        if (basePx <= 6)
            logicalPx += 1.4
        else if (basePx <= 8)
            logicalPx += 1.0
        else if (basePx <= 10)
            logicalPx += 0.6

        var dpr = _dpr()
        var snapped = Math.round(logicalPx * dpr) / dpr
        return Math.max(1, Math.round(snapped))
    }

    readonly property var waveformZoomLevels: [0.10, 0.14, 0.18, 0.22, 0.29, 0.38, 0.52, 0.70, 0.95, 1.30, 1.80, 2.50, 3.50, 5.00, 7.20]
    readonly property int  zoomStepMin: 0
    readonly property int  zoomStepMax: waveformZoomLevels.length - 1
    property int  waveformZoomStep: waveformZoomLevels.indexOf(0.22)
    readonly property real waveformZoom: waveformZoomLevels[waveformZoomStep]

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
    }

    // -------------------------------------------------------------------------
    // VIEWPORT SCALING
    // -------------------------------------------------------------------------
    readonly property real baseUiWidth: 1600
    readonly property real rawUiScale: width / baseUiWidth
    readonly property real uiScale: _snapScaleToPhysicalPixels(rawUiScale)
    readonly property int scaledWaveformHeight: Math.round(window.baseWaveformHeight * window.uiScale)
    readonly property int scaledDeckMixerHeight: Math.round(window.baseDeckMixerHeight * window.uiScale)
    readonly property int topBarHeight: 34
    readonly property int fxBarHeight: 90
    readonly property int mixerBaseWidth: 280

    readonly property real baseUiHeight: 150 + (baseUiWidth / 6.5) + 4

    // ─────────────────────────────────────────────────────────────────────────
    // MAIN LAYOUT – direct child, no async Loader wrapping
    // ─────────────────────────────────────────────────────────────────────────
    ColumnLayout {
        id: mainLayout
        anchors.fill: parent
        spacing: 0
        visible: false

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

        Item {
            id: waveformViewport
            Layout.fillWidth: true
            Layout.minimumHeight: window.scaledWaveformHeight
            Layout.preferredHeight: window.scaledWaveformHeight
            Layout.maximumHeight: window.scaledWaveformHeight
            visible: window.showWaveforms
            clip: true

            Item {
                id: waveformCanvas
                anchors.fill: parent

                ColumnLayout {
                    id: waveformSection
                    anchors.fill: parent
                    spacing: 0

                    EnlargedWaveform {
                        visible: window.fourDeckMode
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckC
                        backgroundColor: "#2a2a2a"
                        waveformZoom: window.waveformZoom
                    }

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

                    EnlargedWaveform {
                        visible: window.fourDeckMode
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckD
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
            visible: !window.libraryExpanded && (window.showDeckA || window.showDeckB || window.showMixer)
            clip: true

            readonly property real designWidth: window.baseUiWidth
            readonly property real designHeight: window.baseDeckMixerHeight
            readonly property real uniformScaleRaw: Math.min(
                width / Math.max(1, designWidth),
                height / Math.max(1, designHeight)
            )
            readonly property real uniformScale: window._snapScaleToPhysicalPixels(Math.max(0.1, uniformScaleRaw))

            Item {
                id: deckMixerCanvas
                width: deckMixerViewport.designWidth
                height: deckMixerViewport.designHeight
                anchors.centerIn: parent
                scale: deckMixerViewport.uniformScale
                transformOrigin: Item.Center

                RowLayout {
                    id: deckRow
                    anchors.fill: parent
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    spacing: 0

                    DeckControl {
                        deckName: "A"
                        visible: window.showDeckA
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckA
                    }

                    MixerSection {
                        visible: window.showMixer
                        Layout.preferredWidth: window.mixerBaseWidth
                        Layout.minimumWidth: window.mixerBaseWidth
                        Layout.fillHeight: true
                        engineA: deckA
                        engineB: deckB
                    }

                    DeckControl {
                        deckName: "B"
                        visible: window.showDeckB
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckB
                    }
                }
            }
        }

        // ── Second deck row (4-deck mode) ─────────────────────────────────
        Item {
            id: deckMixerViewport2
            property bool _vis: window.fourDeckMode && !window.libraryExpanded
            Layout.fillWidth: true
            Layout.minimumHeight: _vis ? window.scaledDeckMixerHeight : 0
            Layout.preferredHeight: _vis ? window.scaledDeckMixerHeight : 0
            Layout.maximumHeight: _vis ? window.scaledDeckMixerHeight : 0
            visible: _vis
            clip: true

            readonly property real designWidth: window.baseUiWidth
            readonly property real designHeight: window.baseDeckMixerHeight
            readonly property real uniformScaleRaw: Math.min(
                width  / Math.max(1, designWidth),
                height / Math.max(1, designHeight)
            )
            readonly property real uniformScale: window._snapScaleToPhysicalPixels(Math.max(0.1, uniformScaleRaw))

            Item {
                width: deckMixerViewport2.designWidth
                height: deckMixerViewport2.designHeight
                anchors.centerIn: parent
                scale: deckMixerViewport2.uniformScale
                transformOrigin: Item.Center

                RowLayout {
                    anchors.fill: parent
                    spacing: 0

                    DeckControl {
                        deckName: "C"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckC
                    }

                    MixerSection {
                        Layout.preferredWidth: window.mixerBaseWidth
                        Layout.minimumWidth: window.mixerBaseWidth
                        Layout.fillHeight: true
                        engineA: deckC
                        engineB: deckD
                        channelAId: "deckC"
                        channelBId: "deckD"
                        deckNameA: "C"
                        deckNameB: "D"
                    }

                    DeckControl {
                        deckName: "D"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckD
                    }
                }
            }
        }

        Rectangle {
            visible: window.fourDeckMode && !window.libraryExpanded
            Layout.fillWidth: true
            Layout.minimumHeight: visible ? 1 : 0
            Layout.preferredHeight: visible ? 1 : 0
            Layout.maximumHeight: visible ? 1 : 0
            color: "#000000"
        }

        CrossfaderBar {
            id: crossfaderBar
            property bool _vis: window.showCrossfader && !window.libraryExpanded
            Layout.fillWidth: true
            Layout.minimumHeight:  _vis ? 36 : 0
            Layout.preferredHeight: _vis ? 36 : 0
            Layout.maximumHeight:  _vis ? 36 : 0
            visible: _vis
            engineA: deckA
            engineB: deckB
            engineC: deckC
            engineD: deckD
            fourDeckMode: window.fourDeckMode
        }

        Rectangle {
            property bool _vis: window.showCrossfader && !window.libraryExpanded
            visible: _vis
            Layout.fillWidth: true
            Layout.minimumHeight:  _vis ? 1 : 0
            Layout.preferredHeight: _vis ? 1 : 0
            Layout.maximumHeight:  _vis ? 1 : 0
            color: "#000000"
        }

        FxBar {
            id: fxBarSection
            Layout.fillWidth: true
            Layout.minimumHeight: window.libraryExpanded ? 0 : window.fxBarHeight
            Layout.preferredHeight: window.libraryExpanded ? 0 : window.fxBarHeight
            Layout.maximumHeight: window.libraryExpanded ? 0 : window.fxBarHeight
            visible: !window.libraryExpanded && window.showFxBar
        }

        Library {
            id: librarySection
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: window.showLibrary
        }
    }

    // ── Previous unsafe shutdown notification ───────────────────────────────
    Rectangle {
        id: uncleanShutdownWarning
        anchors.top: parent.top
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width * 0.92, 640)
        height: unsafeShutdownRow.implicitHeight + 22
        radius: 6
        color: "#1d1508"
        border.color: "#8a5a14"
        z: 1001
        visible: window.uncleanShutdownWarningVisible
        opacity: visible ? 1.0 : 0.0

        property string visibleMessage: ""

        Behavior on opacity {
            NumberAnimation { duration: 180; easing.type: Easing.InOutQuad }
        }

        Row {
            id: unsafeShutdownRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 10

            Rectangle {
                width: 22
                height: 22
                radius: 11
                color: "#3a2505"
                border.color: "#a76a16"
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    anchors.centerIn: parent
                    text: "!"
                    color: "#ffb347"
                    font.pixelSize: 14
                    font.bold: true
                }
            }

            Text {
                width: parent.width - 64
                text: uncleanShutdownWarning.visibleMessage
                color: "#dfc08a"
                font.pixelSize: window.sp(12)
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                width: 22
                height: 22
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: unsafeShutdownDismissHover.hovered ? "#3a2610" : "#24180a"
                border.color: "#6a4518"
                HoverHandler { id: unsafeShutdownDismissHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: window.uncleanShutdownWarningVisible = false }

                Text {
                    anchors.centerIn: parent
                    text: "x"
                    color: "#b78b4a"
                    font.pixelSize: 11
                    font.bold: true
                }
            }
        }
    }

    // ── Audio device fallback notification ───────────────────────────────────
    Rectangle {
        id: audioFallbackToast
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width * 0.9, 600)
        height: toastCol.implicitHeight + 20
        radius: 6
        color: "#1a1200"
        border.color: "#7a4800"
        z: 998
        visible: opacity > 0
        opacity: 0.0

        property string message: ""

        Connections {
            target: typeof deckA !== "undefined" && deckA ? deckA : null
            function onAudioDeviceFallbackChanged() {
                var msg = deckA ? deckA.audioDeviceFallbackMessage : ""
                if (msg) {
                    audioFallbackToast.message = msg
                    audioFallbackToast.opacity = 1.0
                    audioFallbackDismissTimer.restart()
                } else {
                    audioFallbackToast.opacity = 0.0
                }
            }
        }

        Timer {
            id: audioFallbackDismissTimer
            interval: 12000
            repeat: false
            onTriggered: audioFallbackToast.opacity = 0.0
        }

        Behavior on opacity {
            NumberAnimation { duration: 300; easing.type: Easing.InOutQuad }
        }

        Row {
            id: toastCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 10

            Text {
                text: "⚠"
                color: "#ff9900"
                font.pixelSize: 14
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                width: parent.width - 60
                text: audioFallbackToast.message
                color: "#ccaa66"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                width: 22; height: 22
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: dismissH.hovered ? "#2a0000" : "#1a0000"
                border.color: "#442222"
                HoverHandler { id: dismissH; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: audioFallbackToast.opacity = 0.0 }
                Text { anchors.centerIn: parent; text: "✕"; color: "#885555"; font.pixelSize: 10 }
            }
        }
    }

    WelcomeScreen {
        id: welcomeOverlay
        active: false
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
                      ? "Closing..."
                      : "Are you sure you want to quit?"
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
                        text: "OK"
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
                        text: "Cancel"
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
                text: "Saving database and settings..."
                color: "#cfcfcf"
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: window.sp(11)
            }

            Text {
                width: parent.width
                visible: !window.exitShutdownInProgress
                text: (typeof libraryDb !== "undefined" && libraryDb !== null)
                      ? (libraryDb.mirroredDatabaseStatus ? libraryDb.mirroredDatabaseStatus : "DB A: unknown | DB B: unknown")
                      : "DB A: unknown | DB B: unknown"
                color: "#b8b8b8"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: window.sp(11)
            }

            CheckBox {
                id: manualBackupCheck
                visible: !window.exitShutdownInProgress
                checked: false
                padding: 0
                spacing: 8
                text: "Save manual database backup"
                indicator: Rectangle {
                    implicitWidth: 16
                    implicitHeight: 16
                    radius: 2
                    border.width: 1
                    border.color: parent.checked ? "#9a9a9a" : "#5a5a5a"
                    color: parent.checked ? "#4c4c4c" : "#232323"

                    Rectangle {
                        anchors.centerIn: parent
                        width: 8
                        height: 8
                        radius: 1
                        color: parent.visible && parent.parent.checked ? "#e5e5e5" : "transparent"
                        visible: parent.parent.checked
                    }
                }
                contentItem: Text {
                    text: manualBackupCheck.text
                    color: "#f0f0f0"
                    font.pixelSize: window.sp(12)
                    font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: manualBackupCheck.indicator.width + manualBackupCheck.spacing
                }
                onCheckedChanged: {
                    if (window.exitShutdownInProgress)
                        return
                    window.exitManualBackupRequested = checked
                }
            }

            Text {
                width: parent.width
                visible: !window.exitShutdownInProgress
                text: "When enabled, a separate backup file is saved on exit, only updated through this option."
                color: "#a8a8a8"
                font.pixelSize: window.sp(10)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }
}
