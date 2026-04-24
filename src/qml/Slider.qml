import QtQuick
import QtQuick.Controls as Controls

Controls.Slider {
    id: control
    property bool centerFill: false

    implicitWidth: orientation === Qt.Vertical ? 22 : 150
    implicitHeight: orientation === Qt.Vertical ? 150 : 22

    background: Rectangle {
        x: control.orientation === Qt.Horizontal ? control.leftPadding : control.width / 2 - 2
        y: control.orientation === Qt.Horizontal ? control.height / 2 - 2 : control.topPadding
        width: control.orientation === Qt.Horizontal ? control.availableWidth : 4
        height: control.orientation === Qt.Horizontal ? 4 : control.availableHeight
        radius: 0
        color: "#1f1f1f"
        border.color: "#000000"
        border.width: 0

        Rectangle {
            visible: control.orientation === Qt.Horizontal && !control.centerFill
            x: 1
            y: 1
            width: Math.max(0, control.visualPosition * (parent.width - 2))
            height: parent.height - 2
            radius: 0
            color: control.pressed ? "#5a5a5a" : "#3e3e3e"
        }

        Rectangle {
            visible: control.orientation === Qt.Horizontal && control.centerFill
            y: 1
            height: parent.height - 2
            radius: 0
            color: control.pressed ? "#5a5a5a" : "#3e3e3e"

            readonly property real midPx: parent.width / 2
            readonly property real posPx: 1 + control.visualPosition * (parent.width - 2)

            x: Math.min(midPx, posPx)
            width: Math.max(0, Math.abs(posPx - midPx))
        }

        Rectangle {
            visible: control.orientation === Qt.Vertical && !control.centerFill
            x: 1
            y: parent.height - 1 - Math.max(0, (1.0 - control.visualPosition) * (parent.height - 2))
            width: parent.width - 2
            height: Math.max(0, (1.0 - control.visualPosition) * (parent.height - 2))
            radius: 0
            color: control.pressed ? "#5a5a5a" : "#3e3e3e"
        }

        Rectangle {
            visible: control.orientation === Qt.Vertical && control.centerFill
            x: 1
            width: parent.width - 2
            radius: 0
            color: control.pressed ? "#5a5a5a" : "#3e3e3e"

            readonly property real midPy: parent.height / 2
            readonly property real posPy: 1 + control.visualPosition * (parent.height - 2)

            y: Math.min(midPy, posPy)
            height: Math.max(0, Math.abs(posPy - midPy))
        }
    }

    handle: Rectangle {
        implicitWidth: control.orientation === Qt.Vertical ? 24 : 18
        implicitHeight: control.orientation === Qt.Vertical ? 12 : 22
        x: control.orientation === Qt.Horizontal
           ? control.leftPadding + control.visualPosition * (control.availableWidth - width)
           : control.width / 2 - width / 2
        y: control.orientation === Qt.Horizontal
           ? control.height / 2 - height / 2
           : control.topPadding + control.visualPosition * (control.availableHeight - height)
        radius: 0
        color: control.pressed ? "#e0e0e0" : "#c8c8c8"
        border.color: control.pressed ? "#707070" : "#444444"
        border.width: 0
    }
}