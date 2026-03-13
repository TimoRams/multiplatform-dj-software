import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// FxBar – horizontal FX strip that sits between the Deck/Mixer section and the
// library. Two FxUnits flank a simpler Sound Color selector in the center.
Rectangle {
    id: root

    color: "#121212"
    height: 40

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
            readonly property var modes: ["Space", "D.Echo", "Crush", "Pitch", "Noise", "Filter"]

            function isActiveMode(modeName) {
                if (typeof fxManager !== "undefined")
                    return fxManager.soundColorMode === modeName
                return fallbackMode === modeName
            }

            Connections {
                target: typeof fxManager !== "undefined" ? fxManager : null

                function onSoundColorModeChanged() {
                    soundColorPanel.fallbackMode = fxManager.soundColorMode
                }
            }

            Component.onCompleted: {
                if (typeof fxManager !== "undefined")
                    fallbackMode = fxManager.soundColorMode
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 3

                Text {
                    text: "SOUND COLOR FX"
                    color: "#686868"
                    font.pixelSize: 8
                    font.bold: true
                    font.family: "monospace"
                    Layout.alignment: Qt.AlignHCenter
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
