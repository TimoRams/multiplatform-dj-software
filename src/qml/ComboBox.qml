import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Window

Controls.ComboBox {
    id: control
    readonly property var hostWindow: control.Window.window
    function sp(px) {
        return (hostWindow && typeof hostWindow.sp === "function") ? hostWindow.sp(px) : px
    }

    implicitWidth: Math.max(120, sp(140))
    implicitHeight: Math.max(30, sp(28))
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
        font.pixelSize: control.sp(12)
        verticalAlignment: Text.AlignVCenter
        leftPadding: Math.max(8, Math.round(control.implicitHeight * 0.3))
        rightPadding: Math.max(20, Math.round(control.implicitHeight * 0.8))
        elide: Text.ElideRight
    }

    indicator: Rectangle {
        x: control.width - width - Math.max(7, Math.round(control.implicitHeight * 0.28))
        y: control.topPadding + (control.availableHeight - height) / 2
        width: Math.max(8, Math.round(control.implicitHeight * 0.32))
        height: width
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