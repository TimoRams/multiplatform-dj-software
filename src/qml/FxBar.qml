import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// ─────────────────────────────────────────────────────────────────────────────
// FxBar – horizontal FX strip that sits between the Deck/Mixer section and the
//         Library.  Two FxUnits share the full width 50 / 50 with a thin
//         divider in the exact centre.
// ─────────────────────────────────────────────────────────────────────────────
Rectangle {
    id: root

    color: "#141414"
    height: 40

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── FX Unit 1 (Deck A side) ──────────────────────────────────────────
        FxUnit {
            id: fxUnit1
            unitId: 1
            accentColor: "#1e90ff"   // blue – matches deck A
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        // ── Centre divider (left edge of SoundColor panel) ────────────────
        Rectangle {
            width:  1
            Layout.fillHeight: true
            color: "#2e2e2e"
        }

        // ── SoundColor Panel ─────────────────────────────────────────────────
        Rectangle {
            id: soundColorPanel
            color: "#111111"
            Layout.preferredWidth:  180
            Layout.fillHeight: true

            property string activeMode: "Filter"

            readonly property var modes: ["Space", "D.Echo", "Crush", "Pitch", "Noise", "Filter"]

            // Mode color accent per SoundColor mode
            function modeColor(m) {
                switch(m) {
                    case "Space":  return "#8844ff"
                    case "D.Echo": return "#00ccff"
                    case "Crush":  return "#ff4444"
                    case "Pitch":  return "#ffcc00"
                    case "Noise":  return "#ff8800"
                    case "Filter": return "#44dd88"
                    default:       return "#888888"
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 3
                anchors.bottomMargin: 3
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                spacing: 2

                // ── Mode button row ──────────────────────────────────────────
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Repeater {
                        model: soundColorPanel.modes
                        delegate: Rectangle {
                            Layout.fillWidth: true
                            height: 13
                            radius: 2
                            color: soundColorPanel.activeMode === modelData
                                   ? soundColorPanel.modeColor(modelData)
                                   : "#222"
                            border.color: soundColorPanel.activeMode === modelData
                                          ? soundColorPanel.modeColor(modelData)
                                          : "#333"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: 7
                                font.bold: true
                                font.family: "monospace"
                                color: soundColorPanel.activeMode === modelData
                                       ? "#000" : "#666"
                                elide: Text.ElideRight
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    soundColorPanel.activeMode = modelData
                                    if (typeof fxManager !== "undefined")
                                        fxManager.setSoundColorMode(modelData)
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Centre divider (right edge of SoundColor panel) ──────────────────
        Rectangle {
            width:  1
            Layout.fillHeight: true
            color: "#2e2e2e"
        }

        // ── FX Unit 2 (Deck B side) ──────────────────────────────────────────
        FxUnit {
            id: fxUnit2
            unitId: 2
            accentColor: "#ff6a00"   // orange – matches deck B
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
