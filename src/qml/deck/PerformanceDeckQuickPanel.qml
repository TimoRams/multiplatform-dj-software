import QtQuick
import QtQuick.Layouts

// Deck info column in the layout used by professional DJ players: a titled deck
// header, then a stack of flat rows where a small caption on the left names the
// value that is set in large type on the right. No boxes inside boxes — the
// rows are separated by hairlines only, which is what makes the strip read as
// one instrument rather than a pile of widgets.
Rectangle {
    id: root

    property var engine: null
    property string deckName: "A"
    property bool selected: false
    property real beatJumpBeats: 4
    signal selectedRequested()
    signal gridRequested()

    readonly property int rowSpacing: 1
    readonly property color panelText: "#ECEFF1"
    readonly property color mutedText: "#7D858B"
    readonly property color lineColor: "#2C3237"
    readonly property color rowColor: "#1B1F23"
    readonly property color accentColor: "#168FC4"
    readonly property color playingColor: "#E99128"

    // Ejecting mid-playback would cut the output, so the button only arms once
    // the deck is stopped — paused, or sitting at the end of the track.
    readonly property bool canEject: engine && engine.hasTrack && !engine.isPlaying

    color: "#14171A"
    border.color: selected ? accentColor : lineColor
    border.width: 1
    radius: 0

    function jump(beats) {
        if (!engine || !engine.hasTrack || engine.trackDurationSec <= 0)
            return
        var position = engine.getVisualPositionQml()
        var bpm = engine.currentBpm > 0 ? engine.currentBpm : 120
        position = Math.max(0, Math.min(engine.trackDurationSec, position + beats * 60 / bpm))
        engine.setPosition(position / engine.trackDurationSec)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: root.rowSpacing

        // ── Deck header ─────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            color: root.selected ? "#232A30" : "#1E2429"

            Rectangle {
                id: deckChip
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 7
                width: 6; height: 14
                color: root.selected ? root.accentColor : "#3A444B"
            }

            Text {
                anchors.left: deckChip.right
                anchors.leftMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                text: "DECK " + root.deckName
                color: root.panelText
                font.pixelSize: 12; font.weight: Font.DemiBold; font.letterSpacing: 0.8
            }

            Text {
                id: deckState
                anchors.right: ejectButton.left
                anchors.rightMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                text: root.engine && root.engine.isPlaying ? "PLAYING" : "READY"
                color: root.engine && root.engine.isPlaying ? root.playingColor : root.mutedText
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.6
            }

            // Eject sits directly beside the state caption, where the player is
            // reporting that it is not currently playing anything.
            Item {
                id: ejectButton
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 24; height: 24
                opacity: root.canEject ? 1.0 : 0.28

                Canvas {
                    id: ejectGlyph
                    anchors.centerIn: parent
                    width: 13; height: 13
                    property color glyphColor: ejectArea.containsMouse && root.canEject
                                               ? "#FFFFFF" : root.panelText
                    onGlyphColorChanged: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        ctx.fillStyle = glyphColor
                        ctx.beginPath()
                        ctx.moveTo(width * 0.5, 0)
                        ctx.lineTo(width, height * 0.62)
                        ctx.lineTo(0, height * 0.62)
                        ctx.closePath()
                        ctx.fill()
                        ctx.fillRect(0, height * 0.79, width, height * 0.21)
                    }
                }

                MouseArea {
                    id: ejectArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: root.canEject
                    onClicked: if (root.engine) root.engine.ejectTrack()
                }
            }

            MouseArea {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: ejectButton.left
                onClicked: root.selectedRequested()
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── Loaded track ────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 40
            color: root.rowColor

            Column {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.right: parent.right; anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Text {
                    width: parent.width
                    text: root.engine && root.engine.hasTrack ? root.engine.trackTitle : "NO TRACK"
                    color: root.engine && root.engine.hasTrack ? root.panelText : root.mutedText
                    font.pixelSize: 12; font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: root.engine && root.engine.trackArtist !== "" ? root.engine.trackArtist : "—"
                    color: root.mutedText; font.pixelSize: 10
                    elide: Text.ElideRight
                }
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── KEY ─────────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 34
            color: root.rowColor
            Text {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: "KEY"; color: root.mutedText
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.6
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: root.engine && root.engine.trackKey !== "" ? root.engine.trackKey : "—"
                color: root.panelText; font.pixelSize: 17
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── BPM ─────────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 34
            color: root.rowColor
            Text {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: "BPM"; color: root.mutedText
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.6
            }
            // Split so the decimal stays small, the way a player prints a tempo.
            Text {
                id: bpmDecimals
                anchors.right: parent.right; anchors.rightMargin: 9
                anchors.baseline: bpmWhole.baseline
                text: root.engine && root.engine.currentBpm > 0
                      ? "." + (Math.round(root.engine.currentBpm * 10) % 10) : ".-"
                color: root.panelText; font.pixelSize: 12; font.family: "monospace"
            }
            Text {
                id: bpmWhole
                anchors.right: bpmDecimals.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.engine && root.engine.currentBpm > 0
                      ? Math.floor(root.engine.currentBpm).toString() : "---"
                color: root.panelText; font.pixelSize: 19; font.family: "monospace"
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── BEAT JUMP ───────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 34
            color: root.rowColor
            Text {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: jumpMinus.left; anchors.rightMargin: 6
                text: "BEAT\nJUMP"; color: root.mutedText; lineHeight: 0.95
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.6
                elide: Text.ElideRight
            }
            Rectangle {
                id: jumpMinus
                anchors.right: jumpValue.left; anchors.rightMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                width: 22; height: Math.min(22, parent.height - 8)
                color: "#14171A"; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "−"; color: root.panelText; font.pixelSize: 13 }
                MouseArea { anchors.fill: parent; onClicked: root.beatJumpBeats = Math.max(0.5, root.beatJumpBeats / 2) }
            }
            Text {
                id: jumpValue
                anchors.right: jumpPlus.left; anchors.rightMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                text: root.beatJumpBeats.toString(); color: root.panelText
                font.pixelSize: 15; font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
                MouseArea { anchors.fill: parent; onClicked: root.jump(root.beatJumpBeats) }
            }
            Rectangle {
                id: jumpPlus
                anchors.right: parent.right; anchors.rightMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                width: 22; height: Math.min(22, parent.height - 8)
                color: "#14171A"; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "+"; color: root.panelText; font.pixelSize: 13 }
                MouseArea { anchors.fill: parent; onClicked: root.beatJumpBeats = Math.min(64, root.beatJumpBeats * 2) }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── Quantize ────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 30
            color: root.rowColor
            Text {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: "Q"
                color: root.engine && root.engine.quantizeEnabled ? root.playingColor : root.mutedText
                font.pixelSize: 12; font.weight: Font.DemiBold
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: root.engine && root.engine.quantizeEnabled ? "ON" : "OFF"
                color: root.engine && root.engine.quantizeEnabled ? root.panelText : root.mutedText
                font.pixelSize: 12; font.weight: Font.DemiBold
            }
            MouseArea {
                anchors.fill: parent
                onClicked: if (root.engine) root.engine.quantizeEnabled = !root.engine.quantizeEnabled
            }
        }
    }
}
