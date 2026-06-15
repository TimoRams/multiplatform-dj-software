import QtQuick
import QtQuick.Layouts
import DJSoftware

Item {
    id: root

    property var engine: null
    property color stripeColor: UiTheme.separatorSubtle

    Layout.fillWidth: true
    Layout.preferredHeight: 44

    Rectangle {
        anchors.fill: parent
        color: UiTheme.bgDisplay
        clip: true

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.stripeColor
        }

        Item {
            id: overviewViewport
            anchors.fill: parent
            anchors.margins: 1
            clip: true

            RgbWaveformItem {
                id: overview
                anchors.fill: parent
                engine: root.engine
                rectified: true
            }
        }

        // ── Loop region ───────────────────────────────────────────────────
        Item {
            visible: root.engine !== null &&
                     root.engine.loopInSet &&
                     _hi > _lo

            readonly property bool   _complete: root.engine
                                               ? root.engine.loopInPosition < root.engine.loopOutPosition
                                               : false
            readonly property bool   _pending: root.engine ? root.engine.loopInSet && !_complete : false
            readonly property bool   _active: root.engine ? root.engine.loopActive && _complete : false
            readonly property double _dur:    (root.engine && root.engine.trackDurationSec > 0)
                                              ? root.engine.trackDurationSec : 1.0
            readonly property double _out: root.engine
                                           ? (_complete
                                              ? root.engine.loopOutPosition
                                              : (_pending
                                                 ? root.engine.loopPreviewOutPosition
                                                 : root.engine.loopOutPosition))
                                           : 0.0
            readonly property double _lo: root.engine ? root.engine.loopInPosition / _dur : 0.0
            readonly property double _hi: root.engine ? _out / _dur : 0.0

            anchors.top:    overviewViewport.top
            anchors.bottom: overviewViewport.bottom
            x:     overviewViewport.x + _lo * overviewViewport.width
            width: Math.max(1, (_hi - _lo) * overviewViewport.width)
            z:     2

            Rectangle {
                anchors.fill: parent
                color: parent._active ? "#2b7cff18"
                    : (parent._pending ? "#2b9fff12" : "#7fd7ff0c")
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: parent._active || parent._pending ? "#7ab8d8" : "#4a6068"
                opacity: parent._active ? 0.55 : (parent._pending ? 0.45 : 0.3)
            }
        }

        Item {
            id: overviewPlayhead
            width: 9
            anchors.top: overviewViewport.top
            anchors.bottom: overviewViewport.bottom
            x: {
                if (!root.engine) return overviewViewport.x
                var p = root.engine.progress
                if (p < 0.0) p = 0.0
                if (p > 1.0) p = 1.0
                return overviewViewport.x + p * overviewViewport.width - width / 2
            }
            z: 5

            Rectangle {
                anchors.centerIn: parent
                width: 2
                height: parent.height
                color: UiTheme.playhead
            }
        }

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

                        if (progress > loopOutProgress)
                            progress = loopInProgress
                    }
                }

                root.engine.setPosition(progress)
            }
        }
    }
}
