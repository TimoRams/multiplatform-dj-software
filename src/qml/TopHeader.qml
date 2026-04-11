import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    color: "#121212"
    
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
    readonly property int meterDotSize: Math.max(6, Math.round(buttonHeight * 0.2))
    readonly property int vuBarWidth: Math.max(100, Math.round(buttonHeight * 3.5))
    readonly property int vuBarHeight: Math.max(5, Math.round(buttonHeight * 0.18))
    readonly property int miniBarHeight: Math.max(4, Math.round(buttonHeight * 0.12))
    readonly property int clipInfoWidth: Math.max(36, Math.round(buttonHeight * 1.15))
    readonly property int logoTitlePx: Math.min(root.sp(14), Math.max(11, Math.round(buttonHeight * 0.42)))
    readonly property int logoSubPx: Math.min(root.sp(8), Math.max(7, Math.round(buttonHeight * 0.23)))
    readonly property int recTextPx: Math.min(root.sp(9), Math.max(8, Math.round(buttonHeight * 0.27)))
    readonly property int iconButtonPx: Math.min(root.sp(13), Math.max(10, Math.round(buttonHeight * 0.4)))
    
    // Buttons should match the bar height exactly.
    readonly property int buttonHeight: root.height

    function sp(px) {
        // TopHeader has a fixed bar height of 34px. Do not scale fonts based on window height,
        // otherwise they will blow up and clip. Return the exact pixel value.
        return px;
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
    
    Component.onCompleted: {
        var date = new Date();
        var h = date.getHours().toString().padStart(2, '0');
        var m = date.getMinutes().toString().padStart(2, '0');
        root.currentTime = h + ":" + m;
    }

    // Settings window — created once, shown/hidden on demand
    SettingsWindow {
        id: settingsWin
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
                radius: 1
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
            spacing: 6
            Layout.fillHeight: true
            Layout.leftMargin: 12
            height: root.buttonHeight

            // LINK toggle button
            Rectangle {
                width: root.linkToggleWidth
                height: parent.height
                radius: 0
                color: (linkManager && linkManager.enabled) ? "#1a3322" : "#1a1a1a"
                border.color: (linkManager && linkManager.enabled) ? "#44cc66" : "#333"
                border.width: 0

                Text {
                    anchors.centerIn: parent
                    text: "LINK"
                    color: (linkManager && linkManager.enabled) ? "#44cc66" : "#777"
                    font.pixelSize: root.sp(9)
                    font.bold: true
                    font.letterSpacing: 0.5
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (linkManager) linkManager.enabled = !linkManager.enabled
                }
            }

            // Peer count
            Text {
                width: root.linkPeerWidth
                text: linkManager ? linkManager.numPeers.toString() : "0"
                color: (linkManager && linkManager.numPeers > 0) ? "#44cc66" : "#555"
                font.pixelSize: root.sp(9)
                font.family: "monospace"
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            // Link BPM display
            Text {
                width: root.linkBpmWidth
                opacity: (linkManager && linkManager.enabled) ? 1.0 : 0.3
                text: linkManager ? linkManager.bpm.toFixed(1) : "120.0"
                color: "#ccc"
                font.pixelSize: root.sp(11)
                font.family: "monospace"
                font.bold: true
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
            }

            // 4-beat phase indicator
            Row {
                spacing: 3
                opacity: (linkManager && linkManager.enabled) ? 1.0 : 0.2
                height: parent.height

                Repeater {
                    model: 4
                    Rectangle {
                        required property int index
                        width: root.meterDotSize
                        height: root.meterDotSize
                        radius: width / 2
                        color: {
                            if (!linkManager || !linkManager.enabled) return "#222"
                            var beatIndex = Math.floor(linkManager.phase)
                            if (beatIndex < 0) beatIndex = 0
                            if (beatIndex > 3) beatIndex = 3
                            return index === beatIndex ? "#44cc66" : "#222"
                        }
                        border.color: {
                            if (!linkManager || !linkManager.enabled) return "#333"
                            var bi = Math.floor(Math.max(0, Math.min(3, linkManager.phase)))
                            return index === bi ? "#66ee88" : "#333"
                        }
                        border.width: 0
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
            Row {
                spacing: 3
                Layout.alignment: Qt.AlignVCenter

                Text {
                    text: "MST"
                    color: "#555"
                    font.pixelSize: root.sp(8)
                    font.bold: true
                    font.family: "monospace"
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
                            radius: 1
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

            // CPU / RAM bars
            Row {
                spacing: 5
                Layout.alignment: Qt.AlignVCenter

                Column {
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
                            border.color: "#2a2a2a"
                            radius: 2

                            Rectangle {
                                width: (sysMonitor ? sysMonitor.cpuUsage : 0) * parent.width
                                height: parent.height; radius: 2
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
                            border.color: "#2a2a2a"
                            radius: 2

                            Rectangle {
                                width: (sysMonitor ? sysMonitor.ramUsage : 0) * parent.width
                                height: parent.height; radius: 2
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
            Text {
                text: root.currentTime
                color: "#bbb"
                font.pixelSize: root.sp(12)
                font.family: "monospace"
                font.bold: true
                Layout.alignment: Qt.AlignVCenter
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
                radius: 2
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
                            radius: 1
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
                radius: 2
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
                            radius: 1
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
