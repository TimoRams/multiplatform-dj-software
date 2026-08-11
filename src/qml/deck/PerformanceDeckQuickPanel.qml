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

    readonly property int tileGap: 4
    readonly property int panelMargin: 8
    readonly property color panelText: "#F2F0D7"
    readonly property color mutedText: "#C5C9C2"
    readonly property color lineColor: "#555C62"
    readonly property color tileColor: selected ? "#30383C" : "#31363A"
    readonly property color accentColor: "#129AD1"

    color: "#252A2E"
    border.color: selected ? accentColor : "#555C62"
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

        // Four identical tiles per deck form the stable XDJ-style grid.
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 36
            color: root.tileColor; border.color: root.selected ? root.accentColor : root.lineColor; border.width: 1
            Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 4; visible: root.selected; color: root.accentColor }
            Text {
                anchors.left: parent.left; anchors.leftMargin: 14
                anchors.right: deckState.left; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: "DECK " + root.deckName
                color: root.panelText; font.pixelSize: 13; font.weight: Font.DemiBold; font.letterSpacing: 0.8
                elide: Text.ElideRight
            }
            Text {
                id: deckState
                anchors.right: parent.right; anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: root.engine && root.engine.isPlaying ? "PLAYING" : "READY"
                color: root.engine && root.engine.isPlaying ? "#E99128" : root.mutedText
                font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 0.5
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 36
            color: root.tileColor; border.color: root.lineColor; border.width: 1
            Text {
                anchors.left: parent.left; anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "SOURCE"; color: root.mutedText; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.5
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "LIBRARY"; color: root.panelText; font.pixelSize: 13; font.weight: Font.DemiBold
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 36
            color: root.tileColor; border.color: root.lineColor; border.width: 1
            Text {
                anchors.left: parent.left; anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "KEY"; color: root.mutedText; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.5
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: root.engine && root.engine.trackKey !== "" ? root.engine.trackKey : "—"
                color: root.panelText; font.pixelSize: 18; font.family: "monospace"
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 36
            color: root.tileColor; border.color: root.lineColor; border.width: 1
            Text {
                anchors.left: parent.left; anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "BEAT JUMP"; color: root.mutedText; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.5
            }
            Rectangle {
                anchors.right: jumpValue.left; anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 30; height: Math.min(30, parent.height - 12)
                color: "#252A2E"; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "−"; color: root.panelText; font.pixelSize: 18 }
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
                width: 30; height: Math.min(30, parent.height - 12)
                color: "#252A2E"; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "+"; color: root.panelText; font.pixelSize: 18 }
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
