import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: mixer
    color: "#181818"
    
    property var engineA: null
    property var engineB: null
    property bool cueAActive: false
    property bool cueBActive: false
    
    // Internal volumes for the faders (so crossfader can modify the actual engine volume)
    property real volA: 0.8
    property real volB: 0.8
    readonly property real vuACombined: engineA ? Math.max(engineA.preFaderVuLevelL, engineA.preFaderVuLevelR) : 0.0
    readonly property real vuBCombined: engineB ? Math.max(engineB.preFaderVuLevelL, engineB.preFaderVuLevelR) : 0.0
    
    function updateVolumes() {
        if(engineA) {
            let cfA = crossfader.value > 0 ? 1.0 - crossfader.value : 1.0;
            engineA.volume = volA * cfA;
        }
        if(engineB) {
            let cfB = crossfader.value < 0 ? 1.0 + crossfader.value : 1.0;
            engineB.volume = volB * cfB;
        }
    }

    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if (id === "deckA_vol") {
                volFaderA.value = value
            } else if (id === "deckB_vol") {
                volFaderB.value = value
            } else if (id === "crossfader") {
                // crossfader from parameter store is 0.0 to 1.0, 
                // internally in UI we use -1.0 to 1.0
                crossfader.value = (value * 2.0) - 1.0
            }
        }
    }

    component MixerKnob: Item {
        id: knobRoot
        property alias text: label.text
        property alias from: dial.from
        property alias to: dial.to
        property alias knobValue: dial.value
        property real knobSize: 26
        property real labelSpace: 6
        property real defaultValue: (dial.from + dial.to) / 2
        property real columnWidth: 40
        // Kept for compatibility with existing uses; no longer affects layout.
        property string labelSide: "left"

        Layout.preferredWidth: columnWidth
        Layout.minimumWidth: columnWidth
        Layout.maximumWidth: columnWidth
        Layout.preferredHeight: knobSize + labelSpace
        Layout.minimumHeight: knobSize + labelSpace
        Layout.maximumHeight: knobSize + labelSpace
        Layout.alignment: Qt.AlignHCenter

        Dial {
            id: dial
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: knobRoot.knobSize
            height: knobRoot.knobSize

            background: Rectangle {
                x: dial.width / 2 - width / 2
                y: dial.height / 2 - height / 2
                width: dial.width
                height: dial.height
                radius: width / 2
                color: "transparent"
                border.color: "transparent"

                // Position arc: center-based for bipolar knobs, min-based for unipolar knobs.
                Canvas {
                    id: knobArc
                    anchors.fill: parent
                    antialiasing: true
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()

                        var cx = width / 2
                        var cy = height / 2
                        var radius = Math.min(width, height) * 0.44
                        var startDeg = 120
                        var spanDeg = 300

                        function clamp01(v) {
                            return Math.max(0, Math.min(1, v))
                        }

                        var from = dial.from
                        var to = dial.to
                        var norm = clamp01((dial.value - from) / (to - from))

                        ctx.lineWidth = Math.max(1, Math.round(width * 0.06))
                        ctx.lineCap = "round"
                        ctx.strokeStyle = "#5f6368"

                        if (from < 0 && to > 0) {
                            var neutral = clamp01((0 - from) / (to - from))
                            var a0 = (startDeg + neutral * spanDeg) * Math.PI / 180
                            var a1 = (startDeg + norm * spanDeg) * Math.PI / 180
                            ctx.beginPath()
                            ctx.arc(cx, cy, radius, a0, a1, norm < neutral)
                            ctx.stroke()
                        } else {
                            var start = startDeg * Math.PI / 180
                            var end = (startDeg + norm * spanDeg) * Math.PI / 180
                            ctx.beginPath()
                            ctx.arc(cx, cy, radius, start, end, false)
                            ctx.stroke()
                        }
                    }

                    Connections {
                        target: dial
                        function onValueChanged() { knobArc.requestPaint() }
                        function onFromChanged() { knobArc.requestPaint() }
                        function onToChanged() { knobArc.requestPaint() }
                    }
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.85
                    height: parent.height * 0.85
                    radius: width / 2
                    color: "#222"
                    border.color: "#444"
                    border.width: 0
                }
            }

            handle: Rectangle {
                id: handleItem
                x: dial.background.x + dial.background.width / 2 - width / 2
                y: dial.background.y + dial.background.height / 2 - height / 2
                width: dial.width * 0.85
                height: dial.height * 0.85
                color: "transparent"
                
                Rectangle {
                    color: "#aaa"
                    width: 2
                    height: parent.height * 0.48
                    radius: 1
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: -2
                }
                
                transform: [
                    Rotation {
                        angle: dial.angle
                        origin.x: handleItem.width / 2
                        origin.y: handleItem.height / 2
                    }
                ]
            }

            TapHandler {
                onDoubleTapped: {
                    dial.enabled = false
                    dial.value = knobRoot.defaultValue
                    dial.enabled = true
                }
            }
        }

        // Tiny label below the knob with minimal reserved space.
        Text {
            id: label
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: dial.bottom
            anchors.topMargin: 1
            color: "#666"
            font.pixelSize: window.spViewport(6)
            font.bold: true
            font.family: "monospace"
        }
    }

    component MixerSlider: Slider {
        id: control

        // For crossfader: draw from center line to handle.
        // For volume faders: draw from bottom to handle.
        property bool centerFill: false

        background: Rectangle {
            x: control.orientation === Qt.Horizontal ? control.leftPadding : control.width / 2 - 2
            y: control.orientation === Qt.Horizontal ? control.height / 2 - 2 : control.topPadding
            width: control.orientation === Qt.Horizontal ? control.availableWidth : 4
            height: control.orientation === Qt.Horizontal ? 4 : control.availableHeight
            radius: 0
            color: "#202020"
            border.color: "#3a3a3a"
            border.width: 0

            Rectangle {
                visible: control.orientation === Qt.Horizontal && !control.centerFill
                x: 1
                y: 1
                width: Math.max(0, control.visualPosition * (parent.width - 2))
                height: parent.height - 2
                radius: 0
                color: control.pressed ? "#5a5a5a" : "#3e3e3e"
            }

            Rectangle {
                visible: control.orientation === Qt.Horizontal && control.centerFill
                y: 1
                height: parent.height - 2
                radius: 0
                color: control.pressed ? "#5a5a5a" : "#3e3e3e"

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
                radius: 0
                color: control.pressed ? "#5a5a5a" : "#3e3e3e"
            }
        }

        handle: Rectangle {
            implicitWidth: control.orientation === Qt.Vertical ? 24 : 18
            implicitHeight: control.orientation === Qt.Vertical ? 12 : 22
            x: control.orientation === Qt.Horizontal
               ? control.leftPadding + control.visualPosition * (control.availableWidth - width)
               : control.width / 2 - width / 2
            y: control.orientation === Qt.Horizontal
               ? control.height / 2 - height / 2
               : control.topPadding + control.visualPosition * (control.availableHeight - height)
            radius: 0
            color: control.pressed ? "#e0e0e0" : "#c8c8c8"
            border.color: control.pressed ? "#707070" : "#444444"
            border.width: 0
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 8

        // Deck A & B Controls
        RowLayout {
            id: channelSegmentsRow
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
            spacing: 6

            // -------------------- CHANNEL A SEGMENT --------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignTop
                color: "#151515"
                border.color: "#353535"
                border.width: 0
                radius: 0

                Item {
                    id: segmentAContent
                    anchors.fill: parent
                    anchors.margins: 4

                    ColumnLayout {
                        id: channelAControls
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        spacing: 5

                        ColumnLayout {
                            id: eqStackA
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 5

                            MixerKnob {
                                text: "T"; labelSide: "left"; from: 0; to: 2; knobValue: 1.0
                                knobSize: 16
                                onKnobValueChanged: { if(engineA) engineA.trim = knobValue }
                            }
                            MixerKnob {
                                text: "H"; labelSide: "left"; from: -1; to: 1; knobValue: 0
                                onKnobValueChanged: { if(engineA) engineA.eqHigh = knobValue }
                            }
                            MixerKnob {
                                text: "M"; labelSide: "left"; from: -1; to: 1; knobValue: 0
                                onKnobValueChanged: { if(engineA) engineA.eqMid = knobValue }
                            }
                            MixerKnob {
                                text: "L"; labelSide: "left"; from: -1; to: 1; knobValue: 0
                                onKnobValueChanged: { if(engineA) engineA.eqLow = knobValue }
                            }
                            MixerKnob {
                                id: scKnobA
                                text: "SC"; labelSide: "left"; from: -1; to: 1; knobValue: 0.0
                                defaultValue: 0.0
                                onKnobValueChanged: {
                                    if(typeof fxManager !== "undefined") {
                                        fxManager.setSoundColorDeck(1, knobValue)
                                        if(fxManager.soundColorMode === "Filter") {
                                            if(engineA) engineA.filter = knobValue
                                        } else {
                                            if(engineA) engineA.filter = 0.0
                                        }
                                    } else {
                                        if(engineA) engineA.filter = knobValue
                                    }
                                }
                            }
                        }

                        Button {
                            text: "CUE"
                            checkable: true
                            checked: mixer.cueAActive
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 40
                            Layout.minimumWidth: 40
                            Layout.maximumWidth: 40
                            Layout.preferredHeight: 24
                            palette.buttonText: "#fff"
                            background: Rectangle {
                                color: parent.checked ? "#1f3a26" : "#333"
                                border.color: parent.checked ? "#66cc88" : "#555"
                                border.width: 0
                                radius: 0
                            }
                            onClicked: mixer.cueAActive = checked
                        }

                        MixerSlider {
                            id: volFaderA
                            Layout.fillHeight: false
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 40
                            Layout.minimumWidth: 40
                            Layout.maximumWidth: 40
                            Layout.preferredHeight: 48
                            Layout.maximumHeight: 48
                            orientation: Qt.Vertical
                            from: 0.0; to: 1.0; value: 1.0
                            onValueChanged: {
                                mixer.volA = value;
                                mixer.updateVolumes();
                                if (parameterStore && parameterStore.getParameter("deckA_vol") !== value) {
                                    parameterStore.setParameter("deckA_vol", value);
                                }
                            }
                            TapHandler {
                                onDoubleTapped: {
                                    volFaderA.enabled = false
                                    volFaderA.value = 1.0
                                    volFaderA.enabled = true
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: vuABack
                        width: 6
                        anchors.left: parent.left
                        anchors.leftMargin: 0
                        anchors.top: channelAControls.top
                        height: eqStackA.height
                        color: "#101010"
                        border.color: "#2f2f2f"
                        border.width: 0
                        radius: 0

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: Math.max(0, Math.min(parent.height, parent.height * mixer.vuACombined))
                            color: mixer.vuACombined > 0.85 ? "#ff5b4d" : (mixer.vuACombined > 0.6 ? "#ffcc55" : "#66cc88")
                            radius: 0
                        }
                    }
                }
            }

            // -------------------- CHANNEL B SEGMENT --------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignTop
                color: "#151515"
                border.color: "#353535"
                border.width: 0
                radius: 0

                Item {
                    id: segmentBContent
                    anchors.fill: parent
                    anchors.margins: 4

                    ColumnLayout {
                        id: channelBControls
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        spacing: 5

                        ColumnLayout {
                            id: eqStackB
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 5

                            MixerKnob {
                                text: "T"; labelSide: "right"; from: 0; to: 2; knobValue: 1.0
                                knobSize: 16
                                onKnobValueChanged: { if(engineB) engineB.trim = knobValue }
                            }
                            MixerKnob {
                                text: "H"; labelSide: "right"; from: -1; to: 1; knobValue: 0
                                onKnobValueChanged: { if(engineB) engineB.eqHigh = knobValue }
                            }
                            MixerKnob {
                                text: "M"; labelSide: "right"; from: -1; to: 1; knobValue: 0
                                onKnobValueChanged: { if(engineB) engineB.eqMid = knobValue }
                            }
                            MixerKnob {
                                text: "L"; labelSide: "right"; from: -1; to: 1; knobValue: 0
                                onKnobValueChanged: { if(engineB) engineB.eqLow = knobValue }
                            }
                            MixerKnob {
                                id: scKnobB
                                text: "SC"; labelSide: "right"; from: -1; to: 1; knobValue: 0.0
                                defaultValue: 0.0
                                onKnobValueChanged: {
                                    if(typeof fxManager !== "undefined") {
                                        fxManager.setSoundColorDeck(2, knobValue)
                                        if(fxManager.soundColorMode === "Filter") {
                                            if(engineB) engineB.filter = knobValue
                                        } else {
                                            if(engineB) engineB.filter = 0.0
                                        }
                                    } else {
                                        if(engineB) engineB.filter = knobValue
                                    }
                                }
                            }
                        }

                        Button {
                            text: "CUE"
                            checkable: true
                            checked: mixer.cueBActive
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 40
                            Layout.minimumWidth: 40
                            Layout.maximumWidth: 40
                            Layout.preferredHeight: 24
                            palette.buttonText: "#fff"
                            background: Rectangle {
                                color: parent.checked ? "#1f3a26" : "#333"
                                border.color: parent.checked ? "#66cc88" : "#555"
                                border.width: 0
                                radius: 0
                            }
                            onClicked: mixer.cueBActive = checked
                        }

                        MixerSlider {
                            id: volFaderB
                            Layout.fillHeight: false
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 40
                            Layout.minimumWidth: 40
                            Layout.maximumWidth: 40
                            Layout.preferredHeight: 48
                            Layout.maximumHeight: 48
                            orientation: Qt.Vertical
                            from: 0.0; to: 1.0; value: 1.0
                            onValueChanged: {
                                mixer.volB = value;
                                mixer.updateVolumes();
                                if (parameterStore && parameterStore.getParameter("deckB_vol") !== value) {
                                    parameterStore.setParameter("deckB_vol", value);
                                }
                            }
                            TapHandler {
                                onDoubleTapped: {
                                    volFaderB.enabled = false
                                    volFaderB.value = 1.0
                                    volFaderB.enabled = true
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: vuBBack
                        width: 6
                        anchors.left: parent.left
                        anchors.leftMargin: 0
                        anchors.top: channelBControls.top
                        height: eqStackB.height
                        color: "#101010"
                        border.color: "#2f2f2f"
                        border.width: 0
                        radius: 0

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: Math.max(0, Math.min(parent.height, parent.height * mixer.vuBCombined))
                            color: mixer.vuBCombined > 0.85 ? "#ff5b4d" : (mixer.vuBCombined > 0.6 ? "#ffcc55" : "#66cc88")
                            radius: 0
                        }
                    }
                }
            }
        }

        // Crossfader Section (fixed at the bottom, below channel segments)
        Item {
            Layout.fillWidth: true
            Layout.minimumHeight: 32
            Layout.preferredHeight: 32
            Layout.maximumHeight: 32

            MixerSlider {
                id: crossfader
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 24
                centerFill: true
                from: -1.0; to: 1.0; value: 0.0
                stepSize: 0.01
                onValueChanged: {
                    mixer.updateVolumes()
                    if (parameterStore) {
                        // convert -1.0 .. 1.0 to 0.0 .. 1.0 for the standard midi range store
                        var normValue = (value + 1.0) / 2.0;
                        if (Math.abs(parameterStore.getParameter("crossfader") - normValue) > 0.01) {
                            parameterStore.setParameter("crossfader", normValue);
                        }
                    }
                }
                TapHandler {
                    onDoubleTapped: {
                        crossfader.enabled = false
                        crossfader.value = 0.0
                        crossfader.enabled = true
                    }
                }
            }
        }
    }
}
