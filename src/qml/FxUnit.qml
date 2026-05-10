import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: root

    property int   unitId:      1
    property bool  deck1Active: btnDeck1.active
    property bool  deck2Active: btnDeck2.active
    property alias effectType:  effectCombo.currentText
    property alias wetDry:      mixKnob.value
    property color accentColor: unitId === 1 ? "#1e90ff" : "#ff6a00"

    signal deck1Toggled(bool active)
    signal deck2Toggled(bool active)

    color: "#181818"

    // ── Compact toggle button ─────────────────────────────────────────────
    component AssignBtn: Rectangle {
        id: ab
        required property string label
        property bool  active: false
        property color accent: root.accentColor

        implicitWidth:  26
        implicitHeight: 22
        radius: 0
        color:  active ? "#222222" : "#1e1e1e"

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left:   parent.left
            anchors.right:  parent.right
            height: 1
            color:  ab.active ? ab.accent : "#333333"
        }

        Text {
            anchors.centerIn: parent
            text:           ab.label
            color:          ab.active ? ab.accent : "#666666"
            font.pixelSize: 10
            font.bold:      ab.active
            font.family:    "monospace"
        }

        HoverHandler { id: abHov }
        Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: abHov.hovered ? 0.03 : 0 }

        MouseArea {
            anchors.fill: parent
            cursorShape:  Qt.PointingHandCursor
            onClicked:    ab.active = !ab.active
        }

        onActiveChanged: ab.parent  // trigger parent bindings
    }

    RowLayout {
        anchors.fill:        parent
        anchors.leftMargin:  8
        anchors.rightMargin: 8
        spacing: 6

        Text {
            text:             "FX" + root.unitId
            color:            "#666666"
            font.pixelSize:   9
            font.bold:        true
            font.family:      "monospace"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 24
        }

        AssignBtn {
            id: btnDeck1
            label:  "1"
            accent: root.accentColor
            Layout.alignment: Qt.AlignVCenter
            onActiveChanged: {
                root.deck1Toggled(active)
                if (typeof fxManager !== "undefined")
                    fxManager.setDeckAssignment(root.unitId, 1, active)
            }
        }

        AssignBtn {
            id: btnDeck2
            label:  "2"
            accent: root.accentColor
            Layout.alignment: Qt.AlignVCenter
            onActiveChanged: {
                root.deck2Toggled(active)
                if (typeof fxManager !== "undefined")
                    fxManager.setDeckAssignment(root.unitId, 2, active)
            }
        }

        ComboBox {
            id: effectCombo
            model: [
                "---",
                "Echo", "Low Cut Echo", "MT Delay",
                "Reverb", "Spiral", "Flanger", "Phaser",
                "Trans", "Enigma Jet", "Bitcrusher", "Pitch Shifter",
                "Stretch", "Slip Roll", "Roll", "Nobius", "Mobius"
            ]
            Layout.fillWidth:       true
            Layout.preferredHeight: 24
            Layout.alignment:       Qt.AlignVCenter

            contentItem: Text {
                leftPadding:       6
                rightPadding:      18
                text:              effectCombo.displayText
                color:             "#e8e8e8"
                font.pixelSize:    10
                font.family:       "monospace"
                verticalAlignment: Text.AlignVCenter
                elide:             Text.ElideRight
            }

            indicator: Canvas {
                x: effectCombo.width - width - 7
                y: effectCombo.topPadding + (effectCombo.availableHeight - height) / 2
                width:  8; height: 6
                contextType: "2d"
                onPaint: {
                    context.reset()
                    context.moveTo(0, 0); context.lineTo(width, 0)
                    context.lineTo(width / 2, height); context.closePath()
                    context.fillStyle = "#777777"; context.fill()
                }
            }

            background: Rectangle {
                color:  effectCombo.pressed ? "#2d2d2d" : "#1e1e1e"
                radius: 0

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    height: 1
                    color:  effectCombo.visualFocus ? root.accentColor : "#333333"
                }
            }

            delegate: ItemDelegate {
                width: effectCombo.width; height: 24
                highlighted: effectCombo.highlightedIndex === index

                contentItem: Text {
                    text:              modelData
                    color:             highlighted ? "#e8e8e8" : "#999999"
                    font.pixelSize:    10
                    font.family:       "monospace"
                    leftPadding:       8
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    color:  highlighted ? "#252525" : "#181818"
                    radius: 0
                    Rectangle {
                        anchors.top:   parent.top
                        anchors.left:  parent.left
                        anchors.right: parent.right
                        height: 1; color: highlighted ? root.accentColor : "#1c1c1c"
                    }
                }
            }

            popup.background: Rectangle {
                color: "#181818"; radius: 0
                Rectangle {
                    anchors.top:   parent.top
                    anchors.left:  parent.left
                    anchors.right: parent.right
                    height: 1; color: "#333333"
                }
            }

            onCurrentTextChanged: {
                if (typeof fxManager !== "undefined")
                    fxManager.setEffectType(root.unitId, currentText)
            }
        }

        // MIX knob
        Column {
            Layout.preferredWidth:  38
            Layout.preferredHeight: 38
            Layout.alignment:       Qt.AlignVCenter
            spacing: 2

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text:           "MIX"
                color:          "#555555"
                font.pixelSize: 8
                font.family:    "monospace"
            }

            Knob {
                id: mixKnob
                anchors.horizontalCenter: parent.horizontalCenter
                width:        26
                height:       26
                from:         0.0
                to:           1.0
                value:        0.0
                stepSize:     0.01
                accentColor:  root.accentColor
                defaultValue: 0.0

                onValueChanged: {
                    if (typeof fxManager !== "undefined")
                        fxManager.setWetDry(root.unitId, value)
                }
            }
        }
    }
}
