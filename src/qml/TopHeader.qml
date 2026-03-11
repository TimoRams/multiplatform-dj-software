import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    color: "#121212"
    height: 40
    border.color: "#333"
    border.width: 1
    
    // Bottom border only
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: "#000"
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
        anchors.margins: 5
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 0

        // ── LEFT: Software name ───────────────────────────────────────────────
        Row {
            spacing: 6
            Layout.alignment: Qt.AlignVCenter

            // Small coloured accent bar (Traktor-style)
            Rectangle {
                width: 3; height: 22; radius: 1
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
                width: 44; height: 22
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

        // ── SPACER ────────────────────────────────────────────────────────────
        Item { Layout.fillWidth: true }

        // ── CENTER GROUP: VU Meter + MASTER Volume ───────────────────────────
        Row {
            spacing: 10
            Layout.alignment: Qt.AlignVCenter

            // ── VU Meter (L + R, dot-based) ──────────────────────────────────
            Row {
                spacing: 2
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    text: "L"
                    color: "#555"
                    font.pixelSize: window.sp(8)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }

                // VU meter dots (L and R stacked, 48 dots each)
                Column {
                    id: vuColumn
                    spacing: 2
                    anchors.verticalCenter: parent.verticalCenter

                    // Shared level properties
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

                    // Channel L dots
                    Row {
                        spacing: 1
                        Repeater {
                            model: 48
                            Rectangle {
                                required property int index
                                width: 2; height: 4; radius: 0.5
                                property real threshold: (index + 1) / 48.0
                                property bool lit: vuColumn.levelL >= threshold
                                color: lit ? (index >= 38 ? "#ff8c00" : "#44cc66") : "#1a1a1a"
                            }
                        }
                    }

                    // Channel R dots
                    Row {
                        spacing: 1
                        Repeater {
                            model: 48
                            Rectangle {
                                required property int index
                                width: 2; height: 4; radius: 0.5
                                property real threshold: (index + 1) / 48.0
                                property bool lit: vuColumn.levelR >= threshold
                                color: lit ? (index >= 38 ? "#ff8c00" : "#44cc66") : "#1a1a1a"
                            }
                        }
                    }
                }

                Text {
                    text: "R"
                    color: "#555"
                    font.pixelSize: window.sp(8)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // ── MASTER Volume (right of VU) ──────────────────────────────────
            Row {
                spacing: 6
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    text: "MASTER"
                    color: "#666"
                    font.pixelSize: window.sp(9)
                    font.bold: true
                    font.letterSpacing: 1
                    anchors.verticalCenter: parent.verticalCenter
                }
                Slider {
                    id: masterSlider
                    width: 90; height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    from: 0.0; to: 1.0; value: 0.8

                    background: Rectangle {
                        x: masterSlider.leftPadding
                        y: masterSlider.topPadding + masterSlider.availableHeight / 2 - height / 2
                        width: masterSlider.availableWidth
                        height: 4; radius: 2
                        color: "#1a1a1a"
                        border.color: "#333"

                        Rectangle {
                            width: masterSlider.visualPosition * parent.width
                            height: parent.height; radius: 2
                            color: "#1e90ff"
                        }
                    }

                    handle: Rectangle {
                        x: masterSlider.leftPadding + masterSlider.visualPosition
                           * (masterSlider.availableWidth - width)
                        y: masterSlider.topPadding + masterSlider.availableHeight / 2 - height / 2
                        width: 10; height: 10; radius: 5
                        color: "#ddd"
                        border.color: "#888"
                        border.width: 1
                    }

                    TapHandler {
                        onDoubleTapped: {
                            masterSlider.enabled = false
                            masterSlider.value = 0.8
                            masterSlider.enabled = true
                        }
                    }
                }
            }
        }

        // ── SPACER ────────────────────────────────────────────────────────────
        Item { Layout.fillWidth: true }

        // ── RIGHT: CPU/RAM + REC + Clock + Actions ────────────────────────────
        Row {
            spacing: 14
            Layout.alignment: Qt.AlignVCenter

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
                width: 42; height: 22
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
                    width: 28; height: 24
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
                    width: 28; height: 24
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
}
