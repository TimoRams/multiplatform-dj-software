import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: mixer
    color: "#181818"
    
    property var engineA: null
    property var engineB: null
    
    // Internal volumes for the faders (so crossfader can modify the actual engine volume)
    property real volA: 0.8
    property real volB: 0.8
    
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

    component MixerKnob: RowLayout {
        id: knobRoot
        property alias text: label.text
        property alias from: dial.from
        property alias to: dial.to
        property alias knobValue: dial.value
        property real knobSize: 26
        property real defaultValue: (dial.from + dial.to) / 2
        // "left" = label on left (Deck A), "right" = label on right (Deck B)
        property string labelSide: "left"

        spacing: 4
        Layout.alignment: Qt.AlignHCenter

        // Left label slot
        Text {
            id: label
            visible: knobRoot.labelSide === "left"
            color: "#666"
            font.pixelSize: window.spViewport(9)
            font.bold: true
            font.family: "monospace"
            Layout.alignment: Qt.AlignVCenter
        }

        Dial {
            id: dial
            Layout.alignment: Qt.AlignVCenter
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
                
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.85
                    height: parent.height * 0.85
                    radius: width / 2
                    color: "#222"
                    border.color: "#444"
                    border.width: 1
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
                    height: parent.height * 0.35
                    radius: 1
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 1
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

        // Right label slot
        Text {
            visible: knobRoot.labelSide === "right"
            text: label.text
            color: "#666"
            font.pixelSize: window.spViewport(9)
            font.bold: true
            font.family: "monospace"
            Layout.alignment: Qt.AlignVCenter
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 15

        // Deck A & B Controls
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 30

            Item { Layout.fillWidth: true } // Left spacer

            // -------------------- DECK A --------------------
            ColumnLayout {
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignHCenter
                spacing: 8

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
                            // Filter mode drives the built-in engine filter directly
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

                Button {
                    text: "CUE"
                    checkable: true
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 30
                    palette.buttonText: checked ? "#000" : "#fff"
                    background: Rectangle { 
                        color: parent.checked ? "#ff9900" : "#333"
                        radius: 4 
                    }
                    onCheckedChanged: { if(engineA) engineA.cueEnabled = checked; }
                }

                // Volume Fader A
                Slider {
                    id: volFaderA
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignHCenter
                    orientation: Qt.Vertical
                    from: 0; to: 1.0; value: 0.8
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

            // -------------------- DECK B --------------------
            ColumnLayout {
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignHCenter
                spacing: 8

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

                Button {
                    text: "CUE"
                    checkable: true
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 30
                    palette.buttonText: checked ? "#000" : "#fff"
                    background: Rectangle { 
                        color: parent.checked ? "#00ccff" : "#333"
                        radius: 4 
                    }
                    onCheckedChanged: { if(engineB) engineB.cueEnabled = checked; }
                }

                // Volume Fader B
                Slider {
                    id: volFaderB
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignHCenter
                    orientation: Qt.Vertical
                    from: 0; to: 1.0; value: 0.8
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

            Item { Layout.fillWidth: true } // Right spacer
        }

        // Crossfader Section
        Text { 
            text: "A   CROSSFADER   B" 
            color: "#888"
            font.pixelSize: window.spViewport(9)
            font.bold: true
            Layout.alignment: Qt.AlignHCenter 
        }
        Slider {
            id: crossfader
            Layout.fillWidth: true
            Layout.preferredHeight: 30
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
