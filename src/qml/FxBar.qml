import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// FxBar – horizontal FX strip that sits between the Deck/Mixer section and the
// library. Two FxUnits flank a simpler Sound Color selector in the center.
Rectangle {
    id: root

    color: "#121212"
    
    // Dynamic FxBar height scaling based on window height
    // At reference height (800px), height is 40px
    // Scales proportionally with window height
    readonly property real _refHeight: 40
    readonly property real _refWindowHeight: 800
    height: Math.max(36, Math.round(_refHeight * (window.height / _refWindowHeight)))

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: "#222"
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        FxUnit {
            id: fxUnit1
            unitId: 1
            accentColor: "#1e90ff"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        Rectangle {
            width: 1
            Layout.fillHeight: true
            color: "#2a2a2a"
        }

        Rectangle {
            id: soundColorPanel
            color: "#151515"
            Layout.preferredWidth: 240
            Layout.fillHeight: true

            property string fallbackMode: "Filter"
            property real fallbackParam: 0.5
            readonly property var modes: ["Space", "D.Echo", "Crush", "Pitch", "Noise", "Sweep", "Filter"]

            function isActiveMode(modeName) {
                if (typeof fxManager !== "undefined" && fxManager !== null)
                    return fxManager.soundColorMode === modeName
                return fallbackMode === modeName
            }

            Connections {
                target: (typeof fxManager !== "undefined" && fxManager !== null) ? fxManager : null

                function onSoundColorModeChanged() {
                    soundColorPanel.fallbackMode = fxManager.soundColorMode
                }

                function onSoundColorParamChanged() {
                    soundColorPanel.fallbackParam = fxManager.soundColorParam
                }
            }

            Component.onCompleted: {
                if (typeof fxManager !== "undefined" && fxManager !== null) {
                    fallbackMode = fxManager.soundColorMode
                    fallbackParam = fxManager.soundColorParam
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 3

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Column {
                        Layout.preferredWidth: 28
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 1

                        Dial {
                            id: scParamKnob
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 22
                            height: 22
                            from: 0.0
                            to: 1.0
                            stepSize: 0.01
                            value: soundColorPanel.fallbackParam

                            background: Rectangle {
                                x: scParamKnob.width / 2 - width / 2
                                y: scParamKnob.height / 2 - height / 2
                                width: scParamKnob.width
                                height: scParamKnob.height
                                radius: width / 2
                                color: "transparent"
                                border.color: "transparent"

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width * 0.86
                                    height: parent.height * 0.86
                                    radius: width / 2
                                    color: "#1f1f1f"
                                    border.color: "#5a5a5a"
                                    border.width: 1
                                }
                            }

                            handle: Rectangle {
                                id: scParamHandle
                                x: scParamKnob.background.x + scParamKnob.background.width / 2 - width / 2
                                y: scParamKnob.background.y + scParamKnob.background.height / 2 - height / 2
                                width: scParamKnob.width * 0.86
                                height: scParamKnob.height * 0.86
                                color: "transparent"

                                Rectangle {
                                    color: "#d0d0d0"
                                    width: 2
                                    height: parent.height * 0.35
                                    radius: 1
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.top: parent.top
                                    anchors.topMargin: 1
                                }

                                transform: Rotation {
                                    angle: scParamKnob.angle
                                    origin.x: scParamHandle.width / 2
                                    origin.y: scParamHandle.height / 2
                                }
                            }

                            onValueChanged: {
                                soundColorPanel.fallbackParam = value
                                if (typeof fxManager !== "undefined" && fxManager !== null)
                                    fxManager.setSoundColorParam(value)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Repeater {
                            model: soundColorPanel.modes
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                height: 18
                                radius: 3
                                color: soundColorPanel.isActiveMode(modelData)
                                       ? "#242424"
                                       : "#191919"
                                border.color: soundColorPanel.isActiveMode(modelData)
                                              ? "#666"
                                              : "#303030"
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData
                                    font.pixelSize: 8
                                    font.bold: true
                                    font.family: "monospace"
                                    color: soundColorPanel.isActiveMode(modelData)
                                           ? "#f0f0f0"
                                           : "#767676"
                                    elide: Text.ElideRight
                                }

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.leftMargin: 4
                                    anchors.rightMargin: 4
                                    height: 2
                                    radius: 1
                                    visible: soundColorPanel.isActiveMode(modelData)
                                    color: "#d6d6d6"
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor

                                    onClicked: {
                                        soundColorPanel.fallbackMode = modelData
                                        if (typeof fxManager !== "undefined")
                                            fxManager.setSoundColorMode(modelData)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            width: 1
            Layout.fillHeight: true
            color: "#2a2a2a"
        }

        FxUnit {
            id: fxUnit2
            unitId: 2
            accentColor: "#ff6a00"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
