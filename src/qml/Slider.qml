import QtQuick
import QtQuick.Controls as Controls

Controls.Slider {
    id: control

    implicitWidth: orientation === Qt.Vertical ? 22 : 150
    implicitHeight: orientation === Qt.Vertical ? 150 : 22

    background: Rectangle {
        x: control.orientation === Qt.Horizontal ? control.leftPadding : control.width / 2 - 2
        y: control.orientation === Qt.Horizontal ? control.height / 2 - 2 : control.topPadding
        width: control.orientation === Qt.Horizontal ? control.availableWidth : 4
        height: control.orientation === Qt.Horizontal ? 4 : control.availableHeight
        radius: 0
        color: "#202020"
        border.color: "#3a3a3a"
        border.width: 1

        Rectangle {
            visible: control.orientation === Qt.Horizontal
            x: 1
            y: 1
            width: Math.max(0, control.visualPosition * (parent.width - 2))
            height: parent.height - 2
            radius: 0
            color: control.pressed ? "#5a5a5a" : "#3e3e3e"
        }

        Rectangle {
            visible: control.orientation === Qt.Vertical
            x: 1
            y: parent.height - 1 - Math.max(0, control.visualPosition * (parent.height - 2))
            width: parent.width - 2
            height: Math.max(0, control.visualPosition * (parent.height - 2))
            radius: 0
            color: control.pressed ? "#5a5a5a" : "#3e3e3e"
        }
    }

    handle: Rectangle {
        implicitWidth: Math.max(14, Math.round((control.orientation === Qt.Horizontal ? control.height : control.width) * 0.72))
        implicitHeight: implicitWidth
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.visualPosition * (control.availableHeight - height)
        radius: 0
        color: control.pressed ? "#e0e0e0" : "#c8c8c8"
        border.color: control.pressed ? "#707070" : "#444444"
        border.width: 1
    }
}