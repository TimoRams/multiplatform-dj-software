import QtQuick

Item {
    id: root

    property var engine: null
    property color ringColor: "#3a3a3a"
    property real ringThickness: 7
    property real cutoutAngleDeg: 16
    // 2 beats per full turn feels closer to a DJ platter indicator speed.
    property real beatsPerRevolution: 2.0
    property bool dragActive: false

    property bool _wasPlayingBeforeDrag: false
    property real _lastDragAngle: 0.0
    property real _grabOffsetAngle: 0.0

    function updateRotation() {
        if (root.dragActive)
            return

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

    function angleForPoint(xPos, yPos) {
        var dx = xPos - root.width / 2
        var dy = yPos - root.height / 2
        var angle = Math.atan2(dy, dx) * 180.0 / Math.PI
        if (angle < 0)
            angle += 360.0
        return angle
    }

    function applyDragDelta(deltaAngle) {
        if (!root.engine)
            return

        var bpm = root.engine.currentBpm > 0 ? root.engine.currentBpm : 120.0
        var secondsPerRevolution = (root.beatsPerRevolution * 60.0) / bpm
        var deltaSeconds = (deltaAngle / 360.0) * secondsPerRevolution

        root.engine.scratchBySeconds(deltaSeconds)
    }

    function normalizeAngle(angle) {
        var a = angle % 360.0
        if (a < 0)
            a += 360.0
        return a
    }

    function shortestAngleDelta(fromAngle, toAngle) {
        var delta = toAngle - fromAngle
        if (delta > 180)
            delta -= 360
        else if (delta < -180)
            delta += 360
        return delta
    }

    implicitWidth: 120
    implicitHeight: 120

    Item {
        id: platter
        anchors.fill: parent

        Item {
            id: markerRotator
            anchors.fill: parent
            transformOrigin: Item.Center

            Canvas {
                id: ringCanvas
                anchors.fill: parent
                antialiasing: true

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)

                    var cx = width * 0.5
                    var cy = height * 0.5
                    var lw = Math.max(2, root.ringThickness)
                    var radius = Math.min(width, height) * 0.5 - lw * 0.5 - 1
                    var gap = (Math.max(1, root.cutoutAngleDeg) * Math.PI) / 180.0
                    var start = -Math.PI * 0.5 + gap * 0.5
                    var end = start + (Math.PI * 2.0 - gap)

                    ctx.beginPath()
                    ctx.arc(cx, cy, radius, start, end, false)
                    ctx.lineWidth = lw
                    ctx.lineCap = "round"
                    ctx.strokeStyle = root.ringColor
                    ctx.stroke()
                }
            }

            FrameAnimation {
                // Keep platter locked to actual playhead during play and scrub/seek.
                running: root.engine !== null
                onTriggered: root.updateRotation()
            }
        }

        onWidthChanged: ringCanvas.requestPaint()
        onHeightChanged: ringCanvas.requestPaint()

        MouseArea {
            id: turntableMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: root.dragActive ? Qt.ClosedHandCursor : Qt.OpenHandCursor

            function inPlatter(mouse) {
                var dx = mouse.x - platter.width / 2
                var dy = mouse.y - platter.height / 2
                return (dx * dx + dy * dy) <= (platter.width * platter.width / 4)
            }

            onPressed: (mouse) => {
                if (!root.engine || !inPlatter(mouse)) {
                    mouse.accepted = false
                    return
                }

                root.dragActive = true
                var cursorAngle = root.angleForPoint(mouse.x, mouse.y)
                root._grabOffsetAngle = root.shortestAngleDelta(cursorAngle, markerRotator.rotation)
                root._lastDragAngle = markerRotator.rotation
                root._wasPlayingBeforeDrag = root.engine.isPlaying
                root.engine.pauseForScrub()
            }

            onPositionChanged: (mouse) => {
                if (!root.dragActive || !root.engine)
                    return

                var cursorAngle = root.angleForPoint(mouse.x, mouse.y)
                var nextVisualAngle = root.normalizeAngle(cursorAngle + root._grabOffsetAngle)
                var delta = root.shortestAngleDelta(root._lastDragAngle, nextVisualAngle)

                markerRotator.rotation = nextVisualAngle
                root._lastDragAngle = nextVisualAngle
                root.applyDragDelta(delta)
            }

            onReleased: {
                if (!root.dragActive || !root.engine)
                    return

                root.dragActive = false
                root.engine.resumeAfterScrub()
                root._wasPlayingBeforeDrag = false
                root.updateRotation()
            }

            onCanceled: {
                root.dragActive = false
                if (root.engine)
                    root.engine.resumeAfterScrub()
                root._wasPlayingBeforeDrag = false
                root.updateRotation()
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
