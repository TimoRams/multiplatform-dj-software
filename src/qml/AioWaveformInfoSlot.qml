import QtQuick
import QtQuick.Layouts

Item {
    id: root

    property string slotPosition: "upper"

    Layout.fillWidth: true
    Layout.fillHeight: true

    readonly property string slotLabel: slotPosition === "lower" ? "Controls / Info" : "Controls / Info"

    Rectangle {
        anchors.fill: parent
        color: UiTheme.bgDisplay

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: UiTheme.separatorSubtle
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: UiTheme.separatorSubtle
        }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 4
            width: Math.min(parent.width - 24, 420)

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: root.slotLabel
                color: UiTheme.textSecondary
                font.pixelSize: 11
                font.bold: true
                font.family: "monospace"
                font.letterSpacing: 0.6
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                text: slotPosition === "lower"
                      ? "Reserved for deck info & touch controls"
                      : "Reserved for deck info & touch controls"
                color: UiTheme.textMuted
                font.pixelSize: 9
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }
}
