import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: cfBar
    color: "#080808"

    property var  engineA: null
    property var  engineB: null
    property var  engineC: null
    property var  engineD: null
    property bool fourDeckMode: false

    property string assignA: "A"
    property string assignB: "B"
    property string assignC: "A"
    property string assignD: "B"

    property real cfPos: 0.0

    // 0 = smooth/linear (wide fade), 1 = sharp/cut (narrow fade near edges)
    property real cfSharpness: 0.0

    property real volA: 1.0
    property real volB: 1.0
    property real volC: 1.0
    property real volD: 1.0

    readonly property color clrA: "#ff9900"
    readonly property color clrB: "#00ccff"
    readonly property color clrC: "#cc44ff"
    readonly property color clrD: "#44ddaa"

    // Serato-style curve:
    //   e = 1   → linear crossfade (both channels at 50% at center)
    //   e → 0   → sharp/cut (outgoing stays near full, incoming rises immediately,
    //              transition compressed toward outer edges)
    //
    // t ∈ [0,1]  (t = (cfPos + 1) / 2)
    //   vol_B = t^e        rises 0→1 fast when e is small, slow when e=1
    //   vol_A = (1-t)^e    stays near 1 for long when e is small, linear when e=1
    function curveExp() {
        // knob right (sharp=1) → e≈0.01,  knob left (smooth=0) → e=1
        return Math.pow(10.0, -cfSharpness * 2.0)
    }

    function cfMult(assign) {
        if (assign === "T") return 1.0
        var t = (cfPos + 1.0) * 0.5          // 0 at full-A, 1 at full-B
        var e = curveExp()
        if (assign === "A") return Math.pow(Math.max(0.0, 1.0 - t), e)
        if (assign === "B") return Math.pow(Math.max(0.0, t),       e)
        return 1.0
    }

    function applyVolumes() {
        if (engineA) engineA.volume = volA * cfMult(assignA)
        if (engineB) engineB.volume = volB * cfMult(assignB)
        if (engineC) engineC.volume = volC * cfMult(assignC)
        if (engineD) engineD.volume = volD * cfMult(assignD)
    }

    onCfPosChanged:       applyVolumes()
    onCfSharpnessChanged: applyVolumes()
    onAssignAChanged:     applyVolumes()
    onAssignBChanged:     applyVolumes()
    onAssignCChanged:     applyVolumes()
    onAssignDChanged:     applyVolumes()
    onEngineAChanged:     applyVolumes()
    onEngineBChanged:     applyVolumes()
    onEngineCChanged:     applyVolumes()
    onEngineDChanged:     applyVolumes()

    Component.onCompleted: applyVolumes()

    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if      (id === "deckA_vol") { cfBar.volA = value; cfBar.applyVolumes() }
            else if (id === "deckB_vol") { cfBar.volB = value; cfBar.applyVolumes() }
            else if (id === "deckC_vol") { cfBar.volC = value; cfBar.applyVolumes() }
            else if (id === "deckD_vol") { cfBar.volD = value; cfBar.applyVolumes() }
        }
    }

    // ── Per-deck assignment selector ─────────────────────────────────────
    component AssignGroup: RowLayout {
        id: ag
        required property string deckLabel
        required property color  deckAccent
        property string currentAssign: "A"
        signal picked(string v)

        spacing: 1

        Text {
            text: ag.deckLabel; color: ag.deckAccent
            font.pixelSize: window.spViewport(8); font.bold: true; font.family: "monospace"
            Layout.alignment: Qt.AlignVCenter; Layout.rightMargin: 3
        }
        Repeater {
            model: ["A", "T", "B"]
            delegate: Rectangle {
                required property string modelData
                required property int    index
                Layout.preferredWidth: 18; Layout.preferredHeight: 20
                radius: 2
                color:        ag.currentAssign === modelData ? "#1e1e1e" : "#0d0d0d"
                border.width: 1
                border.color: ag.currentAssign === modelData ? ag.deckAccent : "#1c1c1c"
                Text {
                    anchors.centerIn: parent; text: parent.modelData
                    color: ag.currentAssign === parent.modelData ? ag.deckAccent : "#3a3a3a"
                    font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace"
                }
                HoverHandler { id: hov }
                Rectangle { anchors.fill: parent; radius: 2; color: "#fff"
                    opacity: hov.hovered && ag.currentAssign !== parent.modelData ? 0.04 : 0.0 }
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    onClicked: ag.picked(parent.modelData)
                }
            }
        }
    }

    // ── Curve settings — anchored to right ────────────────────────────────
    RowLayout {
        id: rightPanel
        anchors.right: parent.right; anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3

        Rectangle {
            width: 1; height: cfBar.height - 12; color: "#1c1c1c"
            Layout.rightMargin: 5
        }

        // Preset buttons
        Repeater {
            model: [
                { label: "LIN", s: 0.0 },
                { label: "SHP", s: 0.5 },
                { label: "CUT", s: 1.0 }
            ]
            delegate: Rectangle {
                required property var modelData
                required property int index
                Layout.preferredWidth: 28; Layout.preferredHeight: 20
                radius: 2
                readonly property bool isActive: Math.abs(cfBar.cfSharpness - modelData.s) < 0.02
                color:        isActive ? "#171717" : "#0d0d0d"
                border.width: 1; border.color: isActive ? "#3a7ad4" : "#1c1c1c"
                Text {
                    anchors.centerIn: parent; text: parent.modelData.label
                    color: parent.isActive ? "#7ab8f5" : "#444"
                    font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace"
                }
                HoverHandler { id: ph }
                Rectangle { anchors.fill: parent; radius: 2; color: "#fff"
                    opacity: ph.hovered && !parent.isActive ? 0.04 : 0 }
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    onClicked: { cfBar.cfSharpness = modelData.s; curveDial.value = modelData.s }
                }
            }
        }

        Item { Layout.preferredWidth: 4 }

        // Manual sharpness knob
        Item {
            Layout.preferredWidth: 22; Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignVCenter

            Dial {
                id: curveDial
                anchors.fill: parent
                from: 0.0; to: 1.0; value: 0.0; stepSize: 0.01
                onValueChanged: cfBar.cfSharpness = value

                MouseArea {
                    id: curveDragLock
                    anchors.fill: parent
                    z: 100
                    acceptedButtons: Qt.LeftButton
                    preventStealing: true

                    property real _pressGX:  0
                    property real _pressGY:  0
                    property real _pressVal: 0
                    property bool _active:   false

                    onPressed: (mouse) => {
                        var g    = curveDragLock.mapToGlobal(mouse.x, mouse.y)
                        _pressGX  = g.x
                        _pressGY  = g.y
                        _pressVal = curveDial.value
                        _active   = false
                        mouse.accepted = true
                    }

                    onPositionChanged: (mouse) => {
                        var g  = curveDragLock.mapToGlobal(mouse.x, mouse.y)
                        var dy = _pressGY - g.y
                        if (!_active) {
                            if (Math.abs(dy) < 4) return
                            _active = true
                            cursorControl.hideCursor()
                        }
                        var newVal = _pressVal + dy * (curveDial.to - curveDial.from) / 150.0
                        curveDial.value = Math.min(curveDial.to, Math.max(curveDial.from, newVal))
                    }

                    onReleased: {
                        if (_active) {
                            _active = false
                            cursorControl.restoreCursor()
                            cursorControl.moveCursor(_pressGX, _pressGY)
                        }
                    }

                    onDoubleClicked: { curveDial.value = 0.0 }
                }

                background: Rectangle {
                    x: curveDial.width/2 - width/2; y: curveDial.height/2 - height/2
                    width: curveDial.width; height: curveDial.height
                    radius: width/2; color: "transparent"
                    Canvas {
                        id: dialArc; anchors.fill: parent; antialiasing: true
                        onPaint: {
                            var ctx = getContext("2d"); ctx.reset()
                            var cx = width/2; var cy = height/2
                            var r  = Math.min(width,height) * 0.42
                            var s0 = 120 * Math.PI/180; var span = 300 * Math.PI/180
                            ctx.lineWidth = 2; ctx.lineCap = "round"
                            ctx.strokeStyle = "#222"
                            ctx.beginPath(); ctx.arc(cx,cy,r, s0, s0+span, false); ctx.stroke()
                            ctx.strokeStyle = "#3a7ad4"
                            ctx.beginPath(); ctx.arc(cx,cy,r, s0, s0 + curveDial.value*span, false); ctx.stroke()
                        }
                        Connections { target: curveDial; function onValueChanged() { dialArc.requestPaint() } }
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width*0.72; height: parent.height*0.72
                        radius: width/2; color: "#141414"
                    }
                }
                handle: Rectangle {
                    id: dh
                    x: curveDial.background.x + curveDial.background.width /2 - width /2
                    y: curveDial.background.y + curveDial.background.height/2 - height/2
                    width: curveDial.width*0.72; height: curveDial.height*0.72; color: "transparent"
                    Rectangle {
                        color: "#aaa"; width: 1; height: parent.height*0.38
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top; anchors.topMargin: -1
                    }
                    transform: Rotation { angle: curveDial.angle; origin.x: dh.width/2; origin.y: dh.height/2 }
                }
            }
        }

        Item { Layout.preferredWidth: 4 }

        // Curve visualizer (same formula as cfMult)
        Canvas {
            id: curveViz
            Layout.preferredWidth: 58; Layout.preferredHeight: 24
            Layout.alignment: Qt.AlignVCenter
            antialiasing: true

            onPaint: {
                var ctx = getContext("2d"); ctx.reset()
                var w = width; var h = height
                var e = Math.pow(10.0, -cfBar.cfSharpness * 2.0)
                var pad = 2

                ctx.fillStyle = "#0a0a0a"; ctx.fillRect(0,0,w,h)
                ctx.strokeStyle = "#1c1c1c"; ctx.lineWidth = 1
                ctx.strokeRect(0.5, 0.5, w-1, h-1)
                // center divider
                ctx.strokeStyle = "#252525"; ctx.lineWidth = 0.5
                ctx.beginPath(); ctx.moveTo(w/2, pad); ctx.lineTo(w/2, h-pad); ctx.stroke()

                function drawCurve(color, isA) {
                    ctx.strokeStyle = color; ctx.lineWidth = 1.3
                    ctx.beginPath()
                    for (var i = 0; i <= w; i++) {
                        var xn  = (i / w) * 2.0 - 1.0   // -1..+1
                        var t   = (xn + 1.0) * 0.5        // 0..1
                        var vol = isA ? Math.pow(Math.max(0,1-t), e) : Math.pow(Math.max(0,t), e)
                        var py  = (h - pad) - vol * (h - pad*2)
                        if (i === 0) ctx.moveTo(i, py); else ctx.lineTo(i, py)
                    }
                    ctx.stroke()
                }
                drawCurve("#ff9900", true)   // A curve (orange)
                drawCurve("#00ccff", false)  // B curve (cyan)
            }

            Connections {
                target: cfBar
                function onCfSharpnessChanged() { curveViz.requestPaint() }
            }
        }
    }

    // ── Crossfader + assigns — truly centered in the full bar ─────────────
    RowLayout {
        id: centerGroup
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height
        spacing: 0

        // A-side assigns
        AssignGroup {
            deckLabel: "A"; deckAccent: cfBar.clrA; currentAssign: cfBar.assignA
            Layout.alignment: Qt.AlignVCenter
            onPicked: (v) => { cfBar.assignA = v }
        }
        AssignGroup {
            visible: cfBar.fourDeckMode; Layout.preferredWidth: visible ? implicitWidth : 0
            deckLabel: "C"; deckAccent: cfBar.clrC; currentAssign: cfBar.assignC
            Layout.leftMargin: visible ? 6 : 0; Layout.alignment: Qt.AlignVCenter
            onPicked: (v) => { cfBar.assignC = v }
        }

        Item { Layout.preferredWidth: 10 }

        // Crossfader
        Item {
            Layout.preferredWidth: 220; Layout.fillHeight: true

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 0; width: 1; height: 4; color: "#2a2a2a"
            }
            Text {
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: "◄"; color: cfBar.clrA; font.pixelSize: window.spViewport(8)
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                text: "►"; color: cfBar.clrB; font.pixelSize: window.spViewport(8)
            }

            Slider {
                id: cfSlider
                anchors.left: parent.left;   anchors.leftMargin:  14
                anchors.right: parent.right; anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                height: 22
                from: -1.0; to: 1.0; value: 0.0; stepSize: 0.005
                onValueChanged: cfBar.cfPos = value

                property bool cfDragActive: false

                background: Rectangle {
                    x: cfSlider.leftPadding; y: cfSlider.height/2 - height/2
                    width: cfSlider.availableWidth; height: 4
                    radius: 1; color: "#161616"; border.width: 1; border.color: "#0a0a0a"
                    Rectangle {
                        y: 1; height: parent.height - 2; radius: 1
                        color: cfSlider.pressed ? "#4a4a4a" : "#303030"
                        readonly property real mid: parent.width / 2
                        readonly property real pos: 1 + cfSlider.visualPosition * (parent.width - 2)
                        x: Math.min(mid, pos); width: Math.max(0, Math.abs(pos - mid))
                    }
                }

                handle: Rectangle {
                    x: cfSlider.leftPadding + cfSlider.visualPosition * (cfSlider.availableWidth - width)
                    y: cfSlider.height / 2 - height / 2
                    implicitWidth: 18; implicitHeight: 22; radius: 2
                    color: cfSlider.pressed || cfSlider.cfDragActive ? "#f0f0f0" : "#d0d0d0"
                    border.width: 1; border.color: "#888"
                    Rectangle { anchors.centerIn: parent; width: 2; height: parent.height * 0.48; color: "#888" }
                }

                MouseArea {
                    id: cfDragLock
                    anchors.fill: parent
                    z: 100
                    acceptedButtons: Qt.LeftButton
                    preventStealing: true

                    property real _pressGX:  0
                    property real _pressGY:  0
                    property real _pressVal: 0
                    property bool _active:   false

                    onPressed: (mouse) => {
                        var g    = cfDragLock.mapToGlobal(mouse.x, mouse.y)
                        _pressGX  = g.x
                        _pressGY  = g.y
                        _pressVal = cfSlider.value
                        _active   = false
                        mouse.accepted = true
                    }

                    onPositionChanged: (mouse) => {
                        var g     = cfDragLock.mapToGlobal(mouse.x, mouse.y)
                        var delta = g.x - _pressGX
                        if (!_active) {
                            if (Math.abs(delta) < 4) return
                            _active = true
                            cfSlider.cfDragActive = true
                            cursorControl.hideCursor()
                        }
                        var newVal = _pressVal + delta * (cfSlider.to - cfSlider.from) / 200.0
                        cfSlider.value = Math.max(cfSlider.from, Math.min(cfSlider.to, newVal))
                    }

                    onReleased: {
                        if (_active) {
                            _active = false
                            cfSlider.cfDragActive = false
                            cursorControl.restoreCursor()
                            cursorControl.moveCursor(_pressGX, _pressGY)
                        }
                    }

                    onDoubleClicked: {
                        cfSlider.enabled = false
                        cfSlider.value   = 0.0
                        cfSlider.enabled = true
                    }
                }
            }
        }

        Item { Layout.preferredWidth: 10 }

        // B-side assigns
        AssignGroup {
            visible: cfBar.fourDeckMode; Layout.preferredWidth: visible ? implicitWidth : 0
            deckLabel: "D"; deckAccent: cfBar.clrD; currentAssign: cfBar.assignD
            Layout.rightMargin: visible ? 6 : 0; Layout.alignment: Qt.AlignVCenter
            onPicked: (v) => { cfBar.assignD = v }
        }
        AssignGroup {
            deckLabel: "B"; deckAccent: cfBar.clrB; currentAssign: cfBar.assignB
            Layout.alignment: Qt.AlignVCenter
            onPicked: (v) => { cfBar.assignB = v }
        }
    }
}
