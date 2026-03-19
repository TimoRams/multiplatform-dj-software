import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Item {
    id: root

    property var engine: null
    property string accentColor: "#ff9900"

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

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            id: contentRow
            Layout.fillWidth:  true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            spacing: 12

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
                        text: "HOT CUE"
                        color: root.accentColor
                        font.pixelSize: window.sp(9)
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
                            readonly property bool cueSet: cue && cue.set
                            readonly property color activeColor: cueSet
                                ? cue.color
                                : "#3a3a3a"

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
                                font.pixelSize: window.sp(9)
                                font.bold: true
                                font.family: "monospace"
                                opacity: 0.7
                            }

                            Text {
                                anchors.centerIn: parent
                                text: cueSet ? root.formatCueTime(cue.positionSec) : "SET"
                                color: cueSet ? "#ffffff" : "#808080"
                                font.pixelSize: window.sp(8)
                                font.bold: cueSet
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
            text: "8 HOT CUES  •  L-Klick: Jump/Set  •  M-Klick: Clear  •  R-Klick: Farbe"
            color: "#666"
            font.pixelSize: window.sp(8)
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
