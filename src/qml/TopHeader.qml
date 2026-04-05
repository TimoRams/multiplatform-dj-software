import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    color: "#121212"
    
    // Dynamic header height scaling based on window height
    // At reference height (800px), height is 40px
    // Scales proportionally with window height
    readonly property real _refHeaderHeight: 40
    readonly property real _refWindowHeight: 800
    height: Math.max(36, Math.round(_refHeaderHeight * (window.height / _refWindowHeight)))

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
        anchors.margins: Math.max(3, Math.round(root.height * 0.12))
        anchors.leftMargin: Math.max(8, Math.round(root.height * 0.25))
        anchors.rightMargin: Math.max(8, Math.round(root.height * 0.25))
        spacing: 0

        // ── LEFT: Software name ───────────────────────────────────────────────
        Row {
            spacing: 6
            Layout.alignment: Qt.AlignVCenter

            // Small coloured accent bar (Traktor-style)
            Rectangle {
                width: Math.max(2, Math.round(root.height * 0.075)); 
                height: Math.max(16, Math.round(root.height * 0.55)); 
                radius: 1
                anchors.verticalCenter: parent.verticalCenter
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: "#1e90ff" }
                    GradientStop { position: 1.0; color: "#0050cc" }
                }
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Text {
                    text: "DJ-Software"
                    color: "#ffffff"
                    font.pixelSize: window.sp(14)
                    font.bold: true
                    font.letterSpacing: 1.5
                }
                Text {
                    text: "by Ramsbrock.net"
                    color: "#555"
                    font.pixelSize: window.sp(8)
                    font.letterSpacing: 0.5
                }
            }
        }

        // ── ABLETON LINK section (always visible, fixed width) ─────────────────
        Row {
            spacing: 6
            Layout.alignment: Qt.AlignVCenter
            Layout.leftMargin: 12

            // LINK toggle button
            Rectangle {
                width: Math.max(35, Math.round(root.height * 1.1)); 
                height: Math.max(18, Math.round(root.height * 0.55))
                radius: 3
                color: (linkManager && linkManager.enabled) ? "#1a3322" : "#1a1a1a"
                border.color: (linkManager && linkManager.enabled) ? "#44cc66" : "#333"
                border.width: 1
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    anchors.centerIn: parent
                    text: "LINK"
                    color: (linkManager && linkManager.enabled) ? "#44cc66" : "#777"
                    font.pixelSize: window.sp(9)
                    font.bold: true
                    font.letterSpacing: 0.5
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (linkManager) linkManager.enabled = !linkManager.enabled
                }
            }

            // Peer count — always visible
            Text {
                width: 16
                text: linkManager ? linkManager.numPeers.toString() : "0"
                color: (linkManager && linkManager.numPeers > 0) ? "#44cc66" : "#555"
                font.pixelSize: window.sp(9)
                font.family: "monospace"
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                anchors.verticalCenter: parent.verticalCenter
            }

            // Link BPM display — fixed width, opacity-controlled
            Text {
                width: 48
                opacity: (linkManager && linkManager.enabled) ? 1.0 : 0.3
                text: linkManager ? linkManager.bpm.toFixed(1) : "120.0"
                color: "#ccc"
                font.pixelSize: window.sp(11)
                font.family: "monospace"
                font.bold: true
                horizontalAlignment: Text.AlignRight
                anchors.verticalCenter: parent.verticalCenter
            }

            // 4-beat phase indicator (running light) — fixed width, opacity-controlled
            Row {
                spacing: 3
                opacity: (linkManager && linkManager.enabled) ? 1.0 : 0.2
                anchors.verticalCenter: parent.verticalCenter

                Repeater {
                    model: 4
                    Rectangle {
                        required property int index
                        width: 7; height: 7; radius: 3.5
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
                        border.width: 1
                    }
                }
            }
        }

        // ── SPACER (left half) ─────────────────────────────────────────────────
        Item { Layout.fillWidth: true }

        // ── SPACER (right half) ───────────────────────────────────────────────
        Item { Layout.fillWidth: true }

        // ── RIGHT: CPU/RAM + REC + Clock + Actions ────────────────────────────
        Row {
            spacing: 14
            Layout.alignment: Qt.AlignVCenter

            // Anti-Clip button
            Rectangle {
                width: Math.max(44, Math.round(root.height * 1.3)); 
                height: Math.max(18, Math.round(root.height * 0.55))
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                property bool antiClipActive: false
                property real gr: deckA ? deckA.gainReduction : 1.0

                // gr < 0.5 (extreme clipping) -> Dark Red
                // gr < 0.7 (strong clipping) -> Red
                // gr < 0.99 (light clipping) -> Yellow
                // otherwise (no clipping) -> Green
                color: !antiClipActive ? "#1a1a1a" : (gr < 0.5 ? "#5c0000" : (gr < 0.7 ? "#b71c1c" : (gr < 0.99 ? "#f57f17" : "#1b5e20")))
                border.color: !antiClipActive ? "#333" : (gr < 0.5 ? "#8e0000" : (gr < 0.7 ? "#f44336" : (gr < 0.99 ? "#fbc02d" : "#4caf50")))
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "A-CLIP"
                    color: !parent.antiClipActive ? "#666" : (parent.gr < 0.99 ? "#fff" : "#c8e6c9")
                    font.pixelSize: window.sp(8)
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
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    text: "MST"
                    color: "#555"
                    font.pixelSize: window.sp(8)
                    font.bold: true
                    font.family: "monospace"
                    anchors.verticalCenter: parent.verticalCenter
                }

                Dial {
                    id: masterVolDial
                    width: Math.max(20, Math.round(root.height * 0.65)); 
                    height: Math.max(20, Math.round(root.height * 0.65))
                    from: 0.0; to: 1.0; value: 0.8

                    background: Rectangle {
                        x: masterVolDial.width / 2 - width / 2
                        y: masterVolDial.height / 2 - height / 2
                        width: masterVolDial.width
                        height: masterVolDial.height
                        radius: width / 2
                        color: "transparent"

                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width * 0.85
                            height: parent.height * 0.85
                            radius: width / 2
                            color: "#222"
                            border.color: "#444"
                            border.width: 1
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
                            height: parent.height * 0.35
                            radius: 1
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: 1
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

            // CPU / RAM bars (stacked thin bars)
            Row {
                spacing: 5
                anchors.verticalCenter: parent.verticalCenter

                Column {
                    spacing: 2
                    anchors.verticalCenter: parent.verticalCenter

                    // CPU bar
                    Row {
                        spacing: 3
                        Text {
                            text: "CPU"
                            color: "#555"
                            font.pixelSize: window.sp(7)
                            font.bold: true
                            width: 22
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Rectangle {
                            width: 50; height: 4
                            color: "#0d0d0d"
                            border.color: "#2a2a2a"
                            radius: 2
                            anchors.verticalCenter: parent.verticalCenter

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
                            font.pixelSize: window.sp(7)
                            font.bold: true
                            width: 22
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Rectangle {
                            width: 50; height: 4
                            color: "#0d0d0d"
                            border.color: "#2a2a2a"
                            radius: 2
                            anchors.verticalCenter: parent.verticalCenter

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
                width: Math.max(35, Math.round(root.height * 1.05)); 
                height: Math.max(18, Math.round(root.height * 0.55))
                color: "#1a1a1a"
                border.color: "#333"
                radius: 3
                anchors.verticalCenter: parent.verticalCenter

                Row {
                    anchors.centerIn: parent
                    spacing: 4

                    Rectangle {
                        width: 7; height: 7; radius: 3.5
                        color: "#aa3333"
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: "REC"
                        color: "#777"
                        font.pixelSize: window.sp(9)
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            // Clock
            Text {
                text: root.currentTime
                color: "#bbb"
                font.pixelSize: window.sp(12)
                font.family: "monospace"
                font.bold: true
                Layout.alignment: Qt.AlignVCenter
                anchors.verticalCenter: parent.verticalCenter
            }

            // UI action buttons
            Row {
                spacing: 3
                anchors.verticalCenter: parent.verticalCenter

                Button {
                    width: Math.max(24, Math.round(root.height * 0.7)); 
                    height: Math.max(20, Math.round(root.height * 0.6))
                    text: "⛶"
                    background: Rectangle {
                        color: parent.pressed ? "#333" : "#1e1e1e"
                        border.color: "#333"
                        radius: 3
                    }
                    contentItem: Text {
                        text: parent.text; color: "#aaa"; font.pixelSize: window.sp(13)
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment:   Text.AlignVCenter
                        anchors.centerIn: parent
                    }
                    onClicked: {
                        if (root.Window.window.visibility === Window.FullScreen)
                            root.Window.window.showNormal()
                        else
                            root.Window.window.showFullScreen()
                    }
                }

                Button {
                    width: Math.max(24, Math.round(root.height * 0.7)); 
                    height: Math.max(20, Math.round(root.height * 0.6))
                    text: "⚙"
                    background: Rectangle {
                        color: parent.pressed ? "#333" : "#1e1e1e"
                        border.color: "#333"
                        radius: 3
                    }
                    contentItem: Text {
                        text: parent.text; color: "#aaa"; font.pixelSize: window.sp(13)
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment:   Text.AlignVCenter
                        anchors.centerIn: parent
                    }
                    onClicked: {
                        settingsWin.show()
                        settingsWin.raise()
                        settingsWin.requestActivate()
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
                width: 120
                height: 6
                radius: 2
                color: "#1a1a1a"
                border.color: "#333"
                border.width: 1

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
                width: 120
                height: 6
                radius: 2
                color: "#1a1a1a"
                border.color: "#333"
                border.width: 1

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
                font.pixelSize: window.sp(10)
                font.bold: true
                font.family: "monospace"
                width: 40
                horizontalAlignment: Text.AlignHCenter
            }

            // Peak dB display
            Text {
                text: parent.peakMaxDb.toFixed(1) + "dB"
                color: parent.hardClip ? "#ffffff" : (parent.lightClip ? "#ff6b6b" : "#888888")
                font.pixelSize: window.sp(8)
                font.bold: true
                font.family: "monospace"
                width: 40
                horizontalAlignment: Text.AlignRight
            }
        }
    }
}
