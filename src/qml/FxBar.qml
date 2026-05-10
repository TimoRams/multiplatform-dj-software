import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: root

    color: "#181818"
    height: window.fxBarHeight

    Rectangle {
        anchors.top:   parent.top
        anchors.left:  parent.left
        anchors.right: parent.right
        height: 1
        color:  "#0a0a0a"
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        FxUnit {
            id: fxUnit1
            unitId:      1
            accentColor: "#1e90ff"
            Layout.fillWidth:  true
            Layout.fillHeight: true
        }

        Rectangle { width: 1; Layout.fillHeight: true; color: "#0a0a0a" }

        // ── Sound Color panel ─────────────────────────────────────────────
        Rectangle {
            id: scPanel
            color: "#181818"
            Layout.preferredWidth: 240
            Layout.fillHeight:     true

            property string fallbackMode:  "Filter"
            property real   fallbackParam: 0.5
            readonly property var modes: ["Space", "D.Echo", "Crush", "Pitch", "Noise", "Sweep", "Filter"]

            function isActiveMode(name) {
                if (typeof fxManager !== "undefined" && fxManager !== null)
                    return fxManager.soundColorMode === name
                return fallbackMode === name
            }

            Connections {
                target: (typeof fxManager !== "undefined" && fxManager !== null) ? fxManager : null
                function onSoundColorModeChanged()  { scPanel.fallbackMode  = fxManager.soundColorMode }
                function onSoundColorParamChanged() { scPanel.fallbackParam = fxManager.soundColorParam }
            }

            Component.onCompleted: {
                if (typeof fxManager !== "undefined" && fxManager !== null) {
                    fallbackMode  = fxManager.soundColorMode
                    fallbackParam = fxManager.soundColorParam
                }
            }

            ColumnLayout {
                anchors.fill:         parent
                anchors.topMargin:    4
                anchors.bottomMargin: 4
                anchors.leftMargin:   6
                anchors.rightMargin:  6
                spacing: 3

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    // Param knob
                    Column {
                        Layout.preferredWidth: 30
                        Layout.alignment:      Qt.AlignVCenter
                        spacing: 2

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text:           "SC"
                            color:          "#555555"
                            font.pixelSize: 8
                            font.family:    "monospace"
                        }

                        Knob {
                            id: scKnob
                            anchors.horizontalCenter: parent.horizontalCenter
                            width:        22
                            height:       22
                            from:         0.0
                            to:           1.0
                            stepSize:     0.01
                            value:        scPanel.fallbackParam
                            accentColor:  "#999999"
                            defaultValue: 0.5

                            onValueChanged: {
                                scPanel.fallbackParam = value
                                if (typeof fxManager !== "undefined" && fxManager !== null)
                                    fxManager.setSoundColorParam(value)
                            }
                        }
                    }

                    // Mode buttons
                    Grid {
                        Layout.fillWidth: true
                        columns: 4
                        rowSpacing: 2
                        columnSpacing: 2

                        Repeater {
                            model: scPanel.modes
                            delegate: Rectangle {
                                readonly property bool isActive: scPanel.isActiveMode(modelData)

                                width:  (scPanel.width - 36 - 6 * 3 - 12) / 4
                                height: 18
                                radius: 0
                                color:  isActive ? "#252525" : "#1e1e1e"

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left:   parent.left
                                    anchors.right:  parent.right
                                    height: 1
                                    color:  isActive ? "#999999" : "#333333"
                                }

                                Text {
                                    anchors.centerIn: parent
                                    text:           modelData
                                    font.pixelSize: 8
                                    font.bold:      isActive
                                    font.family:    "monospace"
                                    color:          isActive ? "#e8e8e8" : "#666666"
                                    elide:          Text.ElideRight
                                }

                                HoverHandler { id: modeHov }
                                Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: modeHov.hovered ? 0.03 : 0 }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked: {
                                        scPanel.fallbackMode = modelData
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

        Rectangle { width: 1; Layout.fillHeight: true; color: "#0a0a0a" }

        FxUnit {
            id: fxUnit2
            unitId:      2
            accentColor: "#ff6a00"
            Layout.fillWidth:  true
            Layout.fillHeight: true
        }
    }
}
