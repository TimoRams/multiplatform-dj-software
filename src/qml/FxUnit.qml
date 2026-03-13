import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// FxUnit – a single effects unit (deck assignment + effect type + wet/dry knob)
Rectangle {
    id: root

    property int unitId: 1
    property alias deck1Active: btnDeck1.checked
    property alias deck2Active: btnDeck2.checked
    property alias effectType: effectCombo.currentText
    property alias wetDry: wetDryDial.value
    property color accentColor: unitId === 1 ? "#1e90ff" : "#ff6a00"

    signal deck1Toggled(bool active)
    signal deck2Toggled(bool active)

    height: 40
    color: "#161616"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 6

        Text {
            text: "FX" + root.unitId
            color: "#8d8d8d"
            font.pixelSize: 9
            font.bold: true
            font.family: "monospace"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 24
        }

        Button {
            id: btnDeck1
            text: "1"
            checkable: true
            checked: false
            Layout.preferredWidth: 26
            Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignVCenter

            contentItem: Text {
                text: parent.text
                color: parent.checked ? root.accentColor : "#8f8f8f"
                font.pixelSize: 10
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                color: parent.checked ? "#222222" : "#1f1f1f"
                radius: 3
                border.color: parent.checked ? root.accentColor : "#353535"
                border.width: 1
            }

            onCheckedChanged: {
                root.deck1Toggled(checked)
                if (typeof fxManager !== "undefined")
                    fxManager.setDeckAssignment(root.unitId, 1, checked)
            }
        }

        Button {
            id: btnDeck2
            text: "2"
            checkable: true
            checked: false
            Layout.preferredWidth: 26
            Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignVCenter

            contentItem: Text {
                text: parent.text
                color: parent.checked ? root.accentColor : "#8f8f8f"
                font.pixelSize: 10
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                color: parent.checked ? "#222222" : "#1f1f1f"
                radius: 3
                border.color: parent.checked ? root.accentColor : "#353535"
                border.width: 1
            }

            onCheckedChanged: {
                root.deck2Toggled(checked)
                if (typeof fxManager !== "undefined")
                    fxManager.setDeckAssignment(root.unitId, 2, checked)
            }
        }

        ComboBox {
            id: effectCombo
            model: [
                "---",
                "Echo",
                "Low Cut Echo",
                "MT Delay",
                "Reverb",
                "Spiral",
                "Flanger",
                "Phaser",
                "Trans",
                "Enigma Jet",
                "Bitcrusher",
                "Pitch Shifter",
                "Stretch",
                "Slip Roll",
                "Roll",
                "Nobius",
                "Mobius"
            ]
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            Layout.alignment: Qt.AlignVCenter

            contentItem: Text {
                leftPadding: 6
                rightPadding: 18
                text: effectCombo.displayText
                color: "#d5d5d5"
                font.pixelSize: 10
                font.family: "monospace"
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            indicator: Canvas {
                x: effectCombo.width - width - 7
                y: effectCombo.topPadding + (effectCombo.availableHeight - height) / 2
                width: 8
                height: 6
                contextType: "2d"

                onPaint: {
                    context.reset()
                    context.moveTo(0, 0)
                    context.lineTo(width, 0)
                    context.lineTo(width / 2, height)
                    context.closePath()
                    context.fillStyle = "#777"
                    context.fill()
                }
            }

            background: Rectangle {
                color: effectCombo.pressed ? "#242424" : "#1f1f1f"
                radius: 3
                border.color: effectCombo.visualFocus ? root.accentColor : "#353535"
                border.width: 1
            }

            delegate: ItemDelegate {
                width: effectCombo.width
                height: 24
                highlighted: effectCombo.highlightedIndex === index

                contentItem: Text {
                    text: modelData
                    color: highlighted ? "#f1f1f1" : "#bcbcbc"
                    font.pixelSize: 10
                    font.family: "monospace"
                    leftPadding: 8
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    color: highlighted ? "#262626" : "#171717"
                    border.color: highlighted ? root.accentColor : "transparent"
                    border.width: highlighted ? 1 : 0
                }
            }

            popup.background: Rectangle {
                color: "#171717"
                border.color: "#303030"
                border.width: 1
            }

            onCurrentTextChanged: {
                if (typeof fxManager !== "undefined")
                    fxManager.setEffectType(root.unitId, currentText)
            }
        }

        Item {
            Layout.preferredWidth: 36
            Layout.preferredHeight: 32
            Layout.alignment: Qt.AlignVCenter

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                text: "MIX"
                color: "#666"
                font.pixelSize: 8
                font.family: "monospace"
            }

            Dial {
                id: wetDryDial
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: 26
                height: 26
                from: 0.0
                to: 1.0
                value: 0.0
                stepSize: 0.01

                background: Rectangle {
                    x: wetDryDial.width / 2 - width / 2
                    y: wetDryDial.height / 2 - height / 2
                    width: wetDryDial.width
                    height: wetDryDial.height
                    radius: width / 2
                    color: "transparent"
                    border.color: "transparent"

                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width * 0.85
                        height: parent.height * 0.85
                        radius: width / 2
                        color: "#1f1f1f"
                        border.color: wetDryDial.value > 0 ? root.accentColor : "#444"
                        border.width: 1
                    }
                }

                handle: Rectangle {
                    id: dialHandle
                    x: wetDryDial.background.x + wetDryDial.background.width / 2 - width / 2
                    y: wetDryDial.background.y + wetDryDial.background.height / 2 - height / 2
                    width: wetDryDial.width * 0.85
                    height: wetDryDial.height * 0.85
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

                    transform: Rotation {
                        angle: wetDryDial.angle
                        origin.x: dialHandle.width / 2
                        origin.y: dialHandle.height / 2
                    }
                }

                TapHandler {
                    onDoubleTapped: {
                        wetDryDial.enabled = false
                        wetDryDial.value = 0.0
                        wetDryDial.enabled = true
                    }
                }

                onValueChanged: {
                    if (typeof fxManager !== "undefined")
                        fxManager.setWetDry(root.unitId, value)
                }
            }
        }
    }
}
