import QtQuick
import QtQuick.Controls as Controls

Controls.Button {
    id: control

    implicitWidth: Math.max(48, contentItem ? contentItem.implicitWidth + leftPadding + rightPadding : 72)
    implicitHeight: 32
    padding: 0
    leftPadding: 10
    rightPadding: 10
    topPadding: 0
    bottomPadding: 0

    background: Rectangle {
        radius: 0
        color: control.down ? "#2b2b2b" : (control.hovered ? "#232323" : "#1f1f1f")
        border.color: control.down ? "#5a5a5a" : (control.hovered ? "#575757" : "#3a3a3a")
        border.width: 1
    }

    contentItem: Text {
        text: control.text
        color: control.enabled ? "#f0f0f0" : "#777777"
        font.pixelSize: 12
        font.bold: control.checked || control.down
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}