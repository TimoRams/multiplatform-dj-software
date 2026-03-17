import QtQuick
import QtQuick.Layouts

Item {
    id: root

    property int activeMode: 0
    property var engine: null
    property string accentColor: "#ff9900"
    readonly property real padAreaWidth: Math.max(260, root.width * 0.5)

    readonly property var modes: [
        { label: "HOT CUE",   color: "#e04040" },
        { label: "PAD FX",    color: "#40a0e0" },
        { label: "BEATJUMP",  color: "#e0a020" },
        { label: "STEMS",     color: "#40c080" }
    ]

    // Pad colours per mode (8 per mode). Hot Cue colours follow CDJ conventions.
    readonly property var padColors: [
        // HOT CUE
        ["#e04040","#e08030","#e0d030","#30b050","#30a0d0","#6060e0","#c040c0","#e06080"],
        // PAD FX
        ["#2080c0","#2080c0","#2080c0","#2080c0","#2080c0","#2080c0","#2080c0","#2080c0"],
        // BEATJUMP
        ["#c08820","#c08820","#c08820","#c08820","#c08820","#c08820","#c08820","#c08820"],
        // STEMS
        ["#30a868","#30a868","#30a868","#30a868","#30a868","#30a868","#30a868","#30a868"]
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        // Mode tabs are intentionally only as wide as the pad grid.
        RowLayout {
            Layout.preferredWidth: padAreaWidth
            Layout.maximumWidth: padAreaWidth
            Layout.alignment: Qt.AlignLeft
            Layout.preferredHeight: 32
            Layout.maximumHeight:   32
            spacing: 2

            Repeater {
                model: root.modes

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 3

                    color: root.activeMode === index ? "#2a2a2a" : "#181818"

                    // Active mode accent bar (bottom edge)
                    Rectangle {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        radius: 1
                        color:  modelData.color
                        visible: root.activeMode === index
                    }

                    Text {
                        anchors.centerIn: parent
                        text:  modelData.label
                        color: root.activeMode === index ? "#e0e0e0" : "#606060"
                        font.pixelSize: window.sp(9)
                        font.bold: root.activeMode === index
                        font.family: "sans-serif"
                        font.letterSpacing: 0.3
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.activeMode = index
                    }
                }
            }
        }

        // ── PAD GRID (4 × 2) — half width, half height, left-aligned ───
        RowLayout {
            id: contentRow
            Layout.fillWidth:  true
            Layout.fillHeight: true
            Layout.minimumHeight: 88
            spacing: 12

            Item {
                Layout.preferredWidth: padAreaWidth
                Layout.maximumWidth: padAreaWidth
                Layout.fillHeight: true

                GridLayout {
                    anchors.fill: parent

                    columns: 4
                    rows:    2
                    columnSpacing: 3
                    rowSpacing:    3

                    Repeater {
                        model: 8

                        Rectangle {
                            Layout.fillWidth:  true
                            Layout.fillHeight: true
                            Layout.row:    Math.floor(index / 4)
                            Layout.column: index % 4

                            radius: 5
                            color:  padMouse.pressed
                                    ? Qt.lighter(padBaseColor, 1.6)
                                    : padMouse.containsMouse
                                      ? Qt.lighter(padBaseColor, 1.2)
                                      : padBaseColor

                            border.color: Qt.lighter(padBaseColor, 1.4)
                            border.width: 1

                            readonly property color padBaseColor:
                                Qt.darker(root.padColors[root.activeMode][index], 2.8)

                            // Pad number (top-left corner)
                            Text {
                                anchors.top:  parent.top
                                anchors.left: parent.left
                                anchors.margins: 5
                                text:  (index + 1).toString()
                                color: Qt.lighter(parent.padBaseColor, 2.0)
                                font.pixelSize: window.sp(10)
                                font.bold: true
                                font.family: "monospace"
                                opacity: 0.7
                            }

                            MouseArea {
                                id: padMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onPressed:  console.log("[Pad] Mode=" + root.modes[root.activeMode].label
                                                        + " Pad=" + (index + 1))
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
    }
}
