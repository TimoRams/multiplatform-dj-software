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

    component MixerKnob: ColumnLayout {
        property alias text: label.text
        property alias from: dial.from
        property alias to: dial.to
        property alias knobValue: dial.value

        spacing: 2
        Layout.alignment: Qt.AlignHCenter

        Text { 
            id: label
            color: "#aaa"
            font.pixelSize: 9
            font.bold: true
            Layout.alignment: Qt.AlignHCenter 
        }

        Dial { 
            id: dial
            Layout.alignment: Qt.AlignHCenter
            width: 32
            height: 32

            background: Rectangle {
                x: dial.width / 2 - width / 2
                y: dial.height / 2 - height / 2
                width: 32
                height: 32
                radius: 16
                color: "transparent"
                border.color: "transparent"
                
                // Actual visual background
                Rectangle {
                    anchors.centerIn: parent
                    width: 28; height: 28
                    radius: 14
                    color: "#222"
                    border.color: "#444"
                    border.width: 1
                }
            }

            handle: Rectangle {
                id: handleItem
                x: dial.background.x + dial.background.width / 2 - width / 2
                y: dial.background.y + dial.background.height / 2 - height / 2
                width: 28
                height: 28
                color: "transparent"
                
                // The indicator line
                Rectangle {
                    color: "#aaa"
                    width: 2
                    height: 9
                    radius: 1
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 2
                }
                
                transform: [
                    Translate {
                        y: -14 + handleItem.height / 2
                    },
                    Rotation {
                        angle: dial.angle
                        origin.x: handleItem.width / 2
                        origin.y: handleItem.height / 2
                    }
                ]
            }
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
                    text: "TRIM"; from: 0; to: 2; knobValue: 1.0; 
                    onKnobValueChanged: (val) => { if(engineA) engineA.trim = val; }
                }

                MixerKnob { 
                    text: "HIGH"; from: -1; to: 1; knobValue: 0; 
                    onKnobValueChanged: (val) => { if(engineA) engineA.eqHigh = val; }
                }

                MixerKnob { 
                    text: "MID"; from: -1; to: 1; knobValue: 0; 
                    onKnobValueChanged: (val) => { if(engineA) engineA.eqMid = val; }
                }

                MixerKnob { 
                    text: "LOW"; from: -1; to: 1; knobValue: 0; 
                    onKnobValueChanged: (val) => { if(engineA) engineA.eqLow = val; }
                }

                MixerKnob { 
                    text: "FILTER"; from: -1; to: 1; knobValue: 0; 
                    onKnobValueChanged: (val) => { if(engineA) engineA.filter = val; }
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
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignHCenter
                    orientation: Qt.Vertical
                    from: 0; to: 1.0; value: 0.8
                    onValueChanged: { mixer.volA = value; mixer.updateVolumes(); }
                }
            }

            // -------------------- DECK B --------------------
            ColumnLayout {
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignHCenter
                spacing: 8

                MixerKnob { 
                    text: "TRIM"; from: 0; to: 2; knobValue: 1.0; 
                    onKnobValueChanged: { if(engineB) engineB.trim = knobValue; }
                }

                MixerKnob { 
                    text: "HIGH"; from: -1; to: 1; knobValue: 0; 
                    onKnobValueChanged: { if(engineB) engineB.eqHigh = knobValue; }
                }

                MixerKnob { 
                    text: "MID"; from: -1; to: 1; knobValue: 0; 
                    onKnobValueChanged: { if(engineB) engineB.eqMid = knobValue; }
                }

                MixerKnob { 
                    text: "LOW"; from: -1; to: 1; knobValue: 0; 
                    onKnobValueChanged: { if(engineB) engineB.eqLow = knobValue; }
                }

                MixerKnob { 
                    text: "FILTER"; from: -1; to: 1; knobValue: 0; 
                    onKnobValueChanged: { if(engineB) engineB.filter = knobValue; }
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
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignHCenter
                    orientation: Qt.Vertical
                    from: 0; to: 1.0; value: 0.8
                    onValueChanged: { mixer.volB = value; mixer.updateVolumes(); }
                }
            }

            Item { Layout.fillWidth: true } // Right spacer
        }

        // Crossfader Section
        Text { 
            text: "A   CROSSFADER   B" 
            color: "#888"
            font.pixelSize: 9
            font.bold: true
            Layout.alignment: Qt.AlignHCenter 
        }
        Slider {
            id: crossfader
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            from: -1.0; to: 1.0; value: 0.0
            stepSize: 0.01
            onValueChanged: mixer.updateVolumes()
        }
    }
}
