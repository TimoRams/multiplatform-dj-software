import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Item {
    id: root

    property var engine: null
    property string accentColor: "#ff9900"
    property int activeTab: 0

    readonly property var tabs: ["HOT CUE", "PAD FX", "BEATJUMP", "STEMS"]
    readonly property var beatJumpPads: [-16, -8, -4, -2, 2, 4, 8, 16]

    readonly property real padAreaWidth: Math.max(290, root.width * 0.58)
    readonly property var palette16: [
        "#e04040", "#e08030", "#e0d030", "#30b050",
        "#30a0d0", "#6060e0", "#c040c0", "#e06080",
        "#ff4d4d", "#ff9f43", "#f6e05e", "#48bb78",
        "#38b2ac", "#4299e1", "#9f7aea", "#ed64a6"
    ]

    property int colorTargetIndex: -1

    function hotCueAt(index) {
        if (!root.engine || !root.engine.hotCues || index < 0 || index >= root.engine.hotCues.length)
            return null
        return root.engine.hotCues[index]
    }

    function formatCueTime(seconds) {
        var sec = Math.max(0, seconds || 0)
        var mins = Math.floor(sec / 60)
        var s = Math.floor(sec % 60)
        return mins.toString().padStart(2, "0") + ":" + s.toString().padStart(2, "0")
    }

    function beatJumpLabel(index) {
        var b = root.beatJumpPads[index]
        return (b > 0 ? "+" : "") + b + "B"
    }

    function beatDurationSeconds() {
        if (!root.engine)
            return 0.5
        var bpm = root.engine.currentBpm
        if (!bpm || bpm <= 0.0)
            return 0.5
        return 60.0 / bpm
    }

    function doBeatJump(beats) {
        if (!root.engine)
            return
        var duration = root.engine.getDuration()
        if (!duration || duration <= 0.0)
            return

        var current = root.engine.getPlayheadPositionAtomic()
        if (current === undefined || isNaN(current))
            current = root.engine.progress * duration

        var nextPos = current + beats * root.beatDurationSeconds()
        nextPos = Math.max(0.0, Math.min(duration, nextPos))
        root.engine.setPosition(nextPos / duration)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        // Mode tabs above pads.
        RowLayout {
            Layout.preferredWidth: padAreaWidth
            Layout.maximumWidth: padAreaWidth
            Layout.alignment: Qt.AlignLeft
            Layout.preferredHeight: 26
            Layout.maximumHeight: 26
            spacing: 2
            z: 30

            Repeater {
                model: root.tabs

                Rectangle {
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 3
                    color: root.activeTab === index ? "#2a2a2a" : "#181818"
                    border.color: root.activeTab === index ? root.accentColor : "#2a2a2a"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: root.activeTab === index ? "#f0f0f0" : "#888"
                        font.pixelSize: window.spViewport(8)
                        font.bold: root.activeTab === index
                        font.letterSpacing: 0.3
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.activeTab = index
                    }
                }
            }
        }

        RowLayout {
            id: contentRow
            Layout.fillWidth:  true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            spacing: 12
            z: 10

            Item {
                Layout.preferredWidth: padAreaWidth
                Layout.maximumWidth: padAreaWidth
                Layout.fillHeight: true

                Rectangle {
                    anchors.fill: parent
                    radius: 5
                    color: "#151515"
                    border.color: "#2a2a2a"
                    border.width: 1

                    Text {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.margins: 6
                        text: root.tabs[root.activeTab]
                        color: root.accentColor
                        font.pixelSize: window.spViewport(9)
                        font.bold: true
                        font.letterSpacing: 0.7
                    }
                }

                GridLayout {
                    anchors.fill: parent
                    anchors.margins: 6

                    columns: 4
                    rows: 2
                    columnSpacing: 3
                    rowSpacing: 3

                    Repeater {
                        model: 8

                        Rectangle {
                            Layout.fillWidth:  true
                            Layout.fillHeight: true
                            Layout.row:    Math.floor(index / 4)
                            Layout.column: index % 4

                            readonly property var cue: root.hotCueAt(index)
                            readonly property bool isHotCueTab: root.activeTab === 0
                            readonly property bool isBeatJumpTab: root.activeTab === 2
                            readonly property bool isPlaceholderTab: root.activeTab === 1 || root.activeTab === 3
                            readonly property bool cueSet: isHotCueTab && cue && cue.set
                            readonly property color activeColor: cueSet
                                ? cue.color
                                : (isBeatJumpTab ? "#4a3c22" : (isPlaceholderTab ? "#2f2f2f" : "#3a3a3a"))

                            radius: 5
                            color:  padMouse.pressed
                                    ? Qt.lighter(activeColor, 1.4)
                                    : padMouse.containsMouse
                                      ? Qt.lighter(activeColor, 1.15)
                                      : activeColor

                            border.color: cueSet ? Qt.lighter(activeColor, 1.5) : "#303030"
                            border.width: 1

                            // Pad number (top-left corner)
                            Text {
                                anchors.top:  parent.top
                                anchors.left: parent.left
                                anchors.margins: 5
                                text:  (index + 1).toString()
                                color: cueSet ? "#ffffff" : "#999"
                                font.pixelSize: window.spViewport(9)
                                font.bold: true
                                font.family: "monospace"
                                opacity: 0.7
                            }

                            Text {
                                anchors.centerIn: parent
                                text: {
                                    if (isHotCueTab)
                                        return cueSet ? root.formatCueTime(cue.positionSec) : "SET"
                                    if (isBeatJumpTab)
                                        return root.beatJumpLabel(index)
                                    return "-"
                                }
                                color: {
                                    if (isHotCueTab)
                                        return cueSet ? "#ffffff" : "#808080"
                                    if (isBeatJumpTab)
                                        return "#ffd38a"
                                    return "#777"
                                }
                                font.pixelSize: window.spViewport(8)
                                font.bold: isHotCueTab ? cueSet : true
                                font.family: "monospace"
                            }

                            MouseArea {
                                id: padMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton

                                onClicked: (mouse) => {
                                    if (!root.engine)
                                        return

                                    if (isBeatJumpTab) {
                                        if (mouse.button === Qt.LeftButton)
                                            root.doBeatJump(root.beatJumpPads[index])
                                        return
                                    }

                                    if (isPlaceholderTab)
                                        return

                                    if (mouse.button === Qt.LeftButton) {
                                        root.engine.triggerHotCue(index)
                                        return
                                    }

                                    if (mouse.button === Qt.MiddleButton) {
                                        root.engine.clearHotCue(index)
                                        return
                                    }

                                    if (mouse.button === Qt.RightButton) {
                                        root.colorTargetIndex = index
                                        var p = padMouse.mapToItem(root, mouse.x, mouse.y)
                                        colorPopup.x = Math.max(0, Math.min(root.width - colorPopup.width, p.x - colorPopup.width / 2))
                                        colorPopup.y = Math.max(0, Math.min(root.height - colorPopup.height, p.y - colorPopup.height / 2))
                                        colorPopup.open()
                                    }
                                }
                            }

                            Behavior on color { ColorAnimation { duration: 80 } }
                        }
                    }
                }
            }

            TurntableIndicator {
                engine: root.engine
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Math.min(contentRow.height, Math.max(100, root.width * 0.16))
                Layout.preferredHeight: Layout.preferredWidth
                Layout.maximumWidth: Layout.preferredWidth
                Layout.maximumHeight: Layout.preferredHeight
            }

            Item {
                Layout.fillWidth: true
            }
        }

        Text {
            Layout.preferredWidth: padAreaWidth
            Layout.maximumWidth: padAreaWidth
            text: {
                if (root.activeTab === 0)
                    return "8 HOT CUES  •  L-Klick: Jump/Set  •  M-Klick: Clear  •  R-Klick: Farbe"
                if (root.activeTab === 2)
                    return "BEATJUMP  •  L-Klick: ± Beats"
                return "Tab ist vorbereitet (Funktion folgt)"
            }
            color: "#666"
            font.pixelSize: window.spViewport(8)
            font.family: "monospace"
        }
    }

    Popup {
        id: colorPopup
        width: 132
        height: 132
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 6

        background: Rectangle {
            color: "#101010"
            border.color: "#2f2f2f"
            border.width: 1
            radius: 5
        }

        GridLayout {
            anchors.fill: parent
            columns: 4
            rowSpacing: 4
            columnSpacing: 4

            Repeater {
                model: root.palette16

                Rectangle {
                    required property var modelData
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    radius: 4
                    color: modelData
                    border.color: Qt.lighter(modelData, 1.45)
                    border.width: 1

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (root.colorTargetIndex >= 0 && root.engine) {
                                var cue = root.hotCueAt(root.colorTargetIndex)
                                if (!cue || !cue.set)
                                    root.engine.storeHotCue(root.colorTargetIndex)
                                root.engine.setHotCueColor(root.colorTargetIndex, modelData)
                            }
                            colorPopup.close()
                        }
                    }
                }
            }
        }
    }
}
