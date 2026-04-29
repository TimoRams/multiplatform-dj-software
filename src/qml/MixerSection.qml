import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: mixer
    color: "#111111"

    property var engineA: null
    property var engineB: null
    property bool cueAActive: false
    property bool cueBActive: false

    property real volA: 0.8
    property real volB: 0.8
    readonly property real vuACombined: engineA ? Math.max(engineA.preFaderVuLevelL, engineA.preFaderVuLevelR) : 0.0
    readonly property real vuBCombined: engineB ? Math.max(engineB.preFaderVuLevelL, engineB.preFaderVuLevelR) : 0.0

    function updateVolumes() {
        if (engineA) {
            let cfA = crossfader.value > 0 ? 1.0 - crossfader.value : 1.0
            engineA.volume = volA * cfA
        }
        if (engineB) {
            let cfB = crossfader.value < 0 ? 1.0 + crossfader.value : 1.0
            engineB.volume = volB * cfB
        }
    }

    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if      (id === "deckA_vol")   volFaderA.value = value
            else if (id === "deckB_vol")   volFaderB.value = value
            else if (id === "crossfader")  crossfader.value = (value * 2.0) - 1.0
        }
    }

    Connections {
        target: engineA
        function onCueEnabledChanged() { mixer.cueAActive = engineA ? engineA.cueEnabled : false }
    }
    Connections {
        target: engineB
        function onCueEnabledChanged() { mixer.cueBActive = engineB ? engineB.cueEnabled : false }
    }

    // ── Knob component ───────────────────────────────────────────────────
    component MixerKnob: Item {
        id: knobRoot
        property alias text: label.text
        property alias from: dial.from
        property alias to:   dial.to
        property alias knobValue: dial.value
        property real  knobSize:    28
        property real  labelSpace:  8
        property real  defaultValue: (dial.from + dial.to) / 2
        property real  columnWidth: 40
        property string labelSide: "left"

        Layout.preferredWidth:  columnWidth
        Layout.minimumWidth:    columnWidth
        Layout.maximumWidth:    columnWidth
        Layout.preferredHeight: knobSize + labelSpace
        Layout.minimumHeight:   knobSize + labelSpace
        Layout.maximumHeight:   knobSize + labelSpace
        Layout.alignment: Qt.AlignHCenter

        Dial {
            id: dial
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: knobRoot.knobSize; height: knobRoot.knobSize

            background: Rectangle {
                x: dial.width / 2 - width / 2; y: dial.height / 2 - height / 2
                width: dial.width; height: dial.height
                radius: width / 2
                color: "transparent"; border.color: "transparent"

                Canvas {
                    id: knobArc
                    anchors.fill: parent
                    antialiasing: true
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        var cx = width / 2; var cy = height / 2
                        var r  = Math.min(width, height) * 0.44
                        var startDeg = 120; var spanDeg = 300
                        function clamp01(v) { return Math.max(0, Math.min(1, v)) }
                        var from = dial.from; var to = dial.to
                        var norm = clamp01((dial.value - from) / (to - from))
                        ctx.lineWidth  = Math.max(2, Math.round(width * 0.07))
                        ctx.lineCap    = "round"
                        // Track arc (dim)
                        ctx.strokeStyle = "#2a2a2a"
                        ctx.beginPath()
                        ctx.arc(cx, cy, r, startDeg * Math.PI / 180, (startDeg + spanDeg) * Math.PI / 180, false)
                        ctx.stroke()
                        // Value arc (accent)
                        ctx.strokeStyle = "#2d7dd2"
                        if (from < 0 && to > 0) {
                            var neutral = clamp01((0 - from) / (to - from))
                            var a0 = (startDeg + neutral * spanDeg) * Math.PI / 180
                            var a1 = (startDeg + norm    * spanDeg) * Math.PI / 180
                            ctx.beginPath(); ctx.arc(cx, cy, r, a0, a1, norm < neutral); ctx.stroke()
                        } else {
                            var s = startDeg * Math.PI / 180
                            var e = (startDeg + norm * spanDeg) * Math.PI / 180
                            ctx.beginPath(); ctx.arc(cx, cy, r, s, e, false); ctx.stroke()
                        }
                    }
                    Connections {
                        target: dial
                        function onValueChanged() { knobArc.requestPaint() }
                        function onFromChanged()  { knobArc.requestPaint() }
                        function onToChanged()    { knobArc.requestPaint() }
                    }
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.82; height: parent.height * 0.82
                    radius: width / 2
                    color: "#1a1a1a"
                }
            }

            handle: Rectangle {
                id: handleItem
                x: dial.background.x + dial.background.width / 2 - width / 2
                y: dial.background.y + dial.background.height / 2 - height / 2
                width: dial.width * 0.82; height: dial.height * 0.82
                color: "transparent"

                Rectangle {
                    color: "#c0c0c0"
                    width: 2; height: parent.height * 0.44
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top; anchors.topMargin: -1
                }
                transform: Rotation {
                    angle: dial.angle
                    origin.x: handleItem.width / 2; origin.y: handleItem.height / 2
                }
            }

            TapHandler {
                onDoubleTapped: { dial.enabled = false; dial.value = knobRoot.defaultValue; dial.enabled = true }
            }
        }

        Text {
            id: label
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: dial.bottom; anchors.topMargin: 1
            color: "#555"
            font.pixelSize: window.spViewport(6); font.bold: true; font.family: "monospace"
        }
    }

    // ── VU meter component ────────────────────────────────────────────────
    component VuMeterVertical: Rectangle {
        id: vuMeter
        required property real   levelLinear
        required property string deckName

        width: 6
        color: "#0a0a0a"
        radius: 0

        property real peakHoldLevel: 0.0
        readonly property int totalSegments: 24
        readonly property real segmentH: height / totalSegments

        function getBarColor(seg) {
            if (seg >= Math.floor(totalSegments * 0.88)) return "#e03030"
            if (seg >= Math.floor(totalSegments * 0.72)) return "#e08a00"
            return "#2d7dd2"
        }

        onLevelLinearChanged: {
            if (levelLinear > peakHoldLevel) { peakHoldLevel = levelLinear; decayTimer.restart() }
        }

        Timer    { id: decayTimer; interval: 350; onTriggered: decayAnim.start() }
        NumberAnimation {
            id: decayAnim; target: vuMeter; property: "peakHoldLevel"
            from: vuMeter.peakHoldLevel; to: 0.0; duration: 900; easing.type: Easing.InQuad
        }

        Column {
            anchors.fill: parent; spacing: 0
            Repeater {
                model: vuMeter.totalSegments
                delegate: Rectangle {
                    required property int index
                    width: vuMeter.width
                    height: vuMeter.segmentH
                    radius: 0
                    color: {
                        const ri = vuMeter.totalSegments - 1 - index
                        const lit  = Math.floor(vuMeter.levelLinear  * vuMeter.totalSegments)
                        const peak = Math.floor(vuMeter.peakHoldLevel * vuMeter.totalSegments) - 1
                        if (ri === peak && peak >= 0) return "#ffffff"
                        if (ri < lit)                 return vuMeter.getBarColor(ri)
                        return "#141414"
                    }
                }
            }
        }
    }

    // ── Volume/crossfader slider component ───────────────────────────────
    component MixerSlider: Slider {
        id: control
        property bool centerFill: false

        background: Rectangle {
            x: control.orientation === Qt.Horizontal ? control.leftPadding : control.width / 2 - 2
            y: control.orientation === Qt.Horizontal ? control.height / 2 - 2 : control.topPadding
            width:  control.orientation === Qt.Horizontal ? control.availableWidth : 4
            height: control.orientation === Qt.Horizontal ? 4 : control.availableHeight
            radius: 0; color: "#1a1a1a"

            Rectangle {
                visible: control.orientation === Qt.Horizontal && control.centerFill
                y: 1; height: parent.height - 2; radius: 0
                color: control.pressed ? "#4a4a4a" : "#333"
                readonly property real midPx: parent.width / 2
                readonly property real posPx: 1 + control.visualPosition * (parent.width - 2)
                x: Math.min(midPx, posPx)
                width: Math.max(0, Math.abs(posPx - midPx))
            }
            Rectangle {
                visible: control.orientation === Qt.Vertical
                x: 1
                y: parent.height - 1 - Math.max(0, (1.0 - control.visualPosition) * (parent.height - 2))
                width: parent.width - 2
                height: Math.max(0, (1.0 - control.visualPosition) * (parent.height - 2))
                radius: 0; color: control.pressed ? "#4a4a4a" : "#333"
            }
        }

        handle: Rectangle {
            implicitWidth:  control.orientation === Qt.Vertical ? 26 : 18
            implicitHeight: control.orientation === Qt.Vertical ? 10 : 22
            x: control.orientation === Qt.Horizontal
               ? control.leftPadding + control.visualPosition * (control.availableWidth - width)
               : control.width / 2 - width / 2
            y: control.orientation === Qt.Horizontal
               ? control.height / 2 - height / 2
               : control.topPadding + control.visualPosition * (control.availableHeight - height)
            radius: 0; color: control.pressed ? "#e8e8e8" : "#c8c8c8"
        }
    }

    // ── Layout ────────────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 0
        spacing: 0

        // Channel strips
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ── Channel A ─────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // Channel label
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 16
                    color: "#0d0d0d"

                    Text {
                        anchors.centerIn: parent
                        text: "A"; color: "#ff9900"
                        font.pixelSize: window.spViewport(9); font.bold: true; font.family: "monospace"
                        font.letterSpacing: 1.0
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
                }

                // EQ knobs
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 2

                    MixerKnob { text: "T"; from: 0; to: 2; knobValue: 1.0; knobSize: 22; labelSpace: 6; onKnobValueChanged: { if (engineA) engineA.trim = knobValue } }
                    MixerKnob { text: "H"; from: -1; to: 1; knobValue: 0; knobSize: 22; labelSpace: 6; onKnobValueChanged: { if (engineA) engineA.eqHigh = knobValue } }
                    MixerKnob { text: "M"; from: -1; to: 1; knobValue: 0; knobSize: 22; labelSpace: 6; onKnobValueChanged: { if (engineA) engineA.eqMid = knobValue } }
                    MixerKnob { text: "L"; from: -1; to: 1; knobValue: 0; knobSize: 22; labelSpace: 6; onKnobValueChanged: { if (engineA) engineA.eqLow = knobValue } }
                    MixerKnob {
                        id: scKnobA; text: "SC"; from: -1; to: 1; knobValue: 0.0; knobSize: 22; labelSpace: 6; defaultValue: 0.0
                        onKnobValueChanged: {
                            if (typeof fxManager !== "undefined") {
                                fxManager.setSoundColorDeck(1, knobValue)
                                if (fxManager.soundColorMode === "Filter") { if (engineA) engineA.filter = knobValue }
                                else { if (engineA) engineA.filter = 0.0 }
                            } else { if (engineA) engineA.filter = knobValue }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

                // CUE button
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 22
                    color: mixer.cueAActive ? "#0d2a1a" : "#1c1c1c"

                    HoverHandler { id: cueAHov }
                    Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: cueAHov.hovered ? 0.04 : 0; visible: !mixer.cueAActive }

                    Text {
                        anchors.centerIn: parent; text: "CUE"
                        color: mixer.cueAActive ? "#4dd98a" : "#888"
                        font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace"
                    }

                    // Active indicator bar
                    Rectangle {
                        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                        width: 2; color: "#4dd98a"; visible: mixer.cueAActive
                    }

                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            mixer.cueAActive = !mixer.cueAActive
                            if (engineA) engineA.cueEnabled = mixer.cueAActive
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

                // Fader + VU
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    spacing: 0

                    VuMeterVertical {
                        id: vuAMeter
                        Layout.fillHeight: true; Layout.preferredWidth: 6
                        levelLinear: mixer.vuACombined; deckName: "A"
                    }

                    Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                    MixerSlider {
                        id: volFaderA
                        Layout.fillHeight: true; Layout.fillWidth: true
                        orientation: Qt.Vertical
                        from: 0.0; to: 1.0; value: 1.0
                        onValueChanged: {
                            mixer.volA = value; mixer.updateVolumes()
                            if (parameterStore && parameterStore.getParameter("deckA_vol") !== value)
                                parameterStore.setParameter("deckA_vol", value)
                        }
                        TapHandler {
                            onDoubleTapped: { volFaderA.enabled = false; volFaderA.value = 1.0; volFaderA.enabled = true }
                        }
                    }
                }
            }

            Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

            // ── Channel B ─────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // Channel label
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 16
                    color: "#0d0d0d"

                    Text {
                        anchors.centerIn: parent
                        text: "B"; color: "#00ccff"
                        font.pixelSize: window.spViewport(9); font.bold: true; font.family: "monospace"
                        font.letterSpacing: 1.0
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
                }

                // EQ knobs
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 2

                    MixerKnob { text: "T"; from: 0; to: 2; knobValue: 1.0; knobSize: 22; labelSpace: 6; onKnobValueChanged: { if (engineB) engineB.trim = knobValue } }
                    MixerKnob { text: "H"; from: -1; to: 1; knobValue: 0; knobSize: 22; labelSpace: 6; onKnobValueChanged: { if (engineB) engineB.eqHigh = knobValue } }
                    MixerKnob { text: "M"; from: -1; to: 1; knobValue: 0; knobSize: 22; labelSpace: 6; onKnobValueChanged: { if (engineB) engineB.eqMid = knobValue } }
                    MixerKnob { text: "L"; from: -1; to: 1; knobValue: 0; knobSize: 22; labelSpace: 6; onKnobValueChanged: { if (engineB) engineB.eqLow = knobValue } }
                    MixerKnob {
                        id: scKnobB; text: "SC"; from: -1; to: 1; knobValue: 0.0; knobSize: 22; labelSpace: 6; defaultValue: 0.0
                        onKnobValueChanged: {
                            if (typeof fxManager !== "undefined") {
                                fxManager.setSoundColorDeck(2, knobValue)
                                if (fxManager.soundColorMode === "Filter") { if (engineB) engineB.filter = knobValue }
                                else { if (engineB) engineB.filter = 0.0 }
                            } else { if (engineB) engineB.filter = knobValue }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

                // CUE button
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 22
                    color: mixer.cueBActive ? "#0d2a1a" : "#1c1c1c"

                    HoverHandler { id: cueBHov }
                    Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: cueBHov.hovered ? 0.04 : 0; visible: !mixer.cueBActive }

                    Text {
                        anchors.centerIn: parent; text: "CUE"
                        color: mixer.cueBActive ? "#4dd98a" : "#888"
                        font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace"
                    }

                    Rectangle {
                        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                        width: 2; color: "#4dd98a"; visible: mixer.cueBActive
                    }

                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            mixer.cueBActive = !mixer.cueBActive
                            if (engineB) engineB.cueEnabled = mixer.cueBActive
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

                // Fader + VU
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    spacing: 0

                    MixerSlider {
                        id: volFaderB
                        Layout.fillHeight: true; Layout.fillWidth: true
                        orientation: Qt.Vertical
                        from: 0.0; to: 1.0; value: 1.0
                        onValueChanged: {
                            mixer.volB = value; mixer.updateVolumes()
                            if (parameterStore && parameterStore.getParameter("deckB_vol") !== value)
                                parameterStore.setParameter("deckB_vol", value)
                        }
                        TapHandler {
                            onDoubleTapped: { volFaderB.enabled = false; volFaderB.value = 1.0; volFaderB.enabled = true }
                        }
                    }

                    Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                    VuMeterVertical {
                        id: vuBMeter
                        Layout.fillHeight: true; Layout.preferredWidth: 6
                        levelLinear: mixer.vuBCombined; deckName: "B"
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

        // ── Crossfader ────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: "#0d0d0d"

            Row {
                anchors.left: parent.left; anchors.leftMargin: 4
                anchors.verticalCenter: parent.verticalCenter; spacing: 2
                Text { text: "A"; color: "#ff9900"; font.pixelSize: window.spViewport(6); font.bold: true; font.family: "monospace"; anchors.verticalCenter: parent.verticalCenter }
            }
            Row {
                anchors.right: parent.right; anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                Text { text: "B"; color: "#00ccff"; font.pixelSize: window.spViewport(6); font.bold: true; font.family: "monospace"; anchors.verticalCenter: parent.verticalCenter }
            }

            MixerSlider {
                id: crossfader
                anchors.left: parent.left; anchors.right: parent.right
                anchors.leftMargin: 14; anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                height: 20
                centerFill: true
                from: -1.0; to: 1.0; value: 0.0; stepSize: 0.01
                onValueChanged: {
                    mixer.updateVolumes()
                    if (parameterStore) {
                        var normValue = (value + 1.0) / 2.0
                        if (Math.abs(parameterStore.getParameter("crossfader") - normValue) > 0.01)
                            parameterStore.setParameter("crossfader", normValue)
                    }
                }
                TapHandler {
                    onDoubleTapped: { crossfader.enabled = false; crossfader.value = 0.0; crossfader.enabled = true }
                }
            }
        }
    }
}
