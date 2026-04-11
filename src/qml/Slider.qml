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
        border.width: 0

        Rectangle {
            visible: control.orientation === Qt.Horizontal
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: "#3a3a3a"
        }

        Rectangle {
            visible: control.orientation === Qt.Vertical
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: 1
            color: "#3a3a3a"
        }

        Rectangle {
            visible: control.orientation === Qt.Horizontal
            x: 0
            y: 0
            width: Math.max(0, control.visualPosition * parent.width)
            height: parent.height
            radius: 0
            color: control.pressed ? "#5a5a5a" : "#3e3e3e"
        }

        Rectangle {
            visible: control.orientation === Qt.Vertical
            x: 0
            y: parent.height - Math.max(0, control.visualPosition * parent.height)
            width: parent.width
            height: Math.max(0, control.visualPosition * parent.height)
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
        border.width: 0

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: control.pressed ? "#707070" : "#444444"
        }
    }
}