import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    color: UiTheme.surface
    // The expanded tray deliberately paints below this fixed-height item as an
    // overlay.  Keeping it unclipped prevents the workspace from being moved.
    clip: false

    // Height is fully controlled by parent layout — no implicitHeight here.
    // The first row remains fixed while the pull-down quick-access tray opens.
    readonly property int collapsedHeight: UiTheme.toolbarHeight

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
        { label: "Development Controls", propertyName: "showDevelopmentControls", defaultOn: true },
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
        height: hidden ? 0 : 26
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
            font.pixelSize: root.sp(9)
            elide: Text.ElideRight
        }

        Rectangle {
            id: togglePill
            width: 24
            height: 12
            radius: 6
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 12
            color: toggleCard.on ? UiTheme.blue : UiTheme.surfaceInset

            Rectangle {
                width: 8
                height: 8
                radius: 4
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

    // ── State ────────────────────────────────────────────────────────────────
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
        var bpm = Number(engine.trackData.bpm)
        if (!isFinite(bpm) || bpm <= 0)
            return { valid: false, beatInBar: 0, barNumber: 0 }
        var pos = engine.getPlayheadPositionAtomic()
        if (pos === undefined || isNaN(pos))
            return { valid: false, beatInBar: 0, barNumber: 0 }
        var sr = Number(engine.trackData.sampleRate)
        if (!isFinite(sr) || sr <= 0) sr = 44100.0
        var first = Number(engine.trackData.firstBeatSample) / sr
        var beatDur = 60.0 / bpm
        var beats = (pos - first) / beatDur
        var beatFloor = Math.floor(beats)
        return {
            valid: true,
            beatInBar: (((beatFloor % 4) + 4) % 4) + 1,
            barNumber: Math.floor(beatFloor / 4) + 1
        }
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
        parent: Overlay.overlay
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0
        background: Rectangle { color: UiTheme.surface; border.color: UiTheme.border; border.width: 1 }

        contentItem: Column {
            spacing: 0

            Rectangle {
                width: latencyPopup.width; height: 28
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
                    width: latencyPopup.width; height: 26
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
                width: latencyPopup.width; height: 28
                color: UiTheme.panelInset
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    text: "CALLBACK PROFILE"
                    color: UiTheme.textLabel; font.pixelSize: root.sp(9); font.bold: true; font.letterSpacing: 0.6
                }
            }

            Rectangle {
                width: latencyPopup.width; height: 28
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
                width: latencyPopup.width; height: 28
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
                width: latencyPopup.width; height: 24
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
                width: latencyPopup.width; height: 24
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
                width: latencyPopup.width; height: 24
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
                    width: latencyPopup.width; height: 24
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
        padding: 0
        width: 210
        background: Rectangle { color: UiTheme.surface; border.color: UiTheme.border; border.width: 1 }

        contentItem: Column {
            spacing: 0

            Rectangle {
                width: viewMenuPopup.width; height: 28
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
                    cardWidth: viewMenuPopup.width
                    defaultOn: modelData.defaultOn
                    hidden: modelData.hidden === true
                    onToggleRequested: root.toggleViewProperty(propertyName)
                }
            }

            Rectangle {
                id: vt_deckMode
                width: viewMenuPopup.width
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
                        color: !vt_deckMode.fourDeck ? UiTheme.blue : UiTheme.surfaceInset
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
                        color: vt_deckMode.fourDeck ? UiTheme.blue : UiTheme.surfaceInset
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
                    cardWidth: viewMenuPopup.width
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

        // ── Branding ─────────────────────────────────────────────────────────
        Item {
            Layout.preferredWidth: brandRow.implicitWidth + root.padH * 2
            Layout.fillHeight: true

            Row {
                id: brandRow
                anchors.centerIn: parent
                spacing: 6

                // Engine-DJ-style left color strip
                Rectangle {
                    width: 2
                    height: parent.height * 0.6
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.accentBlue
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1
                    Text {
                        text: "BROCKDJ"
                        color: UiTheme.textPrimary
                        font.pixelSize: root.sp(10)
                        font.bold: true
                        font.letterSpacing: 1.4
                    }
                    Text {
                        text: "ramsbrock.net"
                        color: UiTheme.textDim
                        font.pixelSize: root.sp(6)
                        font.letterSpacing: 0.3
                    }
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        Rectangle {
            id: sourceButton
            Layout.preferredWidth: 72
            Layout.fillHeight: true
            readonly property bool active: root.Window.window
                                           ? root.Window.window.sourcePanelActive
                                           : false
            color: sourceMouse.pressed ? UiTheme.blueDim
                 : active ? UiTheme.blueDim
                 : (sourceMouse.containsMouse ? UiTheme.surfaceRaised : UiTheme.panel)
            Row {
                anchors.centerIn: parent
                spacing: 5
                Text { text: "⊙"; color: sourceButton.active ? UiTheme.blue : UiTheme.textLabel; font.pixelSize: root.sp(14); anchors.verticalCenter: parent.verticalCenter }
                Text { text: "SOURCE"; color: sourceButton.active ? UiTheme.textPrimary : UiTheme.textSecondary; font.pixelSize: root.sp(8); font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 2; visible: sourceButton.active; color: root.accentBlue }
            MouseArea {
                id: sourceMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.Window.window && root.Window.window.openLibrarySourceView)
                               root.Window.window.openLibrarySourceView()
            }
        }

        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        // ── Primary navigation ───────────────────────────────────────────────
        Rectangle {
            id: libraryButton
            Layout.preferredWidth: 78
            Layout.fillHeight: true
            readonly property bool active: root.Window.window
                                           ? (root.Window.window.allInOneMode
                                              ? root.Window.window.libraryPanelActive
                                              : root.Window.window.showLibrary)
                                           : false
            color: libraryMouse.pressed ? UiTheme.blueDim
                 : active ? UiTheme.blueDim
                 : (libraryMouse.containsMouse ? UiTheme.surfaceRaised : UiTheme.panel)
            Row {
                anchors.centerIn: parent
                spacing: 5
                Text { text: "▤"; color: libraryButton.active ? UiTheme.blue : UiTheme.textLabel; font.pixelSize: root.sp(15); anchors.verticalCenter: parent.verticalCenter }
                Text { text: "LIBRARY"; color: libraryButton.active ? UiTheme.textPrimary : UiTheme.textSecondary; font.pixelSize: root.sp(8); font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 2; visible: libraryButton.active; color: root.accentBlue }
            MouseArea { id: libraryMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: if (root.Window.window) root.Window.window.toggleAllInOneLibrary() }
        }

        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        Rectangle {
            id: searchButton
            Layout.preferredWidth: 74
            Layout.fillHeight: true
            color: searchMouse.pressed ? UiTheme.surfaceRaised : (searchMouse.containsMouse ? UiTheme.panelRaised : UiTheme.panel)
            Row {
                anchors.centerIn: parent
                spacing: 4
                Text { text: "⌕"; color: UiTheme.textLabel; font.pixelSize: root.sp(17); anchors.verticalCenter: parent.verticalCenter }
                Text { text: "SEARCH"; color: UiTheme.textSecondary; font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.3; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea { id: searchMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor }
        }

        // This spacer keeps the navigation left-aligned and moves all mixer and
        // system controls into a single group on the right.
        Item { Layout.fillWidth: true }

        // ── Anti-Clip ────────────────────────────────────────────────────────
        Rectangle {
            id: antiClipBlock
            Layout.preferredWidth: Math.max(44, Math.round(root.btnH * 1.45))
            Layout.fillHeight: true
            property bool on: false
            property real gr: deckA ? deckA.gainReduction : 1.0
            color: !on       ? UiTheme.panel
                 : gr < 0.5  ? "#3d0000"
                 : gr < 0.7  ? "#6b1010"
                 : gr < 0.99 ? "#8a4a00"
                 : "#0a2e0a"

            Column {
                anchors.centerIn: parent
                spacing: 2

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "A-CLP"
                    color: !antiClipBlock.on ? UiTheme.textDim
                         : antiClipBlock.gr < 0.99 ? UiTheme.textPrimary : UiTheme.green
                    font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.3
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 16; height: 2
                    color: !antiClipBlock.on ? UiTheme.separatorSubtle
                         : antiClipBlock.gr < 0.5  ? "#ff3333"
                         : antiClipBlock.gr < 0.7  ? "#ff7733"
                         : antiClipBlock.gr < 0.99 ? "#ffaa00"
                         : "#44aa44"
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

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        // ── Master volume ─────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: root.dialSz + root.padH * 2 + 12
            Layout.fillHeight: true
            color: UiTheme.panel

            Row {
                anchors.centerIn: parent
                spacing: 4

                Text {
                    text: "MST"
                    color: UiTheme.textLabel
                    font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.3
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
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        // ── Headphone cue ────────────────────────────────────────────────────
        Rectangle {
            id: headphoneCueBlock
            Layout.preferredWidth: root.dialSz + root.padH * 2 + 42
            Layout.fillHeight: true
            color: UiTheme.panel

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
                    color: headphoneCueBlock.masterCueOn ? UiTheme.blueDim : UiTheme.surfaceInset
                    border.color: headphoneCueBlock.masterCueOn ? UiTheme.blue : UiTheme.border

                    Text {
                        anchors.centerIn: parent
                        text: "MC"
                        color: headphoneCueBlock.masterCueOn ? UiTheme.blue : UiTheme.textMuted
                        font.pixelSize: root.sp(7)
                        font.bold: true
                        font.letterSpacing: 0.5
                    }

                    HoverHandler { id: masterCueHover; cursorShape: Qt.PointingHandCursor }
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: UiTheme.textPrimary
                        opacity: masterCueHover.hovered && !headphoneCueBlock.masterCueOn ? 0.04 : 0.0
                    }
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
                    text: "MST"
                    color: headphoneCueBlock.masterCueOn ? UiTheme.blue : UiTheme.textMuted
                    font.pixelSize: root.sp(7)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        // ── System monitor — LAT + CPU + RAM ─────────────────────────────────
        Rectangle {
            id: monitorBlock
            Layout.preferredWidth: 106   // fixed — prevents layout jitter as values change
            Layout.fillHeight: true
            color: latMouse.containsMouse ? UiTheme.surfaceRaised : UiTheme.panel
            Behavior on color { ColorAnimation { duration: 100 } }

            readonly property color latClr: root.totalLatencyMs >= 35.0 ? UiTheme.error
                                           : root.totalLatencyMs >= 20.0 ? UiTheme.warning
                                           : UiTheme.sync
            readonly property real cpuVal: sysMonitor ? sysMonitor.cpuUsage : 0
            readonly property real ramVal: sysMonitor ? sysMonitor.ramUsage : 0

            function metricColor(v) {
                return v > 0.8 ? UiTheme.error : v > 0.5 ? UiTheme.warning : UiTheme.textMuted
            }
            function barColor(v, hue) {
                return v > 0.8 ? "#cc3333" : v > 0.5 ? "#bb7700" : hue
            }

            Column {
                anchors.centerIn: parent
                spacing: 2

                // ── Row 1: Latency ────────────────────────────────────────────
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 3

                    Text {
                        text: "LAT"; color: UiTheme.textDim
                        font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.5
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        width: 44
                        text: root.totalLatencyMs > 0 ? root.totalLatencyMs.toFixed(1) + " ms" : "—  ms"
                        color: monitorBlock.latClr
                        font.pixelSize: root.sp(8); font.bold: true; font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: latencyPopup.opened ? "▴" : "▾"
                        color: UiTheme.textDim; font.pixelSize: root.sp(7)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                // ── Row 2: CPU | RAM side-by-side with mini bars ──────────────
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 2

                    Text {
                        text: "C"; color: UiTheme.textMuted
                        font.pixelSize: root.sp(7); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: 18; height: 3; radius: 1; color: UiTheme.knobTrack
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle {
                            width: Math.max(0, Math.round(parent.width * monitorBlock.cpuVal))
                            height: parent.height; radius: 1
                            color: monitorBlock.barColor(monitorBlock.cpuVal, UiTheme.greenDim)
                            Behavior on width { NumberAnimation { duration: 350; easing.type: Easing.OutQuad } }
                        }
                    }
                    Text {
                        width: 18; text: Math.round(monitorBlock.cpuVal * 100) + "%"
                        color: monitorBlock.metricColor(monitorBlock.cpuVal)
                        font.pixelSize: root.sp(7); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Rectangle { width: 1; height: 7; color: UiTheme.separatorSubtle; anchors.verticalCenter: parent.verticalCenter }

                    Text {
                        text: "R"; color: UiTheme.textMuted
                        font.pixelSize: root.sp(7); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: 18; height: 3; radius: 1; color: UiTheme.knobTrack
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle {
                            width: Math.max(0, Math.round(parent.width * monitorBlock.ramVal))
                            height: parent.height; radius: 1
                            color: monitorBlock.barColor(monitorBlock.ramVal, UiTheme.blueDim)
                            Behavior on width { NumberAnimation { duration: 350; easing.type: Easing.OutQuad } }
                        }
                    }
                    Text {
                        width: 18; text: Math.round(monitorBlock.ramVal * 100) + "%"
                        color: monitorBlock.metricColor(monitorBlock.ramVal)
                        font.pixelSize: root.sp(7); font.family: UiTheme.numericFontFamily
                        horizontalAlignment: Text.AlignRight
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            MouseArea {
                id: latMouse
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root.refreshLatencyInfo()
                    if (latencyPopup.opened) { latencyPopup.close(); return }
                    var p = monitorBlock.mapToItem(latencyPopup.parent, 0, monitorBlock.height + 2)
                    latencyPopup.width = 340
                    latencyPopup.x = p.x; latencyPopup.y = p.y
                    latencyPopup.open()
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        // ── Ableton Link ─────────────────────────────────────────────────────
        Rectangle {
            id: linkBlock
            Layout.preferredWidth: 48
            Layout.fillHeight: true
            color: linkMouse.pressed ? UiTheme.greenDim : ((linkManager && linkManager.enabled) ? UiTheme.greenDim : UiTheme.panel)

            readonly property bool on: linkManager && linkManager.enabled
            readonly property int beatIndex: linkManager ? (((Math.floor(linkManager.beat) % 4) + 4) % 4) : 0

            Column {
                anchors.centerIn: parent
                spacing: 1

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 3
                    Rectangle {
                        width: 4; height: 4; radius: 2
                        anchors.verticalCenter: parent.verticalCenter
                        color: linkBlock.on ? UiTheme.play : UiTheme.separatorSubtle
                    }
                    Text {
                        text: "LINK"
                        color: linkBlock.on ? UiTheme.play : UiTheme.textMuted
                        font.pixelSize: root.sp(6)
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
                            radius: 1
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
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        // ── View toggles ──────────────────────────────────────────────────────
        Rectangle {
            id: viewMenuBtn
            Layout.preferredWidth: root.btnH
            Layout.fillHeight: true
            color: viewBtnMouse.pressed ? UiTheme.surfaceRaised : (viewBtnMouse.containsMouse ? UiTheme.panelRaised : UiTheme.panel)

            Column {
                anchors.centerIn: parent
                spacing: 3
                Repeater {
                    model: 3
                    Rectangle { width: 10; height: 1; color: UiTheme.textMuted }
                }
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

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        // ── Clock ─────────────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 52
            Layout.fillHeight: true
            color: UiTheme.panel

            Text {
                id: clockText
                anchors.centerIn: parent
                text: root.currentTime
                color: UiTheme.textSecondary
                font.pixelSize: root.sp(12); font.family: UiTheme.numericFontFamily; font.bold: true
            }
        }

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
        color: UiTheme.surface
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

        RowLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - UiTheme.space6 * 2, 680)
            height: Math.max(0, Math.min(parent.height - UiTheme.space3 * 2, UiTheme.px(56)))
            spacing: UiTheme.space3

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 0
                color: quickModeMouse.pressed ? UiTheme.blueDim : UiTheme.surfaceRaised
                Text {
                    anchors.centerIn: parent
                    text: root.Window.window && root.Window.window.allInOneMode ? "AIO MODE" : "DESKTOP MODE"
                    color: UiTheme.blue; font.pixelSize: root.sp(10); font.bold: true
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
                Layout.fillHeight: true
                radius: 0
                color: quickLibraryMouse.pressed ? UiTheme.blueDim : UiTheme.surfaceRaised
                Text {
                    anchors.centerIn: parent
                    text: "LIBRARY"
                    color: UiTheme.textPrimary; font.pixelSize: root.sp(10); font.bold: true
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
                Layout.fillHeight: true
                radius: 0
                color: quickSourceMouse.pressed ? UiTheme.blueDim : UiTheme.surfaceRaised
                Text {
                    anchors.centerIn: parent
                    text: "SOURCE"
                    color: UiTheme.textPrimary; font.pixelSize: root.sp(10); font.bold: true
                }
                MouseArea {
                    id: quickSourceMouse; anchors.fill: parent
                    onClicked: if (root.Window.window && root.Window.window.openLibrarySourceView) {
                                   root.Window.window.openLibrarySourceView()
                                   root.Window.window.closeTopBarPullDown()
                               }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 0
                color: quickPerformanceMouse.pressed ? UiTheme.blueDim : UiTheme.surfaceRaised
                Text {
                    anchors.centerIn: parent
                    text: "PERFORMANCE"
                    color: UiTheme.textPrimary; font.pixelSize: root.sp(10); font.bold: true
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
                Layout.fillHeight: true
                radius: 0
                readonly property bool active: root.Window.window ? root.Window.window.settingsPanelActive : false
                color: quickSettingsMouse.pressed ? UiTheme.blueDim : (active ? UiTheme.blueDim : UiTheme.surfaceRaised)
                Text {
                    anchors.centerIn: parent
                    text: "SETTINGS"
                    color: quickSettingsBlock.active ? UiTheme.blue : UiTheme.textPrimary
                    font.pixelSize: root.sp(10); font.bold: true
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
                Layout.fillHeight: true
                radius: 0
                color: quickRecMouse.pressed ? "#3a1717" : UiTheme.surfaceRaised
                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Rectangle { width: 7; height: 7; radius: 4; color: "#c84848"; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "REC"; color: UiTheme.textSecondary; font.pixelSize: root.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea {
                    id: quickRecMouse
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    // Recording control is kept as the existing placeholder.
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 0
                color: quickFullscreenMouse.pressed ? UiTheme.blueDim : UiTheme.surfaceRaised
                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Text { text: "⛶"; color: UiTheme.textSecondary; font.pixelSize: root.sp(15); anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "FULLSCREEN"; color: UiTheme.textPrimary; font.pixelSize: root.sp(9); font.bold: true; anchors.verticalCenter: parent.verticalCenter }
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

    // A familiar Android-style handle: drag it down to reveal the tray, or
    // tap it to toggle.  It sits above the content, so it is reachable in both
    // AIO and desktop mode.
    Item {
        id: pullHandle
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: 112
        height: 16
        z: 20

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 54
            height: 4
            radius: 2
            color: pullHandleMouse.pressed ? UiTheme.textPrimary : UiTheme.textLabel
        }

        MouseArea {
            id: pullHandleMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.OpenHandCursor
            preventStealing: true
            property real pressY: 0
            property real pressProgress: 0

            onPressed: function(mouse) {
                pressY = mouse.y
                pressProgress = root.Window.window ? root.Window.window.topBarPullProgress : 0
                cursorShape = Qt.ClosedHandCursor
            }
            onPositionChanged: function(mouse) {
                if (!pressed || !root.Window.window)
                    return
                var next = pressProgress + (mouse.y - pressY) / UiTheme.toolbarPullExtra
                root.Window.window.topBarPullProgress = Math.max(0.0, Math.min(1.0, next))
            }
            onReleased: function(mouse) {
                if (!root.Window.window)
                    return
                var moved = Math.abs(mouse.y - pressY)
                if (moved < 4)
                    root.Window.window.toggleTopBarPullDown()
                else if (root.Window.window.topBarPullProgress >= 0.35)
                    root.Window.window.openTopBarPullDown()
                else
                    root.Window.window.closeTopBarPullDown()
                cursorShape = Qt.OpenHandCursor
            }
            onCanceled: cursorShape = Qt.OpenHandCursor
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // CENTER OVERLAY — VU meter + beat indicators
    // (rendered on top of the RowLayout, horizontally centered)
    // ════════════════════════════════════════════════════════════════════════
    Rectangle {
        id: centerMeter
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: Math.round((root.collapsedHeight - height) / 2) - 5
        z: 11
        width: 136
        height: 22
        radius: 0
        color: UiTheme.displayBackground
        border.width: 1
        border.color: clipNow ? UiTheme.error : UiTheme.border

        // Final MASTER 1/2 peak: after summed deck mix, master gain and limiter.
        // Never reconstruct this from one or more channel meters.
        property real levelL: deckA ? deckA.masterVuLevelL : 0
        property real levelR: deckA ? deckA.masterVuLevelR : 0
        property bool clipNow: (deckA && deckA.clipDetected) || (deckB && deckB.clipDetected)
        readonly property int segs: 24

        function litSegments(peak) {
            if (peak <= 0.000000001) return 0
            const db = Math.max(-54.0, Math.min(12.0, 20.0 * Math.log10(peak)))
            return Math.ceil((db + 54.0) * (segs / 66.0))
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
                spacing: 2
                Repeater {
                    model: centerMeter.segs
                    Rectangle {
                        required property int index
                        width: 4
                        height: 3
                        radius: 1
                        readonly property int litCount: centerMeter.litSegments(centerMeter.levelL)
                        color: index < litCount ? centerMeter.segColor(index) : UiTheme.knobTrack
                    }
                }
            }

            Row {
                spacing: 2
                Repeater {
                    model: centerMeter.segs
                    Rectangle {
                        required property int index
                        width: 4
                        height: 3
                        radius: 1
                        readonly property int litCount: centerMeter.litSegments(centerMeter.levelR)
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
                        radius: 1
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
