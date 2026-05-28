import QtQuick
import QtQuick.Layouts
import DJSoftware

Item {
    id: root

    property var engine: null
    property color stripeColor: "#2a2a2a"

    Layout.fillWidth: true
    Layout.preferredHeight: 44

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: "#050505"
        border.width: 1
        clip: true

        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.00; color: "#151515" }
            GradientStop { position: 0.48; color: root.stripeColor }
            GradientStop { position: 1.00; color: "#060606" }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: "#2c2c2c"
            opacity: 0.7
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: "#ffffff"
            opacity: 0.08
        }

        RgbWaveformItem {
            id: overview
            anchors.fill: parent
            anchors.leftMargin: 2
            anchors.rightMargin: 2
            anchors.topMargin: 2
            anchors.bottomMargin: 2
            engine: root.engine
            rectified: true
        }

        // ── Loop region — active (bright) + ghost (dim when inactive but positions set) ──
        Item {
            visible: root.engine !== null &&
                     root.engine.loopInPosition < root.engine.loopOutPosition

            readonly property bool   _active: root.engine ? root.engine.loopActive : false
            readonly property double _dur:    (root.engine && root.engine.trackDurationSec > 0)
                                              ? root.engine.trackDurationSec : 1.0
            readonly property double _lo: root.engine ? root.engine.loopInPosition  / _dur : 0.0
            readonly property double _hi: root.engine ? root.engine.loopOutPosition / _dur : 0.0

            anchors.top:    overview.top
            anchors.bottom: overview.bottom
            x:     overview.x + _lo * overview.width
            width: Math.max(1, (_hi - _lo) * overview.width)
            z:     2

            Rectangle {
                anchors.fill: parent
                color: parent._active ? "#2b7cff22" : "#7fd7ff12"
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: parent._active ? "#a9e8ff" : "#6a8d98"
                opacity: parent._active ? 0.75 : 0.45
            }
            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: parent._active ? "#bfffe9" : "#547b73"
            }
            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: parent._active ? "#bfffe9" : "#547b73"
            }
        }

        Item {
            id: overviewPlayhead
            width: 7
            anchors.top: overview.top
            anchors.bottom: overview.bottom
            x: {
                if (!root.engine) return overview.x
                var p = root.engine.progress
                if (p < 0.0) p = 0.0
                if (p > 1.0) p = 1.0
                return overview.x + p * overview.width - width / 2
            }
            z: 5

            Rectangle {
                anchors.centerIn: parent
                width: 5
                height: parent.height
                color: "#ff1f1f"
                opacity: 0.18
            }
            Rectangle {
                anchors.centerIn: parent
                width: 1
                height: parent.height
                color: "#ffffff"
                opacity: 0.95
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                width: 5
                height: 2
                color: "#ff3030"
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: 5
                height: 2
                color: "#ff3030"
            }
        }

        // Scrubbing / seeking
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            
            onPressed: (mouse) => seekTo(mouse.x)
            
            onPositionChanged: (mouse) => seekTo(mouse.x)

            function seekTo(xPos) {
                if (!root.engine) return
                var progress = xPos / width
                if (progress < 0.0) progress = 0.0
                if (progress > 1.0) progress = 1.0

                if (root.engine.loopActive && root.engine.loopOutPosition > root.engine.loopInPosition) {
                    var trackLength = root.engine.duration
                    if (trackLength > 0.0) {
                        var loopInProgress  = root.engine.loopInPosition  / trackLength
                        var loopOutProgress = root.engine.loopOutPosition / trackLength

                        if (progress > loopOutProgress) {
                            // Past loop end → wrap to loop start, loop stays active.
                            progress = loopInProgress
                        }
                        // Before loop start → jump there, audio plays through to loop.
                        // Inside loop → normal jump.
                        // Loop stays active in all cases.
                    }
                }

                root.engine.setPosition(progress)
            }
        }
    }
}
