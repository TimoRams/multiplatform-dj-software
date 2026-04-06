import QtQuick
import QtQuick.Controls as Controls

Controls.ComboBox {
    id: control

    implicitWidth: 160
    implicitHeight: 32
    padding: 0

    background: Rectangle {
        radius: 0
        color: control.pressed ? "#2a2a2a" : "#1f1f1f"
        border.color: control.visualFocus ? "#5a5a5a" : "#3a3a3a"
        border.width: 1
    }

    contentItem: Text {
        text: control.displayText
        color: control.enabled ? "#f0f0f0" : "#777777"
        font.pixelSize: 12
        verticalAlignment: Text.AlignVCenter
        leftPadding: 10
        rightPadding: 26
        elide: Text.ElideRight
    }

    indicator: Rectangle {
        x: control.width - width - 9
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 10
        height: 10
        radius: 0
        color: control.enabled ? "#bbbbbb" : "#777777"
        border.color: "transparent"
    }

    popup.background: Rectangle {
        radius: 0
        color: "#171717"
        border.color: "#3a3a3a"
        border.width: 1
    }
}