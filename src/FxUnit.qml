import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// ─────────────────────────────────────────────────────────────────────────────
// FxUnit – a single effects unit (deck assignment + effect type + wet/dry knob)
// ─────────────────────────────────────────────────────────────────────────────
Rectangle {
    id: root

    // ── Public API ────────────────────────────────────────────────────────────
    // Which FX slot this unit represents (1 = left, 2 = right)
    property int unitId: 1

    // Deck-assignment state (readable from outside)
    property alias deck1Active: btnDeck1.checked
    property alias deck2Active: btnDeck2.checked

    // Currently selected effect name
    property alias effectType: effectCombo.currentText

    // Wet/Dry amount  0.0 … 1.0
    property alias wetDry: wetDryDial.value

    // Accent colour that identifies this unit visually (overridable)
    property color accentColor: unitId === 1 ? "#1e90ff" : "#ff6a00"

    // ── Signals ───────────────────────────────────────────────────────────────
    // NOTE: aliases already auto-generate effectTypeChanged, wetDryChanged,
    // deck1ActiveChanged, deck2ActiveChanged — do NOT redeclare them.
    signal deck1Toggled(bool active)
    signal deck2Toggled(bool active)

    // ── Geometry ──────────────────────────────────────────────────────────────
    height: 40
    color: "#1a1a1a"

    // Thin accent stripe at the top
    Rectangle {
        anchors.top:  parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 2
        color: root.accentColor
        opacity: 0.7
    }

    // ── Layout ────────────────────────────────────────────────────────────────
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin:  8
        anchors.rightMargin: 8
        spacing: 6

        // ── FX label ─────────────────────────────────────────────────────────
        Text {
            text: "FX" + root.unitId
            color: root.accentColor
            font.pixelSize: 10
            font.bold: true
            font.family: "monospace"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 22
        }

        // ── Deck-assignment buttons ───────────────────────────────────────────
        Button {
            id: btnDeck1
            text: "1"
            checkable: true
            checked: false
            Layout.preferredWidth:  26
            Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignVCenter

            contentItem: Text {
                text: parent.text
                color: parent.checked ? "#000" : "#aaa"
                font.pixelSize: 10
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment:   Text.AlignVCenter
            }

            background: Rectangle {
                color: parent.checked ? root.accentColor : "#2e2e2e"
                radius: 3
                border.color: parent.checked ? root.accentColor : "#444"
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
            Layout.preferredWidth:  26
            Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignVCenter

            contentItem: Text {
                text: parent.text
                color: parent.checked ? "#000" : "#aaa"
                font.pixelSize: 10
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment:   Text.AlignVCenter
            }

            background: Rectangle {
                color: parent.checked ? root.accentColor : "#2e2e2e"
                radius: 3
                border.color: parent.checked ? root.accentColor : "#444"
                border.width: 1
            }

            onCheckedChanged: {
                root.deck2Toggled(checked)
                if (typeof fxManager !== "undefined")
                    fxManager.setDeckAssignment(root.unitId, 2, checked)
            }
        }

        // ── Effect selector ──────────────────────────────────────────────────
        ComboBox {
            id: effectCombo
            model: ["---", "Echo", "Reverb", "Flanger", "Filter", "Phaser", "Bitcrusher", "Delay"]
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            Layout.alignment: Qt.AlignVCenter

            contentItem: Text {
                leftPadding: 6
                text: effectCombo.displayText
                color: "#ddd"
                font.pixelSize: 10
                font.family: "monospace"
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            background: Rectangle {
                color: effectCombo.pressed ? "#333" : "#252525"
                radius: 3
                border.color: effectCombo.pressed ? root.accentColor : "#444"
                border.width: 1
            }

            // Use default delegate — custom ones broke mouse input
            delegate: ItemDelegate {
                width: effectCombo.width
                height: 24
                highlighted: effectCombo.highlightedIndex === index

                contentItem: Text {
                    text: modelData
                    color: highlighted ? root.accentColor : "#ccc"
                    font.pixelSize: 10
                    font.family: "monospace"
                    leftPadding: 8
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    color: highlighted ? "#2a2a2a" : "#1a1a1a"
                }
            }

            onCurrentTextChanged: {
                if (typeof fxManager !== "undefined")
                    fxManager.setEffectType(root.unitId, currentText)
            }
        }

        // ── Wet/Dry knob ─────────────────────────────────────────────────────
        Item {
            Layout.preferredWidth:  36
            Layout.preferredHeight: 32
            Layout.alignment: Qt.AlignVCenter

            // Label above the knob
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                text: "W/D"
                color: "#666"
                font.pixelSize: 8
                font.family: "monospace"
            }

            Dial {
                id: wetDryDial
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width:  26
                height: 26
                from:   0.0
                to:     1.0
                value:  0.0
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
                        color: "#222"
                        border.color: wetDryDial.value > 0 ? root.accentColor : "#444"
                        border.width: 1
                    }
                }

                handle: Rectangle {
                    id: dialHandle
                    x: wetDryDial.background.x + wetDryDial.background.width  / 2 - width  / 2
                    y: wetDryDial.background.y + wetDryDial.background.height / 2 - height / 2
                    width:  wetDryDial.width  * 0.85
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
                        origin.x: dialHandle.width  / 2
                        origin.y: dialHandle.height / 2
                    }
                }

                // Double-click resets to 0
                TapHandler {
                    onDoubleTapped: {
                        wetDryDial.enabled = false
                        wetDryDial.value   = 0.0
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
