import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    color: "#121212"
    clip: true
    
    // Height is FULLY controlled by parent layout (main.qml Layout.minimumHeight/preferredHeight/maximumHeight)
    // NO implicitHeight to avoid competing bindings!
    
    // ── FIXED CONSTANTS (no binding to root.height to avoid cascading jitter) ──
    readonly property int verticalPad: 0
    readonly property int horizontalPadding: 8
    readonly property int accentBarWidth: Math.max(2, Math.round(buttonHeight * 0.06))
    readonly property int accentBarHeight: Math.max(12, Math.round(buttonHeight * 0.42))
    readonly property int masterDialSize: Math.max(20, Math.round(buttonHeight * 0.65))
    readonly property int antiClipButtonWidth: Math.max(40, Math.round(buttonHeight * 1.28))
    readonly property int buttonSpacing: 3
    readonly property int linkToggleWidth: Math.max(36, Math.round(buttonHeight * 1.1))
    readonly property int linkPeerWidth: Math.max(16, Math.round(buttonHeight * 0.55))
    readonly property int linkBpmWidth: Math.max(48, Math.round(buttonHeight * 1.5))
    readonly property int meterLabelWidth: Math.max(22, Math.round(buttonHeight * 0.7))
    readonly property int meterBarWidth: Math.max(52, Math.round(buttonHeight * 1.7))
    readonly property int smallButtonWidth: Math.max(36, Math.round(buttonHeight * 1.1))
    readonly property int latencyButtonWidth: Math.max(88, Math.round(buttonHeight * 2.7))
    readonly property int meterDotSize: Math.max(6, Math.round(buttonHeight * 0.2))
    readonly property int vuBarWidth: Math.max(100, Math.round(buttonHeight * 3.5))
    readonly property int vuBarHeight: Math.max(5, Math.round(buttonHeight * 0.18))
    readonly property int miniBarHeight: Math.max(4, Math.round(buttonHeight * 0.12))
    readonly property int clipInfoWidth: Math.max(36, Math.round(buttonHeight * 1.15))
    readonly property int logoTitlePx: Math.min(root.sp(14), Math.max(11, Math.round(buttonHeight * 0.42)))
    readonly property int logoSubPx: Math.min(root.sp(8), Math.max(7, Math.round(buttonHeight * 0.23)))
    readonly property int recTextPx: Math.min(root.sp(9), Math.max(8, Math.round(buttonHeight * 0.27)))
    readonly property int iconButtonPx: Math.min(root.sp(13), Math.max(10, Math.round(buttonHeight * 0.4)))
    readonly property int sectionHeight: Math.max(22, buttonHeight - 4)
    readonly property int sectionRadius: 0
    readonly property int sectionPadding: 6
    readonly property color deckAColor: "#ff9900"
    readonly property color deckBColor: "#00bfff"
    readonly property int beatDotSize: Math.max(6, Math.round(buttonHeight * 0.2))
    property int beatUiTick: 0
    property real totalLatencyMs: 0.0
    property var latencyRows: []
    
    // Buttons should match the visible bar height exactly (pixel-snapped).
    readonly property int buttonHeight: Math.max(1, Math.round(root.height - (root.verticalPad * 2)))
    
    // Throttle resizing to avoid feedback loops
    property int lastReportedWidth: width
    property int resizeThrottle: 0
    
    onWidthChanged: {
        // Debounce width changes to avoid layout thrashing
        resizeThrottle++
        if (resizeThrottle > 3) {
            lastReportedWidth = width
            resizeThrottle = 0
        }
    }

    function sp(px) {
        // TopHeader has a fixed bar height of 34px. Do not scale fonts based on window height,
        // otherwise they will blow up and clip. Return the exact pixel value.
        return px;
    }

    function refreshLatencyInfo() {
        if (!deckA || !deckA.latencyBreakdown)
            return

        var rows = deckA.latencyBreakdown()
        if (!rows || rows.length === 0)
            return

        latencyRows = rows

        var sum = 0.0
        for (var i = 0; i < rows.length; ++i) {
            var ms = Number(rows[i].ms)
            if (!isNaN(ms) && isFinite(ms))
                sum += ms
        }
        totalLatencyMs = sum
    }

    property string currentTime: "00:00"
    
    Timer {
        interval: 1000; running: true; repeat: true
        onTriggered: {
            var date = new Date();
            var h = date.getHours().toString().padStart(2, '0');
            var m = date.getMinutes().toString().padStart(2, '0');
            root.currentTime = h + ":" + m;
        }
    }

    Timer {
        id: latencyPollTimer
        interval: 350
        running: true
        repeat: true
        onTriggered: root.refreshLatencyInfo()
    }
    
    Component.onCompleted: {
        var date = new Date();
        var h = date.getHours().toString().padStart(2, '0');
        var m = date.getMinutes().toString().padStart(2, '0');
        root.currentTime = h + ":" + m;
        root.refreshLatencyInfo()
    }

    function deckBeatInfo(engine) {
        // Dependency so expressions update continuously while transport runs.
        var _tick = root.beatUiTick
        if (!engine || !engine.trackData || !engine.trackData.isBpmAnalyzed)
            return { valid: false, beatInBar: 0, barNumber: 0, phase: 0.0 }

        // Beat/bar position in the SONG must stay tied to the analyzed beatgrid,
        // not to transport tempo changes (tempo fader, sync, link publishing).
        var bpm = Number(engine.trackData.bpm)
        if (!isFinite(bpm) || bpm <= 0)
            return { valid: false, beatInBar: 0, barNumber: 0, phase: 0.0 }

        var playheadSec = engine.getPlayheadPositionAtomic()
        if (playheadSec === undefined || isNaN(playheadSec))
            return { valid: false, beatInBar: 0, barNumber: 0, phase: 0.0 }

        var sampleRate = Number(engine.trackData.sampleRate)
        if (!isFinite(sampleRate) || sampleRate <= 0)
            sampleRate = 44100.0
        var firstBeatSec = Number(engine.trackData.firstBeatSample) / sampleRate

        var beatDur = 60.0 / bpm
        var beatsFromAnchor = (playheadSec - firstBeatSec) / beatDur
        var beatFloor = Math.floor(beatsFromAnchor)
        var beatInBar = ((beatFloor % 4) + 4) % 4
        var barNumber = Math.floor(beatFloor / 4) + 1
        if (barNumber < 1)
            barNumber = 1

        var phase = beatsFromAnchor - beatFloor
        if (phase < 0)
            phase += 1.0

        return {
            valid: true,
            beatInBar: beatInBar,
            barNumber: barNumber,
            phase: phase
        }
    }

    Timer {
        id: beatCounterTimer
        interval: 50
        repeat: true
        running: true
        onTriggered: root.beatUiTick++
    }

    // Settings window — created once, shown/hidden on demand
    SettingsWindow {
        id: settingsWin
    }

    Popup {
        id: latencyPopup
        parent: Overlay.overlay
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0

        background: Rectangle {
            color: "#121212"
            border.color: "#2a2a2a"
            border.width: 1
            radius: 0
        }

        contentItem: Column {
            spacing: 0

            Rectangle {
                width: latencyPopup.width
                height: 26
                color: "#1a1a1a"

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    text: "Latency Breakdown"
                    color: "#d6d6d6"
                    font.pixelSize: root.sp(9)
                    font.bold: true
                    font.letterSpacing: 0.4
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    text: root.totalLatencyMs.toFixed(1) + " ms"
                    color: "#9ecbff"
                    font.pixelSize: root.sp(9)
                    font.family: "monospace"
                    font.bold: true
                }
            }

            Repeater {
                model: root.latencyRows

                Rectangle {
                    required property var modelData
                    required property int index
                    width: latencyPopup.width
                    height: 28
                    color: index % 2 === 0 ? "#141414" : "#171717"

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 8

                        Text {
                            width: 170
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: "#bbbbbb"
                            font.pixelSize: root.sp(8)
                            elide: Text.ElideRight
                        }

                        Text {
                            width: 54
                            anchors.verticalCenter: parent.verticalCenter
                            text: Number(modelData.ms).toFixed(1) + " ms"
                            color: "#f2f2f2"
                            font.pixelSize: root.sp(8)
                            font.family: "monospace"
                            horizontalAlignment: Text.AlignRight
                        }

                        Text {
                            width: 66
                            anchors.verticalCenter: parent.verticalCenter
                            text: Number(modelData.samples).toString() + " smp"
                            color: "#888"
                            font.pixelSize: root.sp(8)
                            font.family: "monospace"
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.topMargin: root.verticalPad
        anchors.bottomMargin: root.verticalPad
        anchors.leftMargin: root.horizontalPadding
        anchors.rightMargin: root.horizontalPadding
        spacing: 0

        // ── LEFT: Software name ───────────────────────────────────────────────
        Row {
            spacing: 6
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignVCenter
            height: root.buttonHeight

            // Small coloured accent bar (Traktor-style)
            Rectangle {
                width: root.accentBarWidth
                height: root.accentBarHeight
                radius: 0
                color: "#1e90ff"
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: "#1e90ff" }
                    GradientStop { position: 1.0; color: "#0050cc" }
                }
            }

            Column {
                spacing: 0
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    text: "DJ-Software"
                    color: "#ffffff"
                    font.pixelSize: root.logoTitlePx
                    font.bold: true
                    font.letterSpacing: 1.5
                }
                Text {
                    text: "by Ramsbrock.net"
                    color: "#555"
                    font.pixelSize: root.logoSubPx
                    font.letterSpacing: 0.5
                }
            }
        }

        // ── ABLETON LINK section ──────────────────────────────────────────────
        Row {
            spacing: 0
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignVCenter
            Layout.leftMargin: 12
            height: root.buttonHeight

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                height: root.sectionHeight
                width: linkPanelRow.implicitWidth + root.sectionPadding * 2
                radius: 0
                color: (linkManager && linkManager.enabled) ? "#182a20" : "#171717"
                border.width: 0

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (linkManager) linkManager.enabled = !linkManager.enabled
                }

                Row {
                    id: linkPanelRow
                    anchors.centerIn: parent
                    spacing: 8

                    Text {
                        width: root.linkToggleWidth
                        anchors.verticalCenter: parent.verticalCenter
                        text: "LINK"
                        color: (linkManager && linkManager.enabled) ? "#7df5a4" : "#848484"
                        font.pixelSize: root.sp(9)
                        font.bold: true
                        font.letterSpacing: 0.4
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        width: root.linkPeerWidth
                        anchors.verticalCenter: parent.verticalCenter
                        text: linkManager ? linkManager.numPeers.toString() : "0"
                        color: (linkManager && linkManager.numPeers > 0) ? "#52d77e" : "#6d6d6d"
                        font.pixelSize: root.sp(9)
                        font.family: "monospace"
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        width: root.linkBpmWidth
                        anchors.verticalCenter: parent.verticalCenter
                        opacity: (linkManager && linkManager.enabled) ? 1.0 : 0.45
                        text: linkManager ? linkManager.bpm.toFixed(1) : "120.0"
                        color: "#d2d2d2"
                        font.pixelSize: root.sp(11)
                        font.family: "monospace"
                        font.bold: true
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                    }

                    Row {
                        spacing: 3
                        anchors.verticalCenter: parent.verticalCenter
                        opacity: (linkManager && linkManager.enabled) ? 1.0 : 0.28

                        Repeater {
                            model: 4
                            Rectangle {
                                required property int index
                                width: root.meterDotSize
                                height: root.meterDotSize
                                radius: width / 2
                                color: {
                                    if (!linkManager || !linkManager.enabled) return "#222"
                                    var beatFloor = Math.floor(linkManager.beat)
                                    var beatIndex = ((beatFloor % 4) + 4) % 4
                                    return index === beatIndex ? "#44cc66" : "#222"
                                }
                                border.width: 0
                            }
                        }
                    }
                }
            }
        }

        // ── SPACER (split UI in half) ─────────────────────────────────────────
        Item { Layout.fillWidth: true }

        // ── RIGHT: CPU/RAM + REC + Clock + Actions ────────────────────────────
        Row {
            spacing: 14
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignVCenter
            height: root.buttonHeight

            // Anti-Clip button
            Rectangle {
                width: root.antiClipButtonWidth
                height: root.buttonHeight
                radius: 0
                property bool antiClipActive: false
                property real gr: deckA ? deckA.gainReduction : 1.0

                color: !antiClipActive ? "#1a1a1a" : (gr < 0.5 ? "#5c0000" : (gr < 0.7 ? "#b71c1c" : (gr < 0.99 ? "#f57f17" : "#1b5e20")))
                border.color: !antiClipActive ? "#333" : (gr < 0.5 ? "#8e0000" : (gr < 0.7 ? "#f44336" : (gr < 0.99 ? "#fbc02d" : "#4caf50")))
                border.width: 0

                Text {
                    anchors.centerIn: parent
                    text: "A-CLIP"
                    color: !parent.antiClipActive ? "#666" : (parent.gr < 0.99 ? "#fff" : "#c8e6c9")
                    font.pixelSize: root.sp(8)
                    font.bold: true
                    font.family: "monospace"
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        parent.antiClipActive = !parent.antiClipActive
                        if (deckA) deckA.setAntiClip(parent.antiClipActive)
                    }
                }
            }

            // Master Volume knob
            Rectangle {
                width: root.masterDialSize + 28
                height: root.buttonHeight
                anchors.verticalCenter: parent.verticalCenter
                radius: 0
                color: "#171717"
                border.width: 0

                Row {
                    anchors.centerIn: parent
                    spacing: 5

                    Text {
                        text: "MST"
                        color: "#767676"
                        font.pixelSize: root.sp(8)
                        font.bold: true
                        font.family: "monospace"
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Dial {
                        id: masterVolDial
                        width: root.masterDialSize
                        height: root.masterDialSize
                        from: 0.0; to: 1.0; value: 0.8

                    background: Rectangle {
                        x: masterVolDial.width / 2 - width / 2
                        y: masterVolDial.height / 2 - height / 2
                        width: masterVolDial.width
                        height: masterVolDial.height
                        radius: width / 2
                        color: "transparent"

                        Canvas {
                            id: masterArc
                            anchors.fill: parent
                            antialiasing: true
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()

                                var cx = width / 2
                                var cy = height / 2
                                var radius = Math.min(width, height) * 0.44
                                var startDeg = 120
                                var spanDeg = 300
                                var norm = Math.max(0, Math.min(1, (masterVolDial.value - masterVolDial.from) / (masterVolDial.to - masterVolDial.from)))

                                ctx.lineWidth = Math.max(1, Math.round(width * 0.06))
                                ctx.lineCap = "round"
                                ctx.strokeStyle = "#5f6368"
                                ctx.beginPath()
                                ctx.arc(cx, cy, radius, startDeg * Math.PI / 180, (startDeg + norm * spanDeg) * Math.PI / 180, false)
                                ctx.stroke()
                            }

                            Connections {
                                target: masterVolDial
                                function onValueChanged() { masterArc.requestPaint() }
                            }
                        }

                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width * 0.85
                            height: parent.height * 0.85
                            radius: width / 2
                            color: "#222"
                            border.color: "#444"
                            border.width: 0
                        }
                    }

                    handle: Rectangle {
                        id: mstHandle
                        x: masterVolDial.background.x + masterVolDial.background.width / 2 - width / 2
                        y: masterVolDial.background.y + masterVolDial.background.height / 2 - height / 2
                        width: masterVolDial.width * 0.85
                        height: masterVolDial.height * 0.85
                        color: "transparent"

                        Rectangle {
                            color: "#aaa"
                            width: 2
                            height: parent.height * 0.48
                            radius: 0
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: -2
                        }

                        transform: [
                            Rotation {
                                angle: masterVolDial.angle
                                origin.x: mstHandle.width / 2
                                origin.y: mstHandle.height / 2
                            }
                        ]
                    }

                        onValueChanged: {
                            if (deckA) deckA.setMasterVolume(value)
                        }

                        TapHandler {
                            onDoubleTapped: {
                                masterVolDial.enabled = false
                                masterVolDial.value = 0.8
                                masterVolDial.enabled = true
                            }
                        }
                    }
                }
            }

            // CPU / RAM bars
            Rectangle {
                id: latencyButton
                width: root.latencyButtonWidth
                height: root.buttonHeight
                anchors.verticalCenter: parent.verticalCenter
                radius: 0
                color: latencyMouse.pressed ? "#232323" : "#171717"
                border.width: 0

                readonly property color latencyColor: root.totalLatencyMs >= 35.0 ? "#ff6f61"
                                                     : root.totalLatencyMs >= 20.0 ? "#ffd166"
                                                     : "#9ecbff"

                Row {
                    anchors.centerIn: parent
                    spacing: 5

                    Text {
                        text: "LAT"
                        color: "#7a7a7a"
                        font.pixelSize: root.sp(8)
                        font.bold: true
                        font.family: "monospace"
                    }

                    Text {
                        text: root.totalLatencyMs > 0 ? root.totalLatencyMs.toFixed(1) + "ms" : "--"
                        color: latencyButton.latencyColor
                        font.pixelSize: root.sp(10)
                        font.bold: true
                        font.family: "monospace"
                    }

                    Text {
                        text: latencyPopup.opened ? "▴" : "▾"
                        color: "#777"
                        font.pixelSize: root.sp(8)
                        font.bold: true
                    }
                }

                MouseArea {
                    id: latencyMouse
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.refreshLatencyInfo()

                        if (latencyPopup.opened) {
                            latencyPopup.close()
                            return
                        }

                        var p = latencyButton.mapToItem(latencyPopup.parent, 0, latencyButton.height + 4)
                        latencyPopup.width = 320
                        latencyPopup.x = p.x
                        latencyPopup.y = p.y
                        latencyPopup.open()
                    }
                }
            }

            // CPU / RAM bars
            Rectangle {
                width: root.meterLabelWidth + root.meterBarWidth + 16
                height: root.buttonHeight
                anchors.verticalCenter: parent.verticalCenter
                radius: 0
                color: "#171717"
                border.width: 0

                Column {
                    anchors.centerIn: parent
                    spacing: 2

                    // CPU bar
                    Row {
                        spacing: 3
                        Text {
                            text: "CPU"
                            color: "#555"
                            font.pixelSize: root.sp(7)
                            font.bold: true
                            width: root.meterLabelWidth
                        }
                        Rectangle {
                            width: root.meterBarWidth
                            height: root.miniBarHeight
                            color: "#0d0d0d"
                            border.width: 0
                            radius: 0

                            Rectangle {
                                width: (sysMonitor ? sysMonitor.cpuUsage : 0) * parent.width
                                height: parent.height; radius: 0
                                color: (sysMonitor && sysMonitor.cpuUsage > 0.8) ? "#e53935"
                                     : (sysMonitor && sysMonitor.cpuUsage > 0.5) ? "#fdd835" : "#2e7d32"
                            }
                        }
                    }

                    // RAM bar
                    Row {
                        spacing: 3
                        Text {
                            text: "RAM"
                            color: "#555"
                            font.pixelSize: root.sp(7)
                            font.bold: true
                            width: root.meterLabelWidth
                        }
                        Rectangle {
                            width: root.meterBarWidth
                            height: root.miniBarHeight
                            color: "#0d0d0d"
                            border.width: 0
                            radius: 0

                            Rectangle {
                                width: (sysMonitor ? sysMonitor.ramUsage : 0) * parent.width
                                height: parent.height; radius: 0
                                color: (sysMonitor && sysMonitor.ramUsage > 0.8) ? "#e53935"
                                     : (sysMonitor && sysMonitor.ramUsage > 0.5) ? "#fdd835" : "#2e7d32"
                            }
                        }
                    }
                }
            }

            // REC button
            Rectangle {
                width: root.smallButtonWidth
                height: root.buttonHeight
                color: "#1a1a1a"
                border.color: "#333"
                border.width: 0
                radius: 0

                Row {
                    anchors.centerIn: parent
                    spacing: 4

                    Rectangle {
                        width: root.meterDotSize
                        height: root.meterDotSize
                        radius: width / 2
                        color: "#aa3333"
                    }
                    Text {
                        text: "REC"
                        color: "#777"
                        font.pixelSize: root.recTextPx
                        font.bold: true
                    }
                }
            }

            // Clock
            Rectangle {
                width: root.linkBpmWidth + 10
                height: root.buttonHeight
                anchors.verticalCenter: parent.verticalCenter
                radius: 0
                color: "#171717"
                border.width: 0

                Text {
                    anchors.centerIn: parent
                    text: root.currentTime
                    color: "#d0d0d0"
                    font.pixelSize: root.sp(12)
                    font.family: "monospace"
                    font.bold: true
                }
            }

            // UI action buttons
            Row {
                spacing: 3
                height: root.buttonHeight

                Rectangle {
                    width: root.buttonHeight
                    height: root.buttonHeight
                    color: fullScreenMouse.pressed ? "#333" : "#1e1e1e"
                    border.color: "#333"
                    border.width: 0
                    radius: 0

                    Text {
                        anchors.centerIn: parent
                        text: "⛶"
                        color: "#aaa"
                        font.pixelSize: root.iconButtonPx
                    }

                    MouseArea {
                        id: fullScreenMouse
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (root.Window.window.visibility === Window.FullScreen)
                                root.Window.window.showNormal()
                            else
                                root.Window.window.showFullScreen()
                        }
                    }
                }

                Rectangle {
                    width: root.buttonHeight
                    height: root.buttonHeight
                    color: settingsMouse.pressed ? "#333" : "#1e1e1e"
                    border.color: "#333"
                    border.width: 0
                    radius: 0

                    Text {
                        anchors.centerIn: parent
                        text: "⚙"
                        color: "#aaa"
                        font.pixelSize: root.iconButtonPx
                    }

                    MouseArea {
                        id: settingsMouse
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            settingsWin.show()
                            settingsWin.raise()
                            settingsWin.requestActivate()
                        }
                    }
                }
            }
        }
    }

    // ── VU METER OVERLAY (centered, horizontal bars with Peak Hold + Decay) ──
    Row {
        anchors.centerIn: parent
        spacing: 8

        // CDJ-like beat/bar counters (A orange, B blue), left of VU meter.
        Rectangle {
            width: 96
            height: root.buttonHeight
            radius: 0
            color: "#161616"
            border.width: 0

            Column {
                anchors.centerIn: parent
                spacing: 3

                Row {
                    spacing: 4
                    Text {
                        width: 10
                        text: "A"
                        color: root.deckAColor
                        font.pixelSize: root.sp(8)
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Repeater {
                        model: 4
                        Rectangle {
                            required property int index
                            readonly property var info: root.deckBeatInfo(deckA)
                            width: root.beatDotSize
                            height: root.beatDotSize
                            radius: width / 2
                            color: {
                                if (!info.valid)
                                    return index === 0 ? "#5a3a16" : "#292929"
                                if (index === info.beatInBar)
                                    return "#ffb84d"
                                return index === 0 ? "#734819" : "#292929"
                            }
                        }
                    }

                    Text {
                        readonly property var info: root.deckBeatInfo(deckA)
                        text: info.valid ? ("B" + info.barNumber) : "B--"
                        color: "#9d9d9d"
                        font.pixelSize: root.sp(7)
                        font.family: "monospace"
                    }
                }

                Row {
                    spacing: 4
                    Text {
                        width: 10
                        text: "B"
                        color: root.deckBColor
                        font.pixelSize: root.sp(8)
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Repeater {
                        model: 4
                        Rectangle {
                            required property int index
                            readonly property var info: root.deckBeatInfo(deckB)
                            width: root.beatDotSize
                            height: root.beatDotSize
                            radius: width / 2
                            color: {
                                if (!info.valid)
                                    return index === 0 ? "#18435b" : "#292929"
                                if (index === info.beatInBar)
                                    return "#55d2ff"
                                return index === 0 ? "#1a5b79" : "#292929"
                            }
                        }
                    }

                    Text {
                        readonly property var info: root.deckBeatInfo(deckB)
                        text: info.valid ? ("B" + info.barNumber) : "B--"
                        color: "#9d9d9d"
                        font.pixelSize: root.sp(7)
                        font.family: "monospace"
                    }
                }
            }
        }

        Column {
            id: vuColumn
            spacing: 4
            anchors.verticalCenter: parent.verticalCenter

            property real levelL: {
                var a = deckA ? deckA.vuLevelL : 0
                var b = deckB ? deckB.vuLevelL : 0
                return Math.max(a, b)
            }
            property real levelR: {
                var a = deckA ? deckA.vuLevelR : 0
                var b = deckB ? deckB.vuLevelR : 0
                return Math.max(a, b)
            }
            property bool clipNow: {
                var a = deckA ? deckA.clipDetected : false
                var b = deckB ? deckB.clipDetected : false
                return a || b
            }

            // dB range: -33 to +9 = 42 segments
            readonly property int totalSegments: 42

            // Convert linear peak to dB
            function peakToDb(peak) {
                if (peak <= 0.0001) return -33.0
                var db = 20.0 * Math.log10(peak)
                return Math.max(-33.0, Math.min(15.0, db))
            }

            // Normalize dB to segment index (0-41)
            function dbToSegmentIndex(db) {
                return Math.floor((db + 33.0) * (totalSegments / 42.0))
            }

            // Color scheme: green → orange → red
            function getBarColor(segmentIndex) {
                if (segmentIndex >= 39) return "#ff2b2b"      // Red
                if (segmentIndex >= 36) return "#ff6b3d"      // Orange-red
                if (segmentIndex >= 24) return "#ffb347"      // Orange
                return "#44cc66"                               // Green
            }

            // Peak Hold + Decay
            property real peakHoldDbL: -33.0
            property real peakHoldDbR: -33.0

            onLevelLChanged: {
                var db = peakToDb(levelL)
                if (db > peakHoldDbL) {
                    peakHoldDbL = db
                    decayTimerL.restart()
                }
            }
            onLevelRChanged: {
                var db = peakToDb(levelR)
                if (db > peakHoldDbR) {
                    peakHoldDbR = db
                    decayTimerR.restart()
                }
            }

            Timer {
                id: decayTimerL
                interval: 300
                onTriggered: decayAnimL.start()
            }
            Timer {
                id: decayTimerR
                interval: 300
                onTriggered: decayAnimR.start()
            }

            NumberAnimation {
                id: decayAnimL
                target: vuColumn
                property: "peakHoldDbL"
                from: vuColumn.peakHoldDbL
                to: -33.0
                duration: 800
                easing.type: Easing.InQuad
            }
            NumberAnimation {
                id: decayAnimR
                target: vuColumn
                property: "peakHoldDbR"
                from: vuColumn.peakHoldDbR
                to: -33.0
                duration: 800
                easing.type: Easing.InQuad
            }

            // LEFT CHANNEL: Horizontal Bar
            Rectangle {
                width: root.vuBarWidth
                height: root.vuBarHeight
                radius: 0
                color: "#1a1a1a"
                border.color: "#333"
                border.width: 0

                Row {
                    anchors.fill: parent
                    anchors.margins: 1
                    spacing: 0

                    Repeater {
                        model: vuColumn.totalSegments
                        Rectangle {
                            required property int index
                            width: (parent.width - (vuColumn.totalSegments - 1) * 0) / vuColumn.totalSegments
                            height: parent.height
                            radius: 0
                            property real currentDb: vuColumn.peakToDb(vuColumn.levelL)
                            property real peakDb: vuColumn.peakHoldDbL
                            property int litSegments: vuColumn.dbToSegmentIndex(currentDb)
                            property int peakSegmentIndex: vuColumn.dbToSegmentIndex(peakDb)
                            property bool isLit: index <= litSegments
                            property bool isPeakBar: index === peakSegmentIndex
                            color: {
                                if (isPeakBar) return "#ffffff"
                                if (!isLit) return "#1a1a1a"
                                return vuColumn.getBarColor(index)
                            }
                        }
                    }
                }
            }

            // RIGHT CHANNEL: Horizontal Bar
            Rectangle {
                width: root.vuBarWidth
                height: root.vuBarHeight
                radius: 0
                color: "#1a1a1a"
                border.color: "#333"
                border.width: 0

                Row {
                    anchors.fill: parent
                    anchors.margins: 1
                    spacing: 0

                    Repeater {
                        model: vuColumn.totalSegments
                        Rectangle {
                            required property int index
                            width: (parent.width - (vuColumn.totalSegments - 1) * 0) / vuColumn.totalSegments
                            height: parent.height
                            radius: 0
                            property real currentDb: vuColumn.peakToDb(vuColumn.levelR)
                            property real peakDb: vuColumn.peakHoldDbR
                            property int litSegments: vuColumn.dbToSegmentIndex(currentDb)
                            property int peakSegmentIndex: vuColumn.dbToSegmentIndex(peakDb)
                            property bool isLit: index <= litSegments
                            property bool isPeakBar: index === peakSegmentIndex
                            color: {
                                if (isPeakBar) return "#ffffff"
                                if (!isLit) return "#1a1a1a"
                                return vuColumn.getBarColor(index)
                            }
                        }
                    }
                }
            }
        }

        // CLIP Indicator
        Column {
            spacing: 2
            anchors.verticalCenter: parent.verticalCenter

            property real peakMaxDb: {
                var dbL = vuColumn.peakToDb(vuColumn.levelL)
                var dbR = vuColumn.peakToDb(vuColumn.levelR)
                return Math.max(dbL, dbR)
            }

            property bool lightClip: vuColumn.clipNow && peakMaxDb < 3.0
            property bool hardClip: vuColumn.clipNow && peakMaxDb >= 3.0

            Timer {
                id: clipBlinkTimer
                interval: 150
                repeat: true
                running: parent.lightClip
            }

            // CLIP text
            Text {
                text: "CLIP"
                color: {
                    if (parent.hardClip) return "#ffffff"
                    if (parent.lightClip) return clipBlinkTimer.running && Math.floor((Date.now() / 150) % 2) ? "#ff4444" : "#444444"
                    return "#333333"
                }
                font.pixelSize: root.sp(10)
                font.bold: true
                font.family: "monospace"
                width: root.clipInfoWidth
                horizontalAlignment: Text.AlignHCenter
            }

            // Peak dB display
            Text {
                text: parent.peakMaxDb.toFixed(1) + "dB"
                color: parent.hardClip ? "#ffffff" : (parent.lightClip ? "#ff6b6b" : "#888888")
                font.pixelSize: root.sp(8)
                font.bold: true
                font.family: "monospace"
                width: root.clipInfoWidth
                horizontalAlignment: Text.AlignRight
            }
        }
    }
}
