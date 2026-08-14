import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property var engine: null
    property string deckName: "A"
    property bool selected: false
    property real beatJumpBeats: 4
    signal selectedRequested()
    signal gridRequested()

    readonly property int tileGap: 3
    readonly property int panelMargin: 6
    readonly property color panelText: "#E7EAEC"
    readonly property color mutedText: "#9DA5AA"
    readonly property color lineColor: "#343B40"
    readonly property color tileColor: selected ? "#252D32" : "#20252A"
    readonly property color accentColor: "#168FC4"

    color: "#171A1D"
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
        anchors.margins: root.panelMargin
        spacing: root.tileGap

        // Compact, information-first deck strip.
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 36
            color: root.tileColor; border.color: root.selected ? root.accentColor : root.lineColor; border.width: 1
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 4; visible: root.selected; color: root.accentColor }
            Text {
                anchors.left: parent.left; anchors.leftMargin: 10
                anchors.right: deckState.left; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: "DECK " + root.deckName
                color: root.panelText; font.pixelSize: 12; font.weight: Font.DemiBold; font.letterSpacing: 0.6
                elide: Text.ElideRight
            }
            Text {
                id: deckState
                anchors.right: parent.right; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: root.engine && root.engine.isPlaying ? "PLAYING" : "READY"
                color: root.engine && root.engine.isPlaying ? "#E99128" : root.mutedText
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.4
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 36
            color: root.tileColor; border.color: root.lineColor; border.width: 1
            Column {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.right: parent.right; anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter; spacing: 2
                Text {
                    width: parent.width
                    text: root.engine && root.engine.hasTrack ? root.engine.trackTitle : "No track loaded"
                    color: root.panelText; font.pixelSize: 11; font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: root.engine && root.engine.trackArtist !== "" ? root.engine.trackArtist : "—"
                    color: root.mutedText; font.pixelSize: 9
                    elide: Text.ElideRight
                }
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 36
            color: root.tileColor; border.color: root.lineColor; border.width: 1
            Text {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: "KEY / BPM"; color: root.mutedText; font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.4
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: (root.engine && root.engine.trackKey !== "" ? root.engine.trackKey : "—")
                      + "  " + (root.engine && root.engine.currentBpm > 0 ? root.engine.currentBpm.toFixed(1) : "---.-")
                color: root.panelText; font.pixelSize: 13; font.family: "monospace"
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 36
            color: root.tileColor; border.color: root.lineColor; border.width: 1
            Text {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: "BEAT JUMP"; color: root.mutedText; font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.4
            }
            Rectangle {
                anchors.right: jumpValue.left; anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 25; height: Math.min(26, parent.height - 10)
                color: "#171A1D"; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "−"; color: root.panelText; font.pixelSize: 15 }
                MouseArea { anchors.fill: parent; onClicked: root.beatJumpBeats = Math.max(0.5, root.beatJumpBeats / 2) }
            }
            Text {
                id: jumpValue
                anchors.right: jumpPlus.left; anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 34
                text: root.beatJumpBeats.toString(); color: root.panelText; font.pixelSize: 14; font.family: "monospace"; horizontalAlignment: Text.AlignHCenter
            }
            Rectangle {
                id: jumpPlus
                anchors.right: parent.right; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 25; height: Math.min(26, parent.height - 10)
                color: "#171A1D"; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "+"; color: root.panelText; font.pixelSize: 15 }
                MouseArea { anchors.fill: parent; onClicked: root.beatJumpBeats = Math.min(64, root.beatJumpBeats * 2) }
            }
            MouseArea {
                anchors.left: parent.left; anchors.leftMargin: 84
                anchors.right: jumpValue.left; anchors.rightMargin: 38
                anchors.top: parent.top; anchors.bottom: parent.bottom
                onClicked: root.jump(root.beatJumpBeats)
            }
        }
    }
}
