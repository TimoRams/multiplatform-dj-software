import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Left overlay strip — does NOT shrink the waveform; playhead stays at deck center.
Rectangle {
    id: root

    property var engine: null
    property bool editMode: false
    property bool expanded: false

    readonly property real deckHeight: parent ? parent.height : 80
    readonly property real deckWidth: parent ? parent.width : 400
    readonly property real btnSize: Math.max(20, Math.min(28, Math.floor(deckHeight / 3.2)))
    readonly property real maxRows: Math.min(3, Math.max(1, Math.floor((deckHeight - 6) / (btnSize + 2))))
    readonly property real cellW: btnSize + 2
    readonly property real cellH: btnSize + 2

    readonly property var toolItems: [
        { id: "collapse", label: "◂", tip: "Collapse", type: "action", action: "collapse" },
        { id: "edit",     label: "✎", tip: "Click waveform → set downbeat", type: "toggle", action: "edit" },
        { id: "bpm",      label: "",  tip: "BPM", type: "bpm" },
        { id: "downbeat", label: "I", tip: "Set downbeat at playhead", type: "downbeat", action: "downbeat" },
        { id: "double",   label: "×2",tip: "Double BPM", type: "action", action: "double" },
        { id: "halve",    label: "/2",tip: "Halve BPM", type: "action", action: "halve" },
        { id: "nudge-1b", label: "-1b", tip: "-1 beat", type: "action", action: "nudge-1b" },
        { id: "nudge-10", label: "-10", tip: "-10 ms", type: "action", action: "nudge-10" },
        { id: "nudge+10", label: "+10", tip: "+10 ms", type: "action", action: "nudge+10" },
        { id: "nudge+1b", label: "+1b", tip: "+1 beat", type: "action", action: "nudge+1b" },
        { id: "lock",     label: "🔓", tip: "Lock grid", type: "toggle", action: "lock" }
    ]

    readonly property int flowCols: Math.max(1, Math.ceil(toolItems.length / maxRows))
    readonly property real flowContentW: flowCols * cellW + 6
    readonly property real collapsedStripWidth: 22
    readonly property real expandedWidth: Math.min(deckWidth * 0.55, Math.max(96, flowContentW))
    readonly property real occupiedWidth: expanded ? expandedWidth : collapsedStripWidth

    width: occupiedWidth
    height: parent ? parent.height : implicitHeight
    color: "#dd0a0a0a"
    z: 25
    clip: true

    Behavior on width { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }

    function runAction(action) {
        if (!root.engine && action !== "collapse" && action !== "edit") return
        switch (action) {
        case "collapse": root.expanded = false; break
        case "edit":     root.editMode = !root.editMode; break
        case "downbeat": root.engine.setDownbeatAtCurrentPosition(); break
        case "double":   root.engine.doubleBpm(); break
        case "halve":    root.engine.halveBpm(); break
        case "nudge-1b": root.engine.nudgeBeatgridBeats(-1); break
        case "nudge-10": root.engine.nudgeBeatgridMs(-10); break
        case "nudge+10": root.engine.nudgeBeatgridMs(10); break
        case "nudge+1b": root.engine.nudgeBeatgridBeats(1); break
        case "lock":     lockBox.checked = !lockBox.checked; break
        }
    }

    CheckBox {
        id: lockBox
        visible: false
        enabled: root.engine !== null
        checked: root.engine ? root.engine.beatgridLocked : false
        onToggled: { if (root.engine) root.engine.beatgridLocked = checked }
    }

    Connections {
        target: root.engine
        function onBeatgridLockedChanged() {
            if (root.engine) lockBox.checked = root.engine.beatgridLocked
        }
    }

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: "#30ffffff"
    }

    // Collapsed strip
    Item {
        anchors.fill: parent
        visible: !root.expanded

        Column {
            anchors.centerIn: parent
            spacing: 3
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 10; height: 10; radius: 2
                color: root.editMode ? "#e60000" : "#444"
                border.color: root.editMode ? "#ff6666" : "#555"
                border.width: 1
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "G"
                color: "#888"
                font.pixelSize: 7
                font.bold: true
            }
        }
        MouseArea {
            id: stripHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.expanded = true
        }
        ToolTip.visible: stripHover.containsMouse
        ToolTip.text: "Beat grid editor"
        ToolTip.delay: 500
    }

    // Expanded — Flow: max 3 rows, rest flows horizontally
    Item {
        anchors.fill: parent
        visible: root.expanded

        Flow {
            id: toolFlow
            anchors.centerIn: parent
            width: root.expandedWidth - 4
            spacing: 2

            Repeater {
                model: root.toolItems

                Item {
                    required property var modelData
                    width: modelData.type === "bpm" ? root.btnSize * 2.2 : root.btnSize
                    height: root.btnSize

                    // BPM field
                    TextField {
                        id: bpmField
                        anchors.fill: parent
                        visible: modelData.type === "bpm"
                        enabled: root.engine && root.engine.trackData && root.engine.trackData.isBpmAnalyzed
                        color: "#eee"
                        placeholderText: "BPM"
                        placeholderTextColor: "#555"
                        background: Rectangle { color: "#111"; border.color: "#333"; radius: 2 }
                        font.pixelSize: Math.max(7, root.btnSize * 0.34)
                        font.family: "monospace"
                        horizontalAlignment: TextInput.AlignHCenter
                        validator: DoubleValidator { bottom: 20; top: 300; decimals: 1 }
                        text: {
                            if (!root.engine || !root.engine.trackData) return ""
                            var bpm = root.engine.trackData.bpm
                            return bpm > 0 ? bpm.toFixed(1) : ""
                        }
                        onEditingFinished: {
                            if (!root.engine) return
                            var v = parseFloat(text)
                            if (!isNaN(v) && v > 0) root.engine.setManualBpm(v)
                        }
                    }

                    // Button cell
                    Rectangle {
                        anchors.fill: parent
                        visible: modelData.type !== "bpm"
                        radius: 2
                        color: {
                            if (modelData.action === "edit" && root.editMode) return "#331e7bd4"
                            if (modelData.action === "lock" && lockBox.checked) return "#331e7bd4"
                            return cellHover.containsMouse ? "#252525" : "#161616"
                        }
                        border.color: {
                            if (modelData.action === "edit" && root.editMode) return "#1e7bd4"
                            if (modelData.action === "lock" && lockBox.checked) return "#1e7bd4"
                            if (modelData.type === "downbeat") return "#55e60000"
                            return "#2a2a2a"
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: modelData.type !== "downbeat"
                            text: modelData.action === "lock"
                                  ? (lockBox.checked ? "🔒" : "🔓")
                                  : modelData.label
                            color: "#ccc"
                            font.pixelSize: Math.max(7, root.btnSize * 0.36)
                            font.bold: true
                            font.family: modelData.label.length <= 3 ? "monospace" : undefined
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: modelData.type === "downbeat"
                            text: "I"
                            color: "#e60000"
                            font.pixelSize: Math.max(14, root.btnSize * 0.72)
                            font.bold: true
                        }

                        MouseArea {
                            id: cellHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.runAction(modelData.action)
                        }
                        ToolTip.visible: cellHover.containsMouse && modelData.tip
                        ToolTip.text: modelData.tip
                        ToolTip.delay: 350
                    }
                }
            }
        }
    }
}
