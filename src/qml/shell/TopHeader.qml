import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    color: UiTheme.navigationSurface
    // The expanded tray deliberately paints below this fixed-height item as an
    // overlay.  Keeping it unclipped prevents the workspace from being moved.
    clip: false

    // Height is fully controlled by parent layout — no implicitHeight here.
    // The first row remains fixed while the pull-down quick-access tray opens.
    readonly property int collapsedHeight: UiTheme.toolbarHeight

    readonly property bool compactBar: width / UiTheme.scale < 1100
    readonly property bool veryCompactBar: width / UiTheme.scale < 1000
    readonly property bool tinyBar: width / UiTheme.scale < 760
    readonly property bool compactHardwareBar: compactBar && !!root.Window.window && root.Window.window.allInOneMode === true
    readonly property color barHover: UiTheme.controlHover
    signal searchRequested()
    readonly property var stemsController: typeof stems !== "undefined" ? stems : null

    Rectangle {
        objectName: "stemPreanalysisTopProgress"
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        height: 3
        z: 3
        visible: root.stemsController && root.stemsController.preanalysisRunning
        color: UiTheme.knobTrack
        Rectangle {
            height: parent.height
            width: parent.width * (root.stemsController ? root.stemsController.preanalysisProgress : 0)
            color: UiTheme.play
            Behavior on width { NumberAnimation { duration: 120 } }
        }
        MouseArea { id: stemProgressHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
        ToolTip.visible: stemProgressHover.containsMouse
        ToolTip.text: root.stemsController
            ? "Stems " + Math.round(root.stemsController.preanalysisProgress * 100) + "% · "
              + root.stemsController.preanalysisTitle : ""
    }

    // ── Sizing helpers ───────────────────────────────────────────────────────
    readonly property int btnH:    Math.max(1, root.collapsedHeight)
    readonly property int padH:    Math.max(7, Math.round(btnH * 0.25))   // inner horizontal pad
    readonly property int sepW:    1                                        // divider width

    // VU meter sizing
    readonly property int vuW:     Math.max(72, Math.round(btnH * 2.6))
    readonly property int vuSegH:  Math.max(3,  Math.round(btnH * 0.12))

    // Beat dot sizing
    readonly property int dotSz:   Math.max(5,  Math.round(btnH * 0.18))

    // Master dial
    readonly property int dialSz:  Math.max(15, Math.round(btnH * 0.54))

    // Deck colors
    readonly property color clrA:  UiTheme.deckA
    readonly property color clrB:  UiTheme.deckB

    // Accent
    readonly property color accentBlue: UiTheme.masterBlue

    // Typography — fixed (no window-height scaling; bar has fixed px height)
    function sp(px) { return px }

    readonly property var viewToggleDefinitions: [
        { label: "Scrolling Waveforms", propertyName: "showWaveforms", defaultOn: true },
        { label: "Development Controls", propertyName: "showDevelopmentControls", defaultOn: false },
        { label: "Deck A", propertyName: "showDeckA", defaultOn: true },
        { label: "Deck B", propertyName: "showDeckB", defaultOn: true },
        { label: "Mixer", propertyName: "showMixer", defaultOn: true, hidden: true },
        { label: "FX Bar", propertyName: "showFxBar", defaultOn: true, hidden: true },
        { label: "Crossfader", propertyName: "showCrossfader", defaultOn: true, hidden: true },
        { label: "Library", propertyName: "showLibrary", defaultOn: true },
        { label: "AIO Deck Controls", propertyName: "showAioDeckControls", defaultOn: true }
    ]

    function viewToggleValue(propertyName, fallback) {
        var window = root.Window.window
        return window ? Boolean(window[propertyName]) : fallback
    }

    function toggleViewProperty(propertyName) {
        var window = root.Window.window
        if (window)
            window[propertyName] = !window[propertyName]
    }

    component ViewToggleCard: Rectangle {
        id: toggleCard
        required property var modelData
        required property string label
        required property string propertyName
        required property real cardWidth
        property bool defaultOn: true
        property bool hidden: false
        readonly property bool on: root.viewToggleValue(propertyName, defaultOn)
        signal toggleRequested()

        width: cardWidth
        height: hidden ? 0 : 34
        visible: !hidden
        color: toggleMouse.containsMouse ? UiTheme.surfaceRaised : UiTheme.panel

        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.right: togglePill.left
            anchors.rightMargin: 8
            text: toggleCard.label
            color: toggleCard.on ? UiTheme.textPrimary : UiTheme.textMuted
            font.pixelSize: root.sp(11)
            elide: Text.ElideRight
        }

        Rectangle {
            id: togglePill
            width: 24
            height: 12
            radius: 0
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 12
            color: toggleCard.on ? UiTheme.blue : UiTheme.surfaceInset

            Rectangle {
                width: 8
                height: 8
                radius: 0
                color: UiTheme.textPrimary
                y: 2
                x: toggleCard.on ? parent.width - 10 : 2
                Behavior on x { NumberAnimation { duration: 80 } }
            }
        }

        MouseArea {
            id: toggleMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: toggleCard.toggleRequested()
        }
    }

    component StatusIcon: UiIcon {}

    // ── State ────────────────────────────────────────────────────────────────
    property string selectedSourceType: "local"
    property string selectedSourceName: "Lokale Library"
    property string currentTime: "00:00"
    property real   totalLatencyMs: 0.0
    property var    latencyRows: []
    property var    audioPerfStats: ({})
    property int    beatUiTick: 0
    property var    settingsWindowInstance: null

    // ── Timers ───────────────────────────────────────────────────────────────
    Connections {
        target: (typeof controlClock !== "undefined") ? controlClock : null
        property int latencyDivider: 0
        function onHousekeepingTick() {
            var d = new Date()
            root.currentTime = d.getHours().toString().padStart(2,"0") + ":"
                             + d.getMinutes().toString().padStart(2,"0")
        }
        function onStatisticsTick() {
            if ((++latencyDivider % 3) === 0)
                root.refreshLatencyInfo()
        }
        function onLinkTick() { root.beatUiTick++ }
    }

    Connections {
        target: deckA
        function onProgressChanged() { root.beatUiTick++ }
        function onPlayingChanged() { root.beatUiTick++ }
    }
    Connections {
        target: deckB
        function onProgressChanged() { root.beatUiTick++ }
        function onPlayingChanged() { root.beatUiTick++ }
    }

    Component.onCompleted: {
        var d = new Date()
        root.currentTime = d.getHours().toString().padStart(2,"0") + ":"
                         + d.getMinutes().toString().padStart(2,"0")
        root.refreshLatencyInfo()
    }

    Component.onDestruction: {
        if (root.settingsWindowInstance) {
            root.settingsWindowInstance.destroy()
            root.settingsWindowInstance = null
        }
    }

    // ── Functions ────────────────────────────────────────────────────────────
    function refreshLatencyInfo() {
        if (!deckA || !deckA.latencyBreakdown) return
        var rows = deckA.latencyBreakdown()
        if (!rows || rows.length === 0) return
        latencyRows = rows
        audioPerfStats = deckA.audioPerformanceStats ? deckA.audioPerformanceStats() : ({})
        var sum = 0.0
        for (var i = 0; i < rows.length; ++i) {
            if (rows[i].countInTotal === false) continue
            var ms = Number(rows[i].ms)
            if (!isNaN(ms) && isFinite(ms)) sum += ms
        }
        totalLatencyMs = sum
    }

    function deckBeatInfo(engine) {
        var _tick = root.beatUiTick
        if (!engine || !engine.trackData || !engine.trackData.isBpmAnalyzed)
            return { valid: false, beatInBar: 0, barNumber: 0 }
        return engine.beatDisplayInfo()
    }

    // The desktop settings tree is large and is almost never needed during a
    // performance session. Construct it on first use instead of keeping a
    // hidden Window (and its mapping editor) alive from application startup.
    Component {
        id: settingsWindowFactory
        Window {
            id: standaloneSettingsWindow
            title: "Settings"
            width: 800
            height: 600
            minimumWidth: 600
            minimumHeight: 400
            visible: false
            color: UiTheme.surfaceInset
            flags: Qt.Dialog

            SettingsPanel {
                anchors.fill: parent
                active: standaloneSettingsWindow.visible
            }
        }
    }

    function showStandaloneSettings() {
        if (!root.settingsWindowInstance)
            root.settingsWindowInstance = settingsWindowFactory.createObject(null)
        if (!root.settingsWindowInstance)
            return
        root.settingsWindowInstance.show()
        root.settingsWindowInstance.raise()
        root.settingsWindowInstance.requestActivate()
    }

    // ── Latency popup ────────────────────────────────────────────────────────
    Popup {
        id: latencyPopup
        parent: monitorBlock
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        padding: 1
        background: Rectangle { color: UiTheme.surface; border.color: UiTheme.borderSubtle; border.width: 1 }

        contentItem: Column {
            spacing: 0

            Rectangle {
                width: latencyPopup.availableWidth; height: 28
                color: UiTheme.panelInset
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    text: "LATENCY BREAKDOWN"
                    color: UiTheme.textLabel; font.pixelSize: root.sp(9); font.bold: true; font.letterSpacing: 0.6
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    text: root.totalLatencyMs.toFixed(1) + " ms"
                    color: UiTheme.blue; font.pixelSize: root.sp(9); font.family: UiTheme.numericFontFamily; font.bold: true
                }
            }

            Repeater {
                model: root.latencyRows
                Rectangle {
                    required property var modelData
                    required property int index
                    width: latencyPopup.availableWidth; height: 26
                    color: index % 2 === 0 ? UiTheme.surfaceInset : UiTheme.panelInset
                    Row {
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                        Text {
                            width: 170; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: modelData.countInTotal === false ? UiTheme.textMuted : UiTheme.textSecondary
                            font.pixelSize: root.sp(8); elide: Text.ElideRight
                        }
                        Text {
                            width: 54; anchors.verticalCenter: parent.verticalCenter
                            text: Number(modelData.ms).toFixed(1) + " ms"
                            color: UiTheme.textPrimary; font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                            horizontalAlignment: Text.AlignRight
                        }
                        Text {
                            width: 66; anchors.verticalCenter: parent.verticalCenter
                            text: Number(modelData.samples).toString() + " smp"
                            color: UiTheme.textDim; font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }

            Rectangle {
                width: latencyPopup.availableWidth; height: 28
                color: UiTheme.panelInset
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    text: "CALLBACK PROFILE"
                    color: UiTheme.textLabel; font.pixelSize: root.sp(9); font.bold: true; font.letterSpacing: 0.6
                }
            }

            Rectangle {
                width: latencyPopup.availableWidth; height: 28
                color: UiTheme.surfaceInset
                Row {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                    Text {
                        width: 82; anchors.verticalCenter: parent.verticalCenter
                        text: "AVG"
                        color: UiTheme.textDim; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: ((Number(root.audioPerfStats.callbackAverageUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                        color: UiTheme.textPrimary; font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        width: 54; anchors.verticalCenter: parent.verticalCenter
                        text: "WORST"
                        color: UiTheme.textDim; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: ((Number(root.audioPerfStats.callbackWorstUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                        color: UiTheme.textPrimary; font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            Rectangle {
                width: latencyPopup.availableWidth; height: 28
                color: UiTheme.panelInset
                Row {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                    Text {
                        width: 82; anchors.verticalCenter: parent.verticalCenter
                        text: "BUDGET"
                        color: UiTheme.textDim; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: ((Number(root.audioPerfStats.callbackBudgetUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                        color: UiTheme.textPrimary; font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        width: 54; anchors.verticalCenter: parent.verticalCenter
                        text: "DSP XRUNS"
                        color: UiTheme.textDim; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: Number(root.audioPerfStats.callbackOverruns) ? Number(root.audioPerfStats.callbackOverruns).toString() : "0"
                        color: Number(root.audioPerfStats.callbackOverruns) > 0 ? UiTheme.error : UiTheme.textPrimary
                        font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            Rectangle {
                width: latencyPopup.availableWidth; height: 24
                color: UiTheme.surfaceInset
                Row {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                    Text {
                        width: 136; anchors.verticalCenter: parent.verticalCenter
                        text: "DEVICE XRUNS"
                        color: UiTheme.textDim; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: Number(root.audioPerfStats.hardwareXruns) ? Number(root.audioPerfStats.hardwareXruns).toString() : "0"
                        color: Number(root.audioPerfStats.hardwareXruns) > 0 ? UiTheme.error : UiTheme.textPrimary
                        font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            Rectangle {
                width: latencyPopup.availableWidth; height: 24
                color: UiTheme.panelInset
                Row {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                    Text {
                        width: 136; anchors.verticalCenter: parent.verticalCenter
                        text: "RT SCHEDULER"
                        color: UiTheme.textDim; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 150; anchors.verticalCenter: parent.verticalCenter
                        text: String(root.audioPerfStats.realtimeScheduling || "waiting-for-callback")
                        color: {
                            const state = String(root.audioPerfStats.realtimeScheduling || "")
                            return state === "sched-fifo-active" || state === "already-realtime"
                                ? UiTheme.green : UiTheme.warning
                        }
                        font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            Rectangle {
                width: latencyPopup.availableWidth; height: 24
                color: UiTheme.surfaceInset
                Row {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                    Text {
                        width: 136; anchors.verticalCenter: parent.verticalCenter
                        text: "UI RENDER"
                        color: UiTheme.textDim; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 150; anchors.verticalCenter: parent.verticalCenter
                        text: {
                            if (typeof renderPressurePolicy === "undefined"
                                    || !renderPressurePolicy)
                                return "normal"
                            return String(renderPressurePolicy.tier)
                                + " / " + renderPressurePolicy.waveformUpdateIntervalMs
                                + " ms"
                        }
                        color: {
                            if (typeof renderPressurePolicy === "undefined"
                                    || !renderPressurePolicy)
                                return UiTheme.textPrimary
                            const tier = String(renderPressurePolicy.tier)
                            if (tier === "audio-first")
                                return UiTheme.warning
                            if (tier === "suspended")
                                return UiTheme.textDim
                            return UiTheme.green
                        }
                        font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            Repeater {
                model: root.audioPerfStats.fxProfiles ? root.audioPerfStats.fxProfiles : []
                Rectangle {
                    required property var modelData
                    width: latencyPopup.availableWidth; height: 24
                    color: UiTheme.surfaceInset
                    Row {
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                        Text {
                            width: 136; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: UiTheme.textDim; font.pixelSize: root.sp(8); elide: Text.ElideRight
                        }
                        Text {
                            width: 62; anchors.verticalCenter: parent.verticalCenter
                            text: ((Number(modelData.averageUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                            color: UiTheme.textPrimary; font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                            horizontalAlignment: Text.AlignRight
                        }
                        Text {
                            width: 46; anchors.verticalCenter: parent.verticalCenter
                            text: "max"
                            color: UiTheme.textMuted; font.pixelSize: root.sp(8); font.bold: true
                        }
                        Text {
                            width: 62; anchors.verticalCenter: parent.verticalCenter
                            text: ((Number(modelData.worstUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                            color: UiTheme.textPrimary; font.pixelSize: root.sp(8); font.family: UiTheme.numericFontFamily
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }
        }
    }

    // ── View menu popup ──────────────────────────────────────────────────────
    Popup {
        id: viewMenuPopup
        parent: viewMenuBtn
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        padding: 1
        width: 210
        background: Rectangle { color: UiTheme.surface; border.color: UiTheme.borderSubtle; border.width: 1 }

        contentItem: Column {
            spacing: 0

            Rectangle {
                width: viewMenuPopup.availableWidth; height: 28
                color: UiTheme.panelInset
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    text: "VIEW TOGGLES"
                    color: UiTheme.textLabel; font.pixelSize: root.sp(9); font.bold: true; font.letterSpacing: 0.6
                }
            }

            Repeater {
                model: root.viewToggleDefinitions.slice(0, 2)
                delegate: ViewToggleCard {
                    label: modelData.label
                    propertyName: modelData.propertyName
                    cardWidth: viewMenuPopup.availableWidth
                    defaultOn: modelData.defaultOn
                    hidden: modelData.hidden === true
                    onToggleRequested: root.toggleViewProperty(propertyName)
                }
            }

            Rectangle {
                id: vt_deckMode
                width: viewMenuPopup.availableWidth
                height: 30
                color: vt_deckModeMouse.containsMouse ? UiTheme.surfaceRaised : UiTheme.panel
                readonly property bool fourDeck: root.Window.window ? root.Window.window.fourDeckMode : false

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    text: "Deck Layout"
                    color: UiTheme.textSecondary
                    font.pixelSize: root.sp(9)
                }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    spacing: 2

                    Rectangle {
                        width: 26
                        height: 16
                        radius: 0
                        color: !vt_deckMode.fourDeck ? UiTheme.surfaceRaised : UiTheme.surfaceInset
                        Text {
                            anchors.centerIn: parent
                            text: "2"
                            color: !vt_deckMode.fourDeck ? UiTheme.textPrimary : UiTheme.textMuted
                            font.pixelSize: root.sp(8)
                            font.bold: true
                            font.family: UiTheme.numericFontFamily
                        }
                    }

                    Rectangle {
                        width: 26
                        height: 16
                        radius: 0
                        color: vt_deckMode.fourDeck ? UiTheme.surfaceRaised : UiTheme.surfaceInset
                        Text {
                            anchors.centerIn: parent
                            text: "4"
                            color: vt_deckMode.fourDeck ? UiTheme.textPrimary : UiTheme.textMuted
                            font.pixelSize: root.sp(8)
                            font.bold: true
                            font.family: UiTheme.numericFontFamily
                        }
                    }
                }

                MouseArea {
                    id: vt_deckModeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.fourDeckMode = !root.Window.window.fourDeckMode
                }
            }

            Repeater {
                model: root.viewToggleDefinitions.slice(2)
                delegate: ViewToggleCard {
                    label: modelData.label
                    propertyName: modelData.propertyName
                    cardWidth: viewMenuPopup.availableWidth
                    defaultOn: modelData.defaultOn
                    hidden: modelData.hidden === true
                    onToggleRequested: root.toggleViewProperty(propertyName)
                }
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // MAIN ROW
    // ════════════════════════════════════════════════════════════════════════
    RowLayout {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.collapsedHeight
        z: 10
        spacing: 0

        Rectangle {
            id: sourceButton
            objectName: "sourceSelectorButton"
            readonly property bool selected: root.Window.window && root.Window.window.sourcePageActive === true
            Layout.preferredWidth: root.btnH
            Layout.preferredHeight: root.btnH
            Layout.fillHeight: true
            color: sourceMouse.containsMouse || selected ? UiTheme.inspectorInset : UiTheme.navigationSurface
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                height: 2
                color: sourceButton.selected ? UiTheme.blue : UiTheme.borderSubtle
            }
            Column {
                anchors.centerIn: parent
                spacing: 1
                StatusIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    kind: root.selectedSourceType === "usb" ? "usb" : "computer"
                    ink: UiTheme.blue
                    width: 17; height: 17
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.selectedSourceType === "usb" ? "USB" : "LOCAL"
                    color: UiTheme.textPrimary
                    font.pixelSize: 9; font.bold: true
                }
            }
            ToolTip.visible: sourceMouse.containsMouse
            ToolTip.delay: 600
            ToolTip.text: "Source · " + root.selectedSourceName
            MouseArea {
                id: sourceMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.Window.window) root.Window.window.toggleLibrarySourceView()
            }
        }

        // ── Branding ─────────────────────────────────────────────────────────
        Item {
            visible: !root.compactBar
            Layout.preferredWidth: brandRow.implicitWidth + root.padH * 2
            Layout.fillHeight: true

            Row {
                id: brandRow
                anchors.centerIn: parent
                spacing: 6

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1
                    Text {
                        text: "BROCKDJ"
                        color: UiTheme.textPrimary
                        font.pixelSize: root.sp(13)
                        font.bold: true
                        font.letterSpacing: 1.4
                    }

                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; Layout.alignment: Qt.AlignVCenter; color: UiTheme.separatorSubtle }

        Button {
            objectName: "performanceNavigation"
            Layout.preferredWidth: UiTheme.px(root.veryCompactBar ? 48 : root.compactBar ? 64 : 100)
            leftPadding: 6; rightPadding: 6
            Layout.fillHeight: true
            text: root.compactBar ? "Live" : "Performance"
            labelSize: 11
            checked: root.Window.window && root.Window.window.activeMainTab === "performance"
            onClicked: if (root.Window.window) {
                root.Window.window.activeMainTab = "performance"
                root.Window.window.closeTopBarPullDown()
            }
        }

        // ── Primary navigation ───────────────────────────────────────────────
        Button {
            id: libraryButton
            objectName: "libraryNavigation"
            Layout.preferredWidth: UiTheme.px(root.veryCompactBar ? 60 : root.compactBar ? 64 : 88)
            leftPadding: 6; rightPadding: 6
            Layout.fillHeight: true
            readonly property bool active: root.Window.window
                ? (!root.Window.window.utilityPageActive && (root.Window.window.allInOneMode ? root.Window.window.libraryPanelActive : root.Window.window.showLibrary))
                : false
            checked: active
            text: "Library"
            labelSize: 11
            onClicked: if (root.Window.window) root.Window.window.toggleAllInOneLibrary()
        }

        Button {
            objectName: "samplerNavigation"
            Layout.preferredWidth: UiTheme.px(root.veryCompactBar ? 64 : 88)
            Layout.fillHeight: true
            leftPadding: 6; rightPadding: 6
            text: "Sampler"; labelSize: 11
            checked: root.Window.window && root.Window.window.samplerPageActive === true
            onClicked: if (root.Window.window) root.Window.window.openMainPage("sampler")
        }

        Button {
            objectName: "effectsNavigation"
            Layout.preferredWidth: UiTheme.px(root.veryCompactBar ? 44 : 60)
            Layout.fillHeight: true
            leftPadding: 6; rightPadding: 6
            text: "FX"; labelSize: 11
            checked: root.Window.window && root.Window.window.fxPageActive === true
            onClicked: if (root.Window.window) root.Window.window.openMainPage("fx")
        }

        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; Layout.alignment: Qt.AlignVCenter; color: UiTheme.separatorSubtle }

        Rectangle {
            id: searchButton
            visible: !root.veryCompactBar
            Layout.preferredWidth: root.compactBar ? 38 : 88
            Layout.fillHeight: true
            color: searchMouse.containsMouse ? root.barHover : "transparent"
            Row {
                anchors.centerIn: parent
                spacing: 4
                StatusIcon { kind: "search"; ink: UiTheme.textLabel; width: 18; height: 18; anchors.verticalCenter: parent.verticalCenter }
                Text { visible: !root.compactBar; text: "Search"; color: UiTheme.textSecondary; font.pixelSize: root.sp(11); font.bold: true; font.letterSpacing: 0.2; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea { id: searchMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.searchRequested() }
        }

        Button {
            objectName: "settingsNavigation"
            Layout.preferredWidth: UiTheme.px(root.veryCompactBar ? 64 : root.compactBar ? 68 : 88)
            leftPadding: 6; rightPadding: 6
            Layout.fillHeight: true
            text: "Settings"
            labelSize: 11
            checked: root.Window.window && root.Window.window.settingsPanelActive
            onClicked: if (root.Window.window) {
                if (!root.Window.window.toggleAllInOneSettings())
                    root.showStandaloneSettings()
                root.Window.window.closeTopBarPullDown()
            }
        }

        // Balance the larger output/status group while reserving meter space.
        Item { Layout.fillWidth: true; Layout.preferredWidth: 180 }
        Item {
            id: meterSlot
            Layout.preferredWidth: root.compactBar ? 112 : 164
            Layout.fillHeight: true
        }

        // ── Master loudness (EBU R128 short-term, LUFS) ──────────────────────
        Item {
            id: loudnessReadout
            objectName: "masterLoudnessReadout"
            visible: !root.veryCompactBar && loudness !== null
            Layout.preferredWidth: root.compactBar ? 46 : 58
            Layout.fillHeight: true

            readonly property var loudness: typeof mixerControl !== "undefined" && mixerControl
                                            ? mixerControl.loudness : null
            readonly property real shortTerm: loudness ? loudness.shortTerm : -70
            readonly property real target: loudness ? loudness.targetLufs : -9
            readonly property bool silent: !loudness || shortTerm <= loudness.floorLufs + 0.01
            // Within ±1 LU of the target is "on target"; louder than +1 LU warns.
            readonly property color levelColor: silent ? UiTheme.textMuted
                : shortTerm > target + 3 ? UiTheme.error
                : shortTerm > target + 1 ? UiTheme.warning
                : shortTerm >= target - 1 ? UiTheme.play
                : UiTheme.textSecondary

            function fmt(value) {
                return !loudness || value <= loudness.floorLufs + 0.01 ? "–" : value.toFixed(1)
            }

            Column {
                anchors.centerIn: parent
                spacing: 0
                Text {
                    objectName: "masterLoudnessValue"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: loudnessReadout.fmt(loudnessReadout.shortTerm)
                    color: loudnessReadout.levelColor
                    font.pixelSize: root.sp(12); font.bold: true; font.family: "monospace"
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "LUFS"
                    color: UiTheme.textLabel
                    font.pixelSize: root.sp(8); font.letterSpacing: 0.4
                }
            }

            MouseArea {
                id: loudnessMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                // Click restarts the integrated measurement; wheel moves the target.
                onClicked: if (loudnessReadout.loudness) loudnessReadout.loudness.reset()
                onWheel: (wheel) => {
                    if (loudnessReadout.loudness)
                        loudnessReadout.loudness.targetLufs = Math.round(loudnessReadout.target + (wheel.angleDelta.y > 0 ? 1 : -1))
                }
            }
            ToolTip.visible: loudnessMouse.containsMouse
            ToolTip.delay: 400
            ToolTip.text: loudness
                ? "Master-Lautheit (EBU R128)\n"
                  + "Momentan: " + fmt(loudness.momentary) + " LUFS\n"
                  + "Kurzzeit (3 s): " + fmt(loudness.shortTerm) + " LUFS\n"
                  + "Integriert: " + fmt(loudness.integrated) + " LUFS\n"
                  + "Max. Kurzzeit: " + fmt(loudness.maxShortTerm) + " LUFS\n"
                  + "Ziel: " + target.toFixed(0) + " LUFS (Mausrad ändert)\n"
                  + "Klick setzt die Messung zurück"
                : ""
        }
        Item { Layout.fillWidth: true }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; Layout.alignment: Qt.AlignVCenter; color: UiTheme.separatorSubtle }

        // ── Master volume ─────────────────────────────────────────────────────
        Rectangle {
            visible: !root.compactBar
            Layout.preferredWidth: root.compactBar ? 58 : 100
            Layout.fillHeight: true
            color: "transparent"

            Row {
                anchors.centerIn: parent
                spacing: 4

                Text {
                    text: root.compactBar ? "MST" : "MASTER"
                    color: UiTheme.textLabel
                    font.pixelSize: root.sp(11); font.bold: true; font.letterSpacing: 0.2
                    anchors.verticalCenter: parent.verticalCenter
                }

                Dial {
                    id: masterVolDial
                    width: root.dialSz; height: root.dialSz
                    from: 0.0; to: 1.0; value: 0.8
                    anchors.verticalCenter: parent.verticalCenter

                    background: Rectangle {
                        x: masterVolDial.width  / 2 - width  / 2
                        y: masterVolDial.height / 2 - height / 2
                        width: masterVolDial.width; height: masterVolDial.height
                        radius: width / 2; color: "transparent"

                        Canvas {
                            id: mstArc
                            anchors.fill: parent; antialiasing: true
                            onPaint: {
                                var ctx = getContext("2d"); ctx.reset()
                                var cx = width / 2; var cy = height / 2
                                var r = Math.min(width, height) * 0.44
                                var norm = Math.max(0, Math.min(1,
                                    (masterVolDial.value - masterVolDial.from)
                                    / (masterVolDial.to - masterVolDial.from)))
                                ctx.lineWidth = Math.max(1.5, width * 0.07)
                                ctx.lineCap   = "butt"
                                // track
                                ctx.strokeStyle = UiTheme.knobTrack
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 120 * Math.PI/180, (120+300) * Math.PI/180)
                                ctx.stroke()
                                // fill
                                ctx.strokeStyle = UiTheme.blue
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 120 * Math.PI/180, (120 + norm*300) * Math.PI/180)
                                ctx.stroke()
                            }
                            Connections {
                                target: masterVolDial
                                function onValueChanged() { mstArc.requestPaint() }
                            }
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width * 0.72; height: parent.height * 0.72
                            radius: width / 2; color: UiTheme.knobFace
                        }
                    }

                    handle: Item {
                        id: mstHandle
                        x: masterVolDial.background.x + masterVolDial.background.width  / 2 - width  / 2
                        y: masterVolDial.background.y + masterVolDial.background.height / 2 - height / 2
                        width:  masterVolDial.width  * 0.72
                        height: masterVolDial.height * 0.72
                        Rectangle {
                            width: 1.5; height: parent.height * 0.42; color: UiTheme.knobHandle
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top; anchors.topMargin: -1
                        }
                        transform: Rotation {
                            angle: masterVolDial.angle
                            origin.x: mstHandle.width  / 2
                            origin.y: mstHandle.height / 2
                        }
                    }

                    onValueChanged: if (deckA) deckA.setMasterVolume(value)
                    TapHandler {
                        onDoubleTapped: {
                            masterVolDial.enabled = false
                            masterVolDial.value   = 0.8
                            masterVolDial.enabled = true
                        }
                    }
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; Layout.alignment: Qt.AlignVCenter; color: UiTheme.separatorSubtle }

        // ── Headphone cue ────────────────────────────────────────────────────
        Rectangle {
            id: headphoneCueBlock
            visible: !root.compactBar
            Layout.preferredWidth: root.compactBar ? 88 : 112
            Layout.fillHeight: true
            color: "transparent"

            property bool syncingCueMix: false
            readonly property bool masterCueOn: deckA ? deckA.masterCueEnabled : false

            Connections {
                target: deckA
                function onHeadphoneMixChanged() {
                    headphoneCueBlock.syncingCueMix = true
                    cueMixDial.value = deckA ? deckA.headphoneMix : 0.0
                    headphoneCueBlock.syncingCueMix = false
                }
            }

            Row {
                anchors.centerIn: parent
                spacing: 4

                Rectangle {
                    width: 32
                    height: Math.max(16, root.btnH * 0.48)
                    radius: 0
                    color: headphoneCueBlock.masterCueOn ? UiTheme.surfaceRaised
                         : masterCueHover.hovered ? UiTheme.panelRaised : UiTheme.panelInset
                    border.color: headphoneCueBlock.masterCueOn ? UiTheme.borderActive
                                : masterCueHover.hovered ? UiTheme.borderHover : UiTheme.borderSubtle

                    Text {
                        anchors.centerIn: parent
                        text: "CUE"
                        color: headphoneCueBlock.masterCueOn ? UiTheme.blue : UiTheme.textMuted
                        font.pixelSize: root.sp(7)
                        font.bold: true
                        font.letterSpacing: 0.5
                    }

                    HoverHandler { id: masterCueHover; cursorShape: Qt.PointingHandCursor }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (deckA) deckA.setMasterCueEnabled(!deckA.masterCueEnabled)
                    }
                }

                Dial {
                    id: cueMixDial
                    width: root.dialSz
                    height: root.dialSz
                    from: 0.0
                    to: 1.0
                    value: deckA ? deckA.headphoneMix : 0.0
                    anchors.verticalCenter: parent.verticalCenter

                    background: Rectangle {
                        x: cueMixDial.width / 2 - width / 2
                        y: cueMixDial.height / 2 - height / 2
                        width: cueMixDial.width
                        height: cueMixDial.height
                        radius: width / 2
                        color: "transparent"

                        Canvas {
                            id: cueMixArc
                            anchors.fill: parent
                            antialiasing: true
                            onPaint: {
                                var ctx = getContext("2d"); ctx.reset()
                                var cx = width / 2; var cy = height / 2
                                var r = Math.min(width, height) * 0.44
                                var norm = Math.max(0, Math.min(1, cueMixDial.value))
                                ctx.lineWidth = Math.max(1.5, width * 0.07)
                                ctx.lineCap = "butt"
                                ctx.strokeStyle = UiTheme.knobTrack
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 120 * Math.PI / 180, 420 * Math.PI / 180)
                                ctx.stroke()
                                ctx.strokeStyle = UiTheme.blue
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 120 * Math.PI / 180, (120 + norm * 300) * Math.PI / 180)
                                ctx.stroke()
                            }
                            Connections {
                                target: cueMixDial
                                function onValueChanged() { cueMixArc.requestPaint() }
                            }
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width * 0.72
                            height: parent.height * 0.72
                            radius: width / 2
                            color: UiTheme.knobFace
                        }
                    }

                    handle: Item {
                        id: cueMixHandle
                        x: cueMixDial.background.x + cueMixDial.background.width / 2 - width / 2
                        y: cueMixDial.background.y + cueMixDial.background.height / 2 - height / 2
                        width: cueMixDial.width * 0.72
                        height: cueMixDial.height * 0.72
                        Rectangle {
                            width: 1.5
                            height: parent.height * 0.42
                            color: UiTheme.knobHandle
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: -1
                        }
                        transform: Rotation {
                            angle: cueMixDial.angle
                            origin.x: cueMixHandle.width / 2
                            origin.y: cueMixHandle.height / 2
                        }
                    }

                    onValueChanged: {
                        if (!headphoneCueBlock.syncingCueMix && deckA)
                            deckA.setHeadphoneMix(value)
                    }
                    TapHandler {
                        onDoubleTapped: cueMixDial.value = 0.5
                    }
                }

                Text {
                    text: root.compactBar ? "MST" : "MASTER"
                    color: headphoneCueBlock.masterCueOn ? UiTheme.blue : UiTheme.textMuted
                    font.pixelSize: root.sp(7)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; Layout.alignment: Qt.AlignVCenter; color: UiTheme.separatorSubtle }

        // ── System monitor — LAT + CPU + RAM ─────────────────────────────────
        Rectangle {
            id: monitorBlock
            visible: !root.tinyBar
            Layout.preferredWidth: root.compactBar ? 112 : 128 // fixed to prevent value jitter
            Layout.fillHeight: true
            color: latMouse.containsMouse ? root.barHover : "transparent"
            Behavior on color { ColorAnimation { duration: 100 } }

            readonly property color latClr: root.totalLatencyMs >= 35.0 ? UiTheme.error
                                           : root.totalLatencyMs >= 20.0 ? UiTheme.warning
                                           : UiTheme.textPrimary
            readonly property real cpuVal: sysMonitor ? sysMonitor.cpuUsage : 0
            readonly property real ramVal: sysMonitor ? sysMonitor.ramUsage : 0

            function metricColor(v) {
                return v > 0.8 ? UiTheme.error : v > 0.5 ? UiTheme.warning : UiTheme.textMuted
            }
            function barColor(v, hue) {
                return v > 0.8 ? UiTheme.error : v > 0.5 ? UiTheme.warning : hue
            }

            Column {
                anchors.centerIn: parent
                spacing: 4
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 6
                    Text { text: "LAT"; color: UiTheme.textDim; font.pixelSize: 9; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        text: root.totalLatencyMs > 0 ? root.totalLatencyMs.toFixed(1) + " ms" : "— ms"
                        color: monitorBlock.latClr
                        font.pixelSize: 11; font.bold: true; font.family: UiTheme.numericFontFamily
                    }
                    Text { text: latencyPopup.visible ? "▴" : "▾"; color: UiTheme.textDim; font.pixelSize: 9 }
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 8
                    Text { text: "CPU " + Math.round(monitorBlock.cpuVal * 100) + "%"; color: monitorBlock.metricColor(monitorBlock.cpuVal); font.pixelSize: 9; font.family: UiTheme.numericFontFamily }
                    Text { text: "RAM " + Math.round(monitorBlock.ramVal * 100) + "%"; color: monitorBlock.metricColor(monitorBlock.ramVal); font.pixelSize: 9; font.family: UiTheme.numericFontFamily }
                }
            }

            MouseArea {
                id: latMouse
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root.refreshLatencyInfo()
                    if (latencyPopup.visible) { latencyPopup.close(); return }
                    var p = monitorBlock.mapToItem(latencyPopup.parent, 0, monitorBlock.height + 2)
                    latencyPopup.width = 340
                    latencyPopup.x = p.x; latencyPopup.y = p.y
                    latencyPopup.open()
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; Layout.alignment: Qt.AlignVCenter; color: UiTheme.separatorSubtle }

        // ── Ableton Link ─────────────────────────────────────────────────────
        Rectangle {
            id: linkBlock
            Layout.preferredWidth: 52
            Layout.fillHeight: true
            color: "transparent"

            readonly property bool on: linkManager && linkManager.enabled
            readonly property int beatIndex: linkManager ? (((Math.floor(linkManager.beat) % 4) + 4) % 4) : 0

            Column {
                anchors.centerIn: parent
                spacing: 1

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 3
                    Rectangle {
                        width: 4; height: 4; radius: 0
                        anchors.verticalCenter: parent.verticalCenter
                        color: linkBlock.on ? UiTheme.play : UiTheme.separatorSubtle
                    }
                    Text {
                        text: "LINK"
                        color: linkBlock.on ? UiTheme.play : UiTheme.textMuted
                        font.pixelSize: root.sp(7)
                        font.bold: true
                        font.letterSpacing: 0.3
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: linkManager ? linkManager.bpm.toFixed(1) : "120.0"
                    color: linkBlock.on ? UiTheme.textPrimary : UiTheme.textMuted
                    font.pixelSize: root.sp(10)
                    font.family: UiTheme.numericFontFamily
                    font.bold: true
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 2
                    Repeater {
                        model: 4
                        Rectangle {
                            required property int index
                            width: 7
                            height: 2
                            radius: 0
                            color: linkBlock.on && index === linkBlock.beatIndex ? UiTheme.play : UiTheme.surfaceInset
                        }
                    }
                }
            }

            MouseArea {
                id: linkMouse
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: if (linkManager) linkManager.enabled = !linkManager.enabled
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; Layout.alignment: Qt.AlignVCenter; color: UiTheme.separatorSubtle }

        // ── Clock ─────────────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 64
            Layout.fillHeight: true
            color: "transparent"

            Text {
                id: clockText
                anchors.centerIn: parent
                text: typeof setRecorder !== "undefined" && setRecorder && setRecorder.recording ? "REC" : root.currentTime
                color: typeof setRecorder !== "undefined" && setRecorder && setRecorder.recording ? UiTheme.error : UiTheme.textSecondary
                font.pixelSize: root.sp(14); font.family: UiTheme.numericFontFamily; font.bold: true
            }
        }

        Row {
            Layout.preferredWidth: 58
            visible: !root.veryCompactBar
            Layout.alignment: Qt.AlignVCenter
            spacing: 8
            readonly property var monitor: typeof sysMonitor !== "undefined" ? sysMonitor : null
            Item {
                width: 20; height: 20
                StatusIcon {
                    anchors.fill: parent
                    kind: parent.parent.monitor && parent.parent.monitor.networkOnline ? parent.parent.monitor.networkTransport : "unknown"
                    ink: parent.parent.monitor && parent.parent.monitor.networkOnline ? UiTheme.textSecondary : UiTheme.textDim
                }
                Text {
                    anchors.right: parent.right; anchors.bottom: parent.bottom
                    text: "×"; color: UiTheme.error; font.pixelSize: 10; font.bold: true
                    visible: parent.parent.monitor && parent.parent.monitor.networkKnown && !parent.parent.monitor.networkOnline
                }
                ToolTip.visible: networkHover.containsMouse
                ToolTip.text: parent.monitor && parent.monitor.networkOnline
                              ? (parent.monitor.networkTransport === "ethernet" ? "LAN verbunden" : parent.monitor.networkTransport === "wifi" ? "WLAN verbunden" : "Internet verbunden")
                              : parent.monitor && parent.monitor.networkKnown ? "Kein Internet" : "Netzwerkstatus unbekannt"
                MouseArea { id: networkHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
            }
            StatusIcon {
                width: 20; height: 20
                kind: "bluetooth"
                ink: parent.monitor && parent.monitor.bluetoothEnabled ? UiTheme.blue : UiTheme.textDim
                ToolTip.visible: bluetoothHover.containsMouse
                ToolTip.text: parent.monitor && parent.monitor.bluetoothAvailable
                              ? (parent.monitor.bluetoothEnabled ? "Bluetooth eingeschaltet" : "Bluetooth ausgeschaltet")
                              : "Bluetooth nicht verfügbar"
                MouseArea { id: bluetoothHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
            }
        }

    }

    Rectangle {
        anchors.left: parent.left; anchors.right: parent.right
        y: root.collapsedHeight - 1
        height: 1
        color: UiTheme.borderSubtle
        z: 12
    }

    // ── Pull-down quick access ──────────────────────────────────────────────
    // The same tray is available in desktop and AIO mode.  It gives touch
    // users generously sized shortcuts without permanently taking deck space.
    Rectangle {
        id: quickAccessTray
        anchors.left: parent.left
        anchors.right: parent.right
        // Start hidden behind the fixed header, then slide down from it.
        // This gives the interaction the same visual direction as a mobile
        // notification shade without changing the workspace geometry.
        y: root.collapsedHeight - height
           + height * (root.Window.window ? root.Window.window.topBarPullProgress : 0.0)
        height: UiTheme.toolbarPullExtra
        color: "#171c24"
        border.color: UiTheme.borderSubtle
        border.width: 1
        onVisibleChanged: if (!visible) viewMenuPopup.close()
        opacity: Math.min(1.0, (root.Window.window ? root.Window.window.topBarPullProgress : 0.0) * 3.0)
        visible: opacity > 0.01
        z: 5
        clip: true

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: UiTheme.separatorSubtle
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Math.max(24, (parent.width - 880) / 2)
            anchors.top: parent.top
            anchors.topMargin: 10
            text: "WORKSPACE"
            color: UiTheme.textMuted
            font.pixelSize: 9
            font.bold: true
            font.letterSpacing: 1.4
        }

        GridLayout {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: UiTheme.px(18)
            width: Math.min(parent.width - UiTheme.space6 * 2, 880)
            height: Math.max(0, parent.height - UiTheme.px(48))
            columns: 5
            rowSpacing: UiTheme.px(8)
            columnSpacing: UiTheme.px(8)

            Rectangle {
                id: quickRecording
                objectName: "quickRecording"
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                Layout.preferredHeight: 44
                radius: 0
                readonly property var recorder: typeof setRecorder !== "undefined" ? setRecorder : null
                readonly property bool active: recorder ? recorder.recording : false
                readonly property bool stopping: recorder ? recorder.stopping : false
                function duration(seconds) {
                    const value = Math.max(0, Math.floor(seconds || 0))
                    function two(n) { return (n < 10 ? "0" : "") + n }
                    return two(Math.floor(value / 3600)) + ":" + two(Math.floor(value / 60) % 60) + ":" + two(value % 60)
                }
                color: recordingMouse.pressed ? UiTheme.surfaceRaised : UiTheme.panel
                border.color: active ? UiTheme.error : UiTheme.borderSubtle
                Column {
                    anchors.centerIn: parent; spacing: 3
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: quickRecording.stopping ? "SAVING …" : quickRecording.active ? "STOP RECORDING" : "START RECORDING"
                        color: quickRecording.active ? UiTheme.error : UiTheme.textPrimary
                        font.pixelSize: root.sp(10); font.bold: true
                    }
                    Text {
                        objectName: "quickRecordingDuration"
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: quickRecording.recorder && quickRecording.recorder.error ? "RECORDING ERROR"
                            : quickRecording.duration(quickRecording.recorder ? quickRecording.recorder.elapsedSeconds : 0)
                        color: quickRecording.recorder && quickRecording.recorder.error ? UiTheme.error : UiTheme.textSecondary
                        font.family: UiTheme.numericFontFamily; font.pixelSize: root.sp(11)
                    }
                }
                MouseArea {
                    id: recordingMouse; anchors.fill: parent
                    enabled: quickRecording.recorder !== null && !quickRecording.stopping
                    cursorShape: Qt.PointingHandCursor
                    onClicked: quickRecording.recorder.toggleRecording()
                }
                ToolTip.visible: recordingMouse.containsMouse && !!quickRecording.recorder && !!quickRecording.recorder.error
                ToolTip.text: quickRecording.recorder ? quickRecording.recorder.error : ""
            }

            Rectangle {
                objectName: "quickSampler"
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                Layout.preferredHeight: 44
                radius: 0; color: UiTheme.panel; border.color: UiTheme.borderSubtle
                Text { anchors.centerIn: parent; text: "Sampler"; color: UiTheme.textPrimary; font.pixelSize: root.sp(11); font.bold: true }
                MouseArea {
                    anchors.fill: parent
                    onClicked: if (root.Window.window) {
                        root.Window.window.openMainPage("sampler")
                    }
                }
            }

        // ── View toggles ──────────────────────────────────────────────────────
        Rectangle {
            id: viewMenuBtn
            Layout.preferredWidth: 1
            Layout.fillWidth: true
            Layout.fillHeight: true
                Layout.preferredHeight: 44
            radius: 0
            border.width: 1
            border.color: UiTheme.borderSubtle
            color: viewBtnMouse.pressed ? UiTheme.surfaceRaised : (viewBtnMouse.containsMouse ? UiTheme.panelRaised : UiTheme.panel)

            Text {
                anchors.centerIn: parent
                text: "View ▾"
                color: UiTheme.textPrimary
                font.pixelSize: root.sp(11)
                font.bold: true
            }

            MouseArea {
                id: viewBtnMouse
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (viewMenuPopup.visible) {
                        viewMenuPopup.close()
                    } else {
                        var p = viewMenuBtn.mapToItem(viewMenuPopup.parent, 0, viewMenuBtn.height + 2)
                        viewMenuPopup.x = p.x - viewMenuPopup.width + viewMenuBtn.width
                        viewMenuPopup.y = p.y
                        viewMenuPopup.open()
                    }
                }
            }
        }

        // ── Anti-Clip ────────────────────────────────────────────────────────
        Rectangle {
            id: antiClipBlock
            Layout.preferredWidth: 1
            Layout.fillWidth: true
            Layout.fillHeight: true
                Layout.preferredHeight: 44
            property bool on: false
            property real gr: deckA ? deckA.gainReduction : 1.0
            radius: 0
            border.width: 1
            border.color: on ? UiTheme.green : UiTheme.borderSubtle
            color: UiTheme.panel


            Column {
                anchors.centerIn: parent
                spacing: 2

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Anti-Clip"
                    color: !antiClipBlock.on ? UiTheme.textDim
                         : antiClipBlock.gr < 0.99 ? UiTheme.textPrimary : UiTheme.green
                    font.pixelSize: root.sp(11); font.bold: true; font.letterSpacing: 0.2
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 16; height: 2
                    color: !antiClipBlock.on ? UiTheme.separatorSubtle
                         : antiClipBlock.gr < 0.5  ? UiTheme.error
                         : antiClipBlock.gr < 0.7  ? UiTheme.orange
                         : antiClipBlock.gr < 0.99 ? UiTheme.warning
                         : UiTheme.play
                }
            }

            MouseArea {
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    antiClipBlock.on = !antiClipBlock.on
                    if (deckA) deckA.setAntiClip(antiClipBlock.on)
                }
            }
        }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                Layout.preferredHeight: 44
                radius: 0
                border.width: 1
                border.color: UiTheme.borderSubtle
                color: UiTheme.buttonBg(false, false, quickModeMouse.pressed)
                Text {
                    anchors.centerIn: parent
                    text: root.Window.window && root.Window.window.allInOneMode ? "AIO MODE" : "DESKTOP MODE"
                    color: UiTheme.blue; font.pixelSize: root.sp(11); font.bold: true
                }
                MouseArea {
                    id: quickModeMouse; anchors.fill: parent
                    onClicked: if (root.Window.window) {
                                   root.Window.window.setAllInOneMode(!root.Window.window.allInOneMode)
                                   root.Window.window.closeTopBarPullDown()
                               }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                Layout.preferredHeight: 44
                radius: 0
                border.width: 1
                border.color: UiTheme.borderSubtle
                color: UiTheme.buttonBg(false, false, quickLibraryMouse.pressed)
                Text {
                    anchors.centerIn: parent
                    text: "Library"
                    color: UiTheme.textPrimary; font.pixelSize: root.sp(11); font.bold: true
                }
                MouseArea {
                    id: quickLibraryMouse; anchors.fill: parent
                    onClicked: if (root.Window.window) {
                                   root.Window.window.toggleAllInOneLibrary()
                                   root.Window.window.closeTopBarPullDown()
                               }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                Layout.preferredHeight: 44
                radius: 0
                border.width: 1
                border.color: UiTheme.borderSubtle
                color: UiTheme.buttonBg(false, false, quickSourceMouse.pressed)
                Text {
                    anchors.centerIn: parent
                    text: "Source"
                    color: UiTheme.textPrimary; font.pixelSize: root.sp(11); font.bold: true
                }
                MouseArea {
                    id: quickSourceMouse; anchors.fill: parent
                    onClicked: if (root.Window.window && root.Window.window.toggleLibrarySourceView) {
                                   root.Window.window.toggleLibrarySourceView()
                               }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                Layout.preferredHeight: 44
                radius: 0
                border.width: 1
                border.color: UiTheme.borderSubtle
                color: UiTheme.buttonBg(false, false, quickPerformanceMouse.pressed)
                Text {
                    anchors.centerIn: parent
                    text: "Performance"
                    color: UiTheme.textPrimary; font.pixelSize: root.sp(11); font.bold: true
                }
                MouseArea {
                    id: quickPerformanceMouse; anchors.fill: parent
                    onClicked: if (root.Window.window) {
                                   root.Window.window.activeMainTab = "performance"
                                   root.Window.window.closeTopBarPullDown()
                               }
                }
            }

            Rectangle {
                id: quickSettingsBlock
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                Layout.preferredHeight: 44
                radius: 0
                border.width: 1
                border.color: UiTheme.borderSubtle
                readonly property bool active: root.Window.window ? root.Window.window.settingsPanelActive : false
                color: UiTheme.buttonBg(quickSettingsBlock.active, false, quickSettingsMouse.pressed)
                Text {
                    anchors.centerIn: parent
                    text: "Settings"
                    color: quickSettingsBlock.active ? UiTheme.blue : UiTheme.textPrimary
                    font.pixelSize: root.sp(11); font.bold: true
                }
                MouseArea {
                    id: quickSettingsMouse; anchors.fill: parent
                    onClicked: {
                        if (!root.Window.window)
                            return
                        if (root.Window.window.toggleAllInOneSettings
                                && root.Window.window.toggleAllInOneSettings()) {
                            root.Window.window.closeTopBarPullDown()
                            return
                        }
                        root.showStandaloneSettings()
                        root.Window.window.closeTopBarPullDown()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                Layout.preferredHeight: 44
                radius: 0
                border.width: 1
                border.color: UiTheme.borderSubtle
                color: UiTheme.buttonBg(false, false, quickFullscreenMouse.pressed)
                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Text { text: "⛶"; color: UiTheme.textSecondary; font.pixelSize: root.sp(15); anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "Fullscreen"; color: UiTheme.textPrimary; font.pixelSize: root.sp(11); font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea {
                    id: quickFullscreenMouse
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (!root.Window.window)
                            return
                        if (root.Window.window.visibility === Window.FullScreen)
                            root.Window.window.showNormal()
                        else
                            root.Window.window.showFullScreen()
                        root.Window.window.closeTopBarPullDown()
                    }
                }
            }
        }
    }

    component PullDownHandle: Item {
        id: grip
        required property var controller
        property real travel: 144
        property real restingY: 38
        property color idleColor: "#738094"
        property color activeColor: "#dce5f0"
        anchors.horizontalCenter: parent.horizontalCenter
        y: restingY + travel * (controller ? controller.topBarPullProgress : 0)
        width: 120
        height: 24
        z: 30

        Rectangle {
            anchors.centerIn: parent
            width: gripMouse.pressed || gripMouse.containsMouse ? 54 : 42
            height: 4
            radius: 0
            color: gripMouse.pressed || gripMouse.containsMouse ? grip.activeColor : grip.idleColor
            Behavior on width { NumberAnimation { duration: 120 } }
            Behavior on color { ColorAnimation { duration: 120 } }
        }

        MouseArea {
            id: gripMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            preventStealing: true
            property real pressY: 0
            property real pressProgress: 0
            property real maxDistance: 0

            function followPointer(mouse) {
                // The grip moves with the tray: measure against its fixed parent.
                const point = mapToItem(grip.parent, mouse.x, mouse.y)
                const delta = point.y - pressY
                maxDistance = Math.max(maxDistance, Math.abs(delta))
                grip.controller.topBarPullProgress = Math.max(0, Math.min(1,
                    pressProgress + delta / Math.max(1, grip.travel)))
            }
            onPressed: (mouse) => {
                if (!grip.controller) return
                pressY = mapToItem(grip.parent, mouse.x, mouse.y).y
                pressProgress = grip.controller.topBarPullProgress
                maxDistance = 0
                grip.controller.topBarPullDragging = true
            }
            onPositionChanged: (mouse) => {
                if (pressed && grip.controller) followPointer(mouse)
            }
            onReleased: (mouse) => {
                if (!grip.controller) return
                followPointer(mouse)
                grip.controller.topBarPullDragging = false
                if (maxDistance < 5)
                    grip.controller.toggleTopBarPullDown()
                else if (grip.controller.topBarPullProgress >= 0.5)
                    grip.controller.openTopBarPullDown()
                else
                    grip.controller.closeTopBarPullDown()
            }
            onCanceled: {
                if (!grip.controller) return
                grip.controller.topBarPullDragging = false
                if (grip.controller.topBarPullProgress >= 0.5)
                    grip.controller.openTopBarPullDown()
                else
                    grip.controller.closeTopBarPullDown()
            }
        }
    }

    PullDownHandle {
        id: pullHandle
        controller: root.Window.window
        travel: UiTheme.toolbarPullExtra
        restingY: root.collapsedHeight - height / 2
        idleColor: UiTheme.textDim
        activeColor: UiTheme.textPrimary
    }

    // ════════════════════════════════════════════════════════════════════════
    // CENTER OVERLAY — VU meter + beat indicators
    // (rendered on top of the RowLayout, horizontally centered)
    // ════════════════════════════════════════════════════════════════════════
    Rectangle {
        id: centerMeter
        parent: meterSlot
        anchors.centerIn: parent
        z: 11
        width: meterSlot.width
        height: 28
        radius: 0
        color: "#0e131b"
        border.width: 1
        border.color: clipHold > 0 ? UiTheme.error : UiTheme.borderSubtle

        // MASTER 1/2 peak after master gain, before the protective limiter.
        // Never reconstruct this from one or more channel meters.
        property real levelL: deckA ? deckA.masterVuLevelL : 0
        property real levelR: deckA ? deckA.masterVuLevelR : 0
        property bool clipNow: (deckA && deckA.clipDetected) || (deckB && deckB.clipDetected)
        property real displayL: 0
        property real displayR: 0
        property int clipHold: 0
        readonly property int segs: 24

        Timer {
            interval: 33
            running: true
            repeat: true
            onTriggered: {
                // Immediate attack, roughly 30 dB/s release; hold overloads 330 ms.
                centerMeter.displayL = Math.max(centerMeter.levelL, centerMeter.displayL * 0.892)
                centerMeter.displayR = Math.max(centerMeter.levelR, centerMeter.displayR * 0.892)
                centerMeter.clipHold = centerMeter.clipNow ? 10 : Math.max(0, centerMeter.clipHold - 1)
            }
        }

        function litSegments(peak) {
            if (peak <= 0.000000001) return 0
            if (peak > 1.001) return segs
            const db = Math.max(-54.0, Math.min(0.0, 20.0 * Math.log10(peak)))
            return Math.ceil((db + 54.0) * (22.0 / 54.0))
        }
        function segColor(i) {
            if (i >= 22) return UiTheme.error
            if (i >= 19) return UiTheme.warning
            if (i >= 14) return UiTheme.cue
            return UiTheme.play
        }

        Column {
            anchors.centerIn: parent
            spacing: 2

            Row {
                spacing: 1
                Repeater {
                    model: centerMeter.segs
                    Rectangle {
                        required property int index
                        width: root.compactBar ? 3 : 5
                        height: 4
                        radius: 0
                        readonly property int litCount: centerMeter.litSegments(centerMeter.displayL)
                        color: index < litCount ? centerMeter.segColor(index) : UiTheme.knobTrack
                    }
                }
            }

            Row {
                spacing: 1
                Repeater {
                    model: centerMeter.segs
                    Rectangle {
                        required property int index
                        width: root.compactBar ? 3 : 5
                        height: 4
                        radius: 0
                        readonly property int litCount: centerMeter.litSegments(centerMeter.displayR)
                        color: index < litCount ? centerMeter.segColor(index) : UiTheme.knobTrack
                    }
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 3
                Repeater {
                    model: 8
                    Rectangle {
                        required property int index
                        width: 7
                        height: 2
                        radius: 0
                        readonly property bool deckABeat: index < 4
                        readonly property int beatIndex: index % 4
                        readonly property var inf: deckABeat ? root.deckBeatInfo(deckA) : root.deckBeatInfo(deckB)
                        color: !inf.valid ? UiTheme.textMuted
                             : (beatIndex + 1) === inf.beatInBar ? (deckABeat ? root.clrA : root.clrB)
                             : (deckABeat ? UiTheme.orangeDim : UiTheme.blueDim)
                    }
                }
            }
        }
    }
}
