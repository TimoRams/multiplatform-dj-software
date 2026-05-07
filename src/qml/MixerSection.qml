import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: mixer
    color: "#0f0f0f"

    property var engineA: null
    property var engineB: null
    property bool cueAActive: false
    property bool cueBActive: false
    property string channelAId: "deckA"
    property string channelBId: "deckB"

    readonly property real vuACombined: engineA ? Math.max(engineA.preFaderVuLevelL, engineA.preFaderVuLevelR) : 0.0
    readonly property real vuBCombined: engineB ? Math.max(engineB.preFaderVuLevelL, engineB.preFaderVuLevelR) : 0.0

    readonly property color clrA: "#ff9900"
    readonly property color clrB: "#00ccff"

    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if      (id === mixer.channelAId + "_vol")  volFaderA.value = value
            else if (id === mixer.channelBId + "_vol")  volFaderB.value = value
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
        property real  knobSize:    Math.max(18, Math.min(22, mixer.width * 0.12))
        property real  labelSpace:  11
        property real  defaultValue: (dial.from + dial.to) / 2
        property real  columnWidth: knobSize + 18
        property string labelSide: "left"
        property color accentColor: "#2d7dd2"

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
                        ctx.lineWidth  = Math.max(2, Math.round(width * 0.08))
                        ctx.lineCap    = "round"
                        ctx.strokeStyle = "#222"
                        ctx.beginPath()
                        ctx.arc(cx, cy, r, startDeg * Math.PI / 180, (startDeg + spanDeg) * Math.PI / 180, false)
                        ctx.stroke()
                        ctx.strokeStyle = knobRoot.accentColor
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
                    width: parent.width * 0.78; height: parent.height * 0.78
                    radius: width / 2
                    color: "#1a1a1a"
                }
            }

            handle: Rectangle {
                id: handleItem
                x: dial.background.x + dial.background.width / 2 - width / 2
                y: dial.background.y + dial.background.height / 2 - height / 2
                width: dial.width * 0.78; height: dial.height * 0.78
                color: "transparent"

                Rectangle {
                    color: "#cccccc"
                    width: 1.5; height: parent.height * 0.42
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
            anchors.top: dial.bottom; anchors.topMargin: 2
            color: "#777"
            font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace"
            font.letterSpacing: 0.3
        }
    }

    // ── VU meter component ────────────────────────────────────────────────
    component VuMeterVertical: Rectangle {
        id: vuMeter
        required property real   levelLinear
        required property string deckName

        width: 10
        color: "#060606"
        radius: 1

        property real peakHoldLevel: 0.0
        readonly property int totalSegments: 24
        readonly property real segmentH: (height - (totalSegments - 1)) / totalSegments

        function getBarColor(seg) {
            if (seg >= Math.floor(totalSegments * 0.88)) return "#e03535"
            if (seg >= Math.floor(totalSegments * 0.72)) return "#d48000"
            if (seg >= Math.floor(totalSegments * 0.50)) return "#5aba52"
            return "#2a9640"
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
            anchors.fill: parent; spacing: 1
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
            radius: 1; color: "#161616"
            border.width: 1; border.color: "#0a0a0a"

            Rectangle {
                visible: control.orientation === Qt.Horizontal && control.centerFill
                y: 1; height: parent.height - 2; radius: 1
                color: control.pressed ? "#4a4a4a" : "#303030"
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
                radius: 1; color: control.pressed ? "#555" : "#363636"
            }
        }

        handle: Rectangle {
            implicitWidth:  control.orientation === Qt.Vertical ? 26 : 18
            implicitHeight: control.orientation === Qt.Vertical ? 12 : 22
            x: control.orientation === Qt.Horizontal
               ? control.leftPadding + control.visualPosition * (control.availableWidth - width)
               : control.width / 2 - width / 2
            y: control.orientation === Qt.Horizontal
               ? control.height / 2 - height / 2
               : control.topPadding + control.visualPosition * (control.availableHeight - height)
            radius: 2
            color: control.pressed ? "#f0f0f0" : "#d0d0d0"
            border.width: 1; border.color: "#888"

            Rectangle {
                anchors.centerIn: parent
                width: control.orientation === Qt.Vertical ? parent.width * 0.48 : 2
                height: control.orientation === Qt.Vertical ? 1 : parent.height * 0.48
                color: "#888"
            }
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
                    Layout.preferredHeight: 26
                    color: "#0a0a0a"

                    Rectangle { width: 3; height: parent.height; color: mixer.clrA; opacity: 0.85 }

                    Text {
                        anchors.centerIn: parent
                        text: "A"; color: mixer.clrA
                        font.pixelSize: window.spViewport(11); font.bold: true; font.family: "monospace"
                        font.letterSpacing: 2.0
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
                }

                // EQ knobs
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 1

                    MixerKnob { text: "T"; from: 0; to: 2; knobValue: 1.0; accentColor: "#c87010"
                        onKnobValueChanged: { if (engineA) engineA.trim = knobValue } }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1
                        color: "#1c1c1c"; Layout.leftMargin: 6; Layout.rightMargin: 6 }

                    MixerKnob { text: "H"; from: -1; to: 1; knobValue: 0; accentColor: "#c87010"
                        onKnobValueChanged: { if (engineA) engineA.eqHigh = knobValue } }
                    MixerKnob { text: "M"; from: -1; to: 1; knobValue: 0; accentColor: "#c87010"
                        onKnobValueChanged: { if (engineA) engineA.eqMid = knobValue } }
                    MixerKnob { text: "L"; from: -1; to: 1; knobValue: 0; accentColor: "#c87010"
                        onKnobValueChanged: { if (engineA) engineA.eqLow = knobValue } }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1
                        color: "#1c1c1c"; Layout.leftMargin: 6; Layout.rightMargin: 6 }

                    MixerKnob {
                        id: scKnobA; text: "SC"; from: -1; to: 1; knobValue: 0.0; defaultValue: 0.0
                        accentColor: "#b06010"
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

                // VU + CUE + Fader (VU spans full height)
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    spacing: 0

                    VuMeterVertical {
                        id: vuAMeter
                        Layout.fillHeight: true; Layout.preferredWidth: 10
                        levelLinear: mixer.vuACombined; deckName: "A"
                    }

                    Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                    ColumnLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 22
                            color: mixer.cueAActive ? "#0c2016" : "#141414"
                            border.width: 1
                            border.color: mixer.cueAActive ? "#1e5030" : "#1c1c1c"

                            HoverHandler { id: cueAHov }
                            Rectangle {
                                anchors.fill: parent
                                color: "#ffffff"; opacity: cueAHov.hovered && !mixer.cueAActive ? 0.04 : 0
                            }

                            Text {
                                anchors.centerIn: parent; text: "CUE"
                                color: mixer.cueAActive ? "#5de89a" : "#606060"
                                font.pixelSize: window.spViewport(9); font.bold: true; font.family: "monospace"
                                font.letterSpacing: 0.8
                            }

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left; anchors.right: parent.right
                                height: 2; color: "#4dd98a"; visible: mixer.cueAActive
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

                        MixerSlider {
                            id: volFaderA
                            Layout.fillHeight: true; Layout.fillWidth: true
                            orientation: Qt.Vertical
                            from: 0.0; to: 1.0; value: 1.0
                            onValueChanged: {
                                if (parameterStore && parameterStore.getParameter(mixer.channelAId + "_vol") !== value)
                                    parameterStore.setParameter(mixer.channelAId + "_vol", value)
                            }
                            TapHandler {
                                onDoubleTapped: { volFaderA.enabled = false; volFaderA.value = 1.0; volFaderA.enabled = true }
                            }
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
                    Layout.preferredHeight: 26
                    color: "#0a0a0a"

                    Rectangle {
                        anchors.right: parent.right
                        width: 3; height: parent.height; color: mixer.clrB; opacity: 0.85
                    }

                    Text {
                        anchors.centerIn: parent
                        text: "B"; color: mixer.clrB
                        font.pixelSize: window.spViewport(11); font.bold: true; font.family: "monospace"
                        font.letterSpacing: 2.0
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
                }

                // EQ knobs
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 1

                    MixerKnob { text: "T"; from: 0; to: 2; knobValue: 1.0; accentColor: "#1a6ab8"
                        onKnobValueChanged: { if (engineB) engineB.trim = knobValue } }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1
                        color: "#1c1c1c"; Layout.leftMargin: 6; Layout.rightMargin: 6 }

                    MixerKnob { text: "H"; from: -1; to: 1; knobValue: 0; accentColor: "#1a6ab8"
                        onKnobValueChanged: { if (engineB) engineB.eqHigh = knobValue } }
                    MixerKnob { text: "M"; from: -1; to: 1; knobValue: 0; accentColor: "#1a6ab8"
                        onKnobValueChanged: { if (engineB) engineB.eqMid = knobValue } }
                    MixerKnob { text: "L"; from: -1; to: 1; knobValue: 0; accentColor: "#1a6ab8"
                        onKnobValueChanged: { if (engineB) engineB.eqLow = knobValue } }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1
                        color: "#1c1c1c"; Layout.leftMargin: 6; Layout.rightMargin: 6 }

                    MixerKnob {
                        id: scKnobB; text: "SC"; from: -1; to: 1; knobValue: 0.0; defaultValue: 0.0
                        accentColor: "#155a9e"
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

                // VU + CUE + Fader (VU spans full height)
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    spacing: 0

                    ColumnLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 22
                            color: mixer.cueBActive ? "#0c2016" : "#141414"
                            border.width: 1
                            border.color: mixer.cueBActive ? "#1e5030" : "#1c1c1c"

                            HoverHandler { id: cueBHov }
                            Rectangle {
                                anchors.fill: parent
                                color: "#ffffff"; opacity: cueBHov.hovered && !mixer.cueBActive ? 0.04 : 0
                            }

                            Text {
                                anchors.centerIn: parent; text: "CUE"
                                color: mixer.cueBActive ? "#5de89a" : "#606060"
                                font.pixelSize: window.spViewport(9); font.bold: true; font.family: "monospace"
                                font.letterSpacing: 0.8
                            }

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left; anchors.right: parent.right
                                height: 2; color: "#4dd98a"; visible: mixer.cueBActive
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

                        MixerSlider {
                            id: volFaderB
                            Layout.fillHeight: true; Layout.fillWidth: true
                            orientation: Qt.Vertical
                            from: 0.0; to: 1.0; value: 1.0
                            onValueChanged: {
                                if (parameterStore && parameterStore.getParameter(mixer.channelBId + "_vol") !== value)
                                    parameterStore.setParameter(mixer.channelBId + "_vol", value)
                            }
                            TapHandler {
                                onDoubleTapped: { volFaderB.enabled = false; volFaderB.value = 1.0; volFaderB.enabled = true }
                            }
                        }
                    }

                    Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                    VuMeterVertical {
                        id: vuBMeter
                        Layout.fillHeight: true; Layout.preferredWidth: 10
                        levelLinear: mixer.vuBCombined; deckName: "B"
                    }
                }
            }
        }

    }
}
