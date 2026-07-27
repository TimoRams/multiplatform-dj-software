import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property var engine: null
    property string deckName: "A"
    signal closeRequested()

    readonly property int headerHeight: 48
    readonly property int rowHeight: 48
    readonly property int panelMargin: 10

    color: "#252A2E"
    border.color: "#555C62"
    border.width: 1
    radius: 0

    component ActionButton: Rectangle {
        required property string label
        property color accent: "#E99128"
        property bool active: false
        signal clicked()
        Layout.fillWidth: true
        Layout.preferredHeight: root.rowHeight
        radius: 0
        color: buttonMouse.pressed ? "#41474B" : (active ? "#3B3326" : "#31363A")
        border.color: active ? accent : "#555C62"
        border.width: 1
        Text {
            anchors.centerIn: parent
            text: parent.label
            color: parent.active ? parent.accent : "#F2F0D7"
            font.pixelSize: 12
            font.weight: Font.DemiBold
            font.letterSpacing: 0.5
        }
        MouseArea { id: buttonMouse; anchors.fill: parent; onClicked: parent.clicked() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.headerHeight
            color: "#555952"
            Text {
                anchors.left: parent.left; anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "‹"
                color: "#F2F0D7"; font.pixelSize: 24
            }
            Text {
                anchors.left: parent.left; anchors.leftMargin: 36
                anchors.right: parent.right; anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "BEATGRID  ·  DECK " + root.deckName
                color: "#F2F0D7"; font.pixelSize: 13; font.weight: Font.DemiBold
                font.letterSpacing: 0.8
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
            }
            MouseArea { anchors.fill: parent; onClicked: root.closeRequested() }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: root.rowHeight
            Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin
            Text { text: "TEMPO"; color: "#C5C9C2"; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.5 }
            Item { Layout.fillWidth: true }
            Text {
                Layout.maximumWidth: Math.max(56, parent.width - 82)
                text: root.engine && root.engine.trackData && root.engine.trackData.isBpmAnalyzed
                      ? root.engine.trackData.bpm.toFixed(2) + " BPM" : "BPM —"
                color: "#F2F0D7"; font.pixelSize: 18; font.family: "monospace"
                elide: Text.ElideRight; horizontalAlignment: Text.AlignRight
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#555C62" }

        GridLayout {
            Layout.fillWidth: true
            Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin
            Layout.topMargin: root.panelMargin
            columns: 2
            columnSpacing: 4; rowSpacing: 4
            ActionButton { label: "÷2 BPM"; onClicked: if (root.engine) root.engine.halveBpm() }
            ActionButton { label: "×2 BPM"; onClicked: if (root.engine) root.engine.doubleBpm() }
            ActionButton { label: "− 1 BEAT"; onClicked: if (root.engine) root.engine.nudgeBeatgridBeats(-1) }
            ActionButton { label: "+ 1 BEAT"; onClicked: if (root.engine) root.engine.nudgeBeatgridBeats(1) }
            ActionButton { label: "− 10 ms"; onClicked: if (root.engine) root.engine.nudgeBeatgridMs(-10) }
            ActionButton { label: "+ 10 ms"; onClicked: if (root.engine) root.engine.nudgeBeatgridMs(10) }
            ActionButton { label: "SET DOWNBEAT"; onClicked: if (root.engine) root.engine.setDownbeatAtCurrentPosition() }
            ActionButton {
                label: root.engine && root.engine.beatgridLocked ? "GRID LOCKED" : "LOCK GRID"
                active: root.engine && root.engine.beatgridLocked
                onClicked: if (root.engine) root.engine.beatgridLocked = !root.engine.beatgridLocked
            }
        }

        Item { Layout.fillHeight: true }
    }
}
