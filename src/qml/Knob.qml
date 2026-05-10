import QtQuick
import QtQuick.Controls as Controls

// Shared rotary knob.
// – accentColor: the fill arc color (pass deck accent or unit accent)
// – defaultValue: double-tap resets here (defaults to midpoint)
// – Bipolar mode: automatic when from < 0 && to > 0 – arc draws from center
Controls.Dial {
    id: knob

    property real  defaultValue: (from + to) / 2
    property color accentColor:  "#ff9900"

    // ── Background: arc canvas + inner circle ─────────────────────────────
    background: Rectangle {
        x: knob.width  / 2 - width  / 2
        y: knob.height / 2 - height / 2
        width:  knob.width
        height: knob.height
        radius: width / 2
        color:  "transparent"
        border.color: "transparent"

        Canvas {
            id: arc
            anchors.fill: parent
            antialiasing: true

            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var cx   = width  / 2
                var cy   = height / 2
                var r    = Math.min(width, height) * 0.44
                var lw   = Math.max(2, Math.round(width * 0.08))
                var sDeg = 120
                var span = 300
                function c01(v) { return Math.max(0, Math.min(1, v)) }
                var norm = c01((knob.value - knob.from) / (knob.to - knob.from))

                ctx.lineWidth = lw
                ctx.lineCap   = "round"

                // Dim track
                ctx.strokeStyle = "#252525"
                ctx.beginPath()
                ctx.arc(cx, cy, r, sDeg * Math.PI / 180, (sDeg + span) * Math.PI / 180, false)
                ctx.stroke()

                // Filled arc (accent)
                ctx.strokeStyle = knob.accentColor
                if (knob.from < 0 && knob.to > 0) {
                    var mid = c01((0 - knob.from) / (knob.to - knob.from))
                    var a0  = (sDeg + mid  * span) * Math.PI / 180
                    var a1  = (sDeg + norm * span) * Math.PI / 180
                    ctx.beginPath()
                    ctx.arc(cx, cy, r, a0, a1, norm < mid)
                    ctx.stroke()
                } else {
                    ctx.beginPath()
                    ctx.arc(cx, cy, r, sDeg * Math.PI / 180, (sDeg + norm * span) * Math.PI / 180, false)
                    ctx.stroke()
                }
            }

            Connections {
                target: knob
                function onValueChanged() { arc.requestPaint() }
                function onFromChanged()  { arc.requestPaint() }
                function onToChanged()    { arc.requestPaint() }
            }
        }

        // Inner circle – gives the raised-button look
        Rectangle {
            anchors.centerIn: parent
            width:  parent.width  * 0.78
            height: parent.height * 0.78
            radius: width / 2
            color:  "#1a1a1a"
        }
    }

    // ── Handle: thin pointer line ─────────────────────────────────────────
    handle: Rectangle {
        id: h
        x: knob.background.x + knob.background.width  / 2 - width  / 2
        y: knob.background.y + knob.background.height / 2 - height / 2
        width:  knob.width  * 0.78
        height: knob.height * 0.78
        color:  "transparent"

        Rectangle {
            color:  "#c8c8c8"
            width:  1.5
            height: parent.height * 0.42
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top:       parent.top
            anchors.topMargin: -1
        }

        transform: Rotation {
            angle:    knob.angle
            origin.x: h.width  / 2
            origin.y: h.height / 2
        }
    }

    // ── Double-tap to reset ───────────────────────────────────────────────
    TapHandler {
        onDoubleTapped: {
            knob.enabled = false
            knob.value   = knob.defaultValue
            knob.enabled = true
        }
    }
}
