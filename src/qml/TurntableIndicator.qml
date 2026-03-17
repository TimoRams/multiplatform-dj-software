import QtQuick

Item {
    id: root

    property var engine: null
    property color ringColor: "#3a3a3a"
    property color centerColor: "#272727"
    property color cutoutColor: "#050505"
    property color cutoutEdgeColor: "#1b1b1b"
    // 2 beats per full turn feels closer to a DJ platter indicator speed.
    property real beatsPerRevolution: 2.0

    function updateRotation() {
        if (!root.engine) {
            markerRotator.rotation = 0
            return
        }

        var playheadSec = root.engine.getPlayheadPositionAtomic()
        var bpm = root.engine.currentBpm > 0 ? root.engine.currentBpm : 120.0
        var degreesPerSecond = (bpm * 360.0) / (60.0 * root.beatsPerRevolution)
        var signedDegreesPerSecond = root.engine.isReverse ? -degreesPerSecond : degreesPerSecond

        var angle = (playheadSec * signedDegreesPerSecond) % 360.0
        if (angle < 0)
            angle += 360.0
        markerRotator.rotation = angle
    }

    implicitWidth: 120
    implicitHeight: 120

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        clip: true
        color: "#0e0e0e"
        border.color: root.ringColor
        border.width: 2

        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 0.16
            height: width
            radius: width / 2
            color: root.centerColor
            border.color: Qt.lighter(root.centerColor, 1.4)
            border.width: 1
        }

        Item {
            id: markerRotator
            anchors.fill: parent
            transformOrigin: Item.Center

            Rectangle {
                anchors.centerIn: parent
                width: parent.width * 0.38
                height: parent.width * 0.09
                radius: height / 2
                color: root.cutoutColor
                border.color: root.cutoutEdgeColor
                border.width: 1
                transform: Translate { x: width / 2 }
            }

            FrameAnimation {
                // Keep platter locked to actual playhead during play and scrub/seek.
                running: root.engine !== null
                onTriggered: root.updateRotation()
            }
        }
    }

    Connections {
        target: root.engine
        function onPlayingChanged() { root.updateRotation() }
        function onProgressChanged() { root.updateRotation() }
        function onTempoChanged() { root.updateRotation() }
        function onReverseChanged() { root.updateRotation() }
        function onTrackLoaded() { root.updateRotation() }
    }

    onEngineChanged: updateRotation()
    Component.onCompleted: updateRotation()
}
