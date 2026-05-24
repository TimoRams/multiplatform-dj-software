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
            Layout.preferredWidth: 248
            Layout.fillHeight:     true

            property string fallbackMode:  "Filter"
            property real   fallbackParam: 0.5
            readonly property var modes: ["Space", "D.Echo", "Crush", "Pitch", "Noise", "Sweep", "Filter"]

            // Per-mode label for the param knob — shown below the knob
            readonly property var paramLabels: ({
                "Space":  "SIZE",
                "D.Echo": "TIME",
                "Crush":  "DEPTH",
                "Pitch":  "RANGE",
                "Noise":  "LEVEL",
                "Sweep":  "RESO",
                "Filter": "RESO"
            })

            readonly property string activeMode: {
                if (typeof fxManager !== "undefined" && fxManager !== null)
                    return fxManager.soundColorMode
                return fallbackMode
            }

            function isActiveMode(name) { return scPanel.activeMode === name }

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
                anchors.topMargin:    5
                anchors.bottomMargin: 5
                anchors.leftMargin:   6
                anchors.rightMargin:  6
                spacing: 4

                // Row 1: Section label
                Text {
                    Layout.fillWidth: true
                    text:           "COLOR FX"
                    color:          "#444444"
                    font.pixelSize: 7
                    font.family:    "monospace"
                    font.letterSpacing: 1
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    // Param knob with mode-specific label
                    Column {
                        Layout.preferredWidth: 32
                        Layout.alignment:      Qt.AlignVCenter
                        spacing: 2

                        Knob {
                            id: scKnob
                            anchors.horizontalCenter: parent.horizontalCenter
                            width:        24
                            height:       24
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

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text:           scPanel.paramLabels[scPanel.activeMode] ?? "PARAM"
                            color:          "#555555"
                            font.pixelSize: 7
                            font.family:    "monospace"
                        }
                    }

                    // Mode buttons — 4 columns, 2 rows
                    Grid {
                        Layout.fillWidth: true
                        columns: 4
                        rowSpacing: 2
                        columnSpacing: 2

                        Repeater {
                            model: scPanel.modes
                            delegate: Rectangle {
                                readonly property bool isActive: scPanel.isActiveMode(modelData)

                                width:  Math.floor((scPanel.width - 44 - 5 * 3) / 4)
                                height: 20
                                radius: 1
                                color:  isActive ? "#2a2a2a" : "#1c1c1c"

                                // Active accent bar at top
                                Rectangle {
                                    anchors.top:   parent.top
                                    anchors.left:  parent.left
                                    anchors.right: parent.right
                                    height: isActive ? 2 : 1
                                    color:  isActive ? "#aaaaaa" : "#2e2e2e"
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
                                Rectangle { anchors.fill: parent; radius: parent.radius; color: "#ffffff"; opacity: modeHov.hovered ? 0.04 : 0 }

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
