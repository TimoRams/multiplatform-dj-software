import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Item {
    id: root

    property var engine: null
    property string accentColor: "#ff9900"
    property int activeTab: 0

    property double hotCueHoldPressedIndex: -1
    property double hotCueHoldCuePosition: 0.0
    property bool hotCueHoldWasPlaying: false

    readonly property var tabs: ["HOT CUE", "PAD FX", "BEATJUMP", "STEMS"]
    readonly property var beatJumpPads: [-16, -8, -4, -2, 2, 4, 8, 16]

    property int colorTargetIndex: -1

    readonly property var palette16: [
        "#e04040", "#e08030", "#e0d030", "#30b050",
        "#30a0d0", "#6060e0", "#c040c0", "#e06080",
        "#ff4d4d", "#ff9f43", "#f6e05e", "#48bb78",
        "#38b2ac", "#4299e1", "#9f7aea", "#ed64a6"
    ]

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
        if (!root.engine) return 0.5
        var bpm = root.engine.currentBpm
        if (!bpm || bpm <= 0.0) return 0.5
        return 60.0 / bpm
    }

    function doBeatJump(beats) {
        if (!root.engine) return
        var duration = root.engine.getDuration()
        if (!duration || duration <= 0.0) return
        var current = root.engine.getPlayheadPositionAtomic()
        if (current === undefined || isNaN(current))
            current = root.engine.progress * duration
        var nextPos = current + beats * root.beatDurationSeconds()
        nextPos = Math.max(0.0, Math.min(duration, nextPos))
        root.engine.setPosition(nextPos / duration)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Mode tabs ─────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            spacing: 1

            Repeater {
                model: root.tabs

                Rectangle {
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 2
                    color: root.activeTab === index ? "#252525" : "#161616"

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left; anchors.right: parent.right
                        height: 2; radius: 1
                        color: root.accentColor
                        visible: root.activeTab === index
                    }

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: root.activeTab === index ? "#f0f0f0" : "#666"
                        font.pixelSize: window.spViewport(8)
                        font.bold: root.activeTab === index
                        font.letterSpacing: 0.4
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.activeTab = index
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#0e0e0e" }

        // ── Pads + Turntable ──────────────────────────────────────────────
        RowLayout {
            id: contentRow
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 4

            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
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
                        readonly property bool isHotCueTab:     root.activeTab === 0
                        readonly property bool isBeatJumpTab:   root.activeTab === 2
                        readonly property bool isPlaceholderTab: root.activeTab === 1 || root.activeTab === 3
                        readonly property bool cueSet: isHotCueTab && cue && cue.set
                        readonly property color activeColor: cueSet
                            ? cue.color
                            : (isBeatJumpTab ? "#3a2e14" : (isPlaceholderTab ? "#1a1a1a" : "#1e1e1e"))

                        radius: 3
                        color: padMouse.pressed
                               ? Qt.lighter(activeColor, 1.6)
                               : padMouse.containsMouse
                                 ? Qt.lighter(activeColor, 1.25)
                                 : activeColor

                        border.color: cueSet ? Qt.lighter(cue.color, 1.7) : "#282828"
                        border.width: 1

                        // Pad number (top-left)
                        Text {
                            anchors.top: parent.top; anchors.left: parent.left
                            anchors.margins: 4
                            text: (index + 1).toString()
                            color: cueSet ? "#cccccc" : "#3a3a3a"
                            font.pixelSize: window.spViewport(7)
                            font.bold: true
                            font.family: "monospace"
                        }

                        // Main label
                        Text {
                            anchors.centerIn: parent
                            text: {
                                if (isHotCueTab)    return cueSet ? root.formatCueTime(cue.positionSec) : "+"
                                if (isBeatJumpTab)  return root.beatJumpLabel(index)
                                return "—"
                            }
                            color: {
                                if (isHotCueTab)    return cueSet ? "#ffffff" : "#444"
                                if (isBeatJumpTab)  return "#ffd38a"
                                return "#333"
                            }
                            font.pixelSize: (isHotCueTab && !cueSet) ? window.spViewport(16) : window.spViewport(8)
                            font.bold: isHotCueTab ? cueSet : true
                            font.family: "monospace"
                        }

                        MouseArea {
                            id: padMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton

                            onPressed: (mouse) => {
                                if (!root.engine) return
                                if (mouse.button !== Qt.LeftButton || !isHotCueTab) return
                                root.hotCueHoldPressedIndex = index
                                root.hotCueHoldCuePosition = cueSet ? cue.positionSec : 0.0
                                root.hotCueHoldWasPlaying = root.engine.isPlaying
                                if (cueSet) {
                                    root.engine.triggerHotCue(index)
                                    if (!root.hotCueHoldWasPlaying) root.engine.play()
                                } else {
                                    root.engine.storeHotCue(index)
                                }
                            }

                            onReleased: (mouse) => {
                                if (!root.engine || !isHotCueTab) return
                                if (root.hotCueHoldPressedIndex !== index) return
                                if (!root.hotCueHoldWasPlaying) {
                                    root.engine.pause()
                                    var trackLen = root.engine.getDuration()
                                    if (trackLen && trackLen > 0) {
                                        var normalizedPos = root.hotCueHoldCuePosition / trackLen
                                        root.engine.setPosition(Math.max(0, Math.min(1.0, normalizedPos)))
                                    }
                                }
                                root.hotCueHoldPressedIndex = -1
                                root.hotCueHoldCuePosition = 0.0
                            }

                            onClicked: (mouse) => {
                                if (!root.engine) return
                                if (isBeatJumpTab) {
                                    if (mouse.button === Qt.LeftButton) root.doBeatJump(root.beatJumpPads[index])
                                    return
                                }
                                if (isPlaceholderTab) return
                                if (mouse.button === Qt.MiddleButton) {
                                    root.engine.clearHotCue(index); return
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

            TurntableIndicator {
                engine: root.engine
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth:  Math.min(contentRow.height, Math.max(72, root.width * 0.14))
                Layout.preferredHeight: Layout.preferredWidth
                Layout.maximumWidth:    Layout.preferredWidth
                Layout.maximumHeight:   Layout.preferredHeight
            }
        }
    }

    Popup {
        id: colorPopup
        width: 132; height: 132
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 6

        background: Rectangle {
            color: "#242424"; border.color: "#111"; border.width: 1; radius: 2
        }

        GridLayout {
            anchors.fill: parent
            columns: 4; rowSpacing: 4; columnSpacing: 4

            Repeater {
                model: root.palette16
                Rectangle {
                    required property var modelData
                    Layout.preferredWidth: 26; Layout.preferredHeight: 26
                    radius: 2; color: modelData
                    border.color: Qt.lighter(modelData, 1.3); border.width: 1

                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (root.colorTargetIndex >= 0 && root.engine) {
                                var cue = root.hotCueAt(root.colorTargetIndex)
                                if (!cue || !cue.set) root.engine.storeHotCue(root.colorTargetIndex)
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
