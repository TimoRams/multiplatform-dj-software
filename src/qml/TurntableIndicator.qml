import QtQuick

Item {
    id: root

    property var   engine: null
    property color ringColor: "#3a3a3a"
    property real  ringThickness: 7
    property real  cutoutAngleDeg: 16
    property real  baseRpm: 33.0 + 1.0/3.0   // 33⅓ RPM = 200 °/s
    property real  scratchDeadzoneDeg: 0.05

    readonly property real degreesPerSecond: 200.0

    property bool dragActive: false
    property bool _scratchEngaged: false
    property real _accumDragAngle: 0.0
    property real _totalScratchedAngle: 0.0
    property real _pressPlayheadSec: 0.0
    property real _lastCursorAngle: 0.0

    function updateRotation() {
        if (!root.engine) { ringRotator.rotation = 0; return }
        var playheadSec = root.engine.scratchVisualActive
                        ? root.engine.getPlayheadPositionAtomic()
                        : root.engine.getVisualPositionQml()
        var angle = (playheadSec * root.degreesPerSecond) % 360.0
        if (angle < 0) angle += 360.0
        ringRotator.rotation = angle
    }

    function angleForPoint(xPos, yPos) {
        var dx = xPos - root.width  * 0.5
        var dy = yPos - root.height * 0.5
        var a = Math.atan2(dy, dx) * 180.0 / Math.PI
        if (a < 0) a += 360.0
        return a
    }

    function shortestAngleDelta(from, to) {
        var d = to - from
        if (d >  180) d -= 360
        if (d < -180) d += 360
        return d
    }

    implicitWidth: 120
    implicitHeight: 120

    Item {
        id: ringRotator
        x: 0; y: 0
        width: parent.width; height: parent.height
        transformOrigin: Item.Center

        Canvas {
            id: ringCanvas
            anchors.fill: parent
            antialiasing: true

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)

                var cx  = width  * 0.5
                var cy  = height * 0.5
                var lw  = Math.max(2, root.ringThickness)
                var rad = Math.min(width, height) * 0.5 - lw * 0.5 - 1
                var gap = root.cutoutAngleDeg * Math.PI / 180.0
                var s   = -Math.PI * 0.5 + gap * 0.5
                var e   = s + (Math.PI * 2.0 - gap)

                ctx.beginPath()
                ctx.arc(cx, cy, rad, s, e, false)
                ctx.lineWidth   = lw
                ctx.lineCap     = "butt"
                ctx.strokeStyle = root.ringColor
                ctx.stroke()
            }
        }
    }

    onWidthChanged:  ringCanvas.requestPaint()
    onHeightChanged: ringCanvas.requestPaint()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: root.dragActive ? Qt.ClosedHandCursor : Qt.OpenHandCursor

        function inPlatter(mouse) {
            var r  = root.width * 0.5
            var dx = mouse.x - r
            var dy = mouse.y - r
            return dx * dx + dy * dy <= r * r
        }

        onPressed: (mouse) => {
            if (!root.engine || !inPlatter(mouse)) { mouse.accepted = false; return }

            root.dragActive           = true
            root._scratchEngaged      = false
            root._accumDragAngle      = 0.0
            root._totalScratchedAngle = 0.0
            root._pressPlayheadSec    = root.engine.getVisualPositionQml()
            root._lastCursorAngle     = root.angleForPoint(mouse.x, mouse.y)
            root.engine.pauseForScrub(root._pressPlayheadSec)
        }

        onPositionChanged: (mouse) => {
            if (!root.dragActive || !root.engine) return

            var cur   = root.angleForPoint(mouse.x, mouse.y)
            var delta = root.shortestAngleDelta(root._lastCursorAngle, cur)
            root._lastCursorAngle = cur

            if (!root._scratchEngaged) {
                root._accumDragAngle += Math.abs(delta)
                if (root._accumDragAngle < root.scratchDeadzoneDeg) return
                root._scratchEngaged = true
            }

            root._totalScratchedAngle += delta
            var deltaSec = delta / root.degreesPerSecond
            root.engine.scratchBySeconds(deltaSec)
        }

        onReleased: {
            if (!root.dragActive || !root.engine) return
            root.dragActive = false
            root._scratchEngaged      = false
            root._accumDragAngle      = 0.0
            root._totalScratchedAngle = 0.0
            root.engine.resumeAfterScrub()
        }

        onCanceled: {
            root.dragActive = false
            root._scratchEngaged      = false
            root._accumDragAngle      = 0.0
            root._totalScratchedAngle = 0.0
            if (root.engine) root.engine.resumeAfterScrub()
        }
    }

    FrameAnimation {
        running: root.engine !== null
        onTriggered: root.updateRotation()
    }

    Connections {
        target: root.engine
        function onPlayingChanged()  { root.updateRotation() }
        function onProgressChanged() {
            if (!root.engine || (!root.engine.isPlaying && !root.engine.scratchVisualActive))
                root.updateRotation()
        }
        function onTempoChanged()    { root.updateRotation() }
        function onReverseChanged()  { root.updateRotation() }
        function onTrackLoaded()     { root.updateRotation() }
    }

    onEngineChanged:       updateRotation()
    Component.onCompleted: updateRotation()
}
