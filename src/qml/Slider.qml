import QtQuick
import QtQuick.Controls as Controls

Controls.Slider {
    id: control

    property bool centerFill: false

    implicitWidth:  orientation === Qt.Vertical ? 22 : 150
    implicitHeight: orientation === Qt.Vertical ? 150 : 22

    background: Rectangle {
        x: control.orientation === Qt.Horizontal ? control.leftPadding  : control.width  / 2 - 2
        y: control.orientation === Qt.Horizontal ? control.height / 2 - 2 : control.topPadding
        width:  control.orientation === Qt.Horizontal ? control.availableWidth  : 4
        height: control.orientation === Qt.Horizontal ? 4 : control.availableHeight
        radius: 1
        color:  "#1e1e1e"

        // Horizontal – left-fill
        Rectangle {
            visible: control.orientation === Qt.Horizontal && !control.centerFill
            x: 1; y: 1
            width:  Math.max(0, control.visualPosition * (parent.width - 2))
            height: parent.height - 2
            radius: 1
            color:  control.pressed ? "#555555" : "#3a3a3a"
        }

        // Horizontal – center-fill (bipolar)
        Rectangle {
            visible: control.orientation === Qt.Horizontal && control.centerFill
            y: 1; height: parent.height - 2; radius: 1
            color:  control.pressed ? "#555555" : "#3a3a3a"
            readonly property real midPx: parent.width / 2
            readonly property real posPx: 1 + control.visualPosition * (parent.width - 2)
            x:     Math.min(midPx, posPx)
            width: Math.max(0, Math.abs(posPx - midPx))
        }

        // Vertical – bottom-fill
        Rectangle {
            visible: control.orientation === Qt.Vertical && !control.centerFill
            x: 1
            y: parent.height - 1 - Math.max(0, (1.0 - control.visualPosition) * (parent.height - 2))
            width:  parent.width  - 2
            height: Math.max(0, (1.0 - control.visualPosition) * (parent.height - 2))
            radius: 1
            color:  control.pressed ? "#555555" : "#3a3a3a"
        }

        // Vertical – center-fill (bipolar)
        Rectangle {
            visible: control.orientation === Qt.Vertical && control.centerFill
            x: 1; width: parent.width - 2; radius: 1
            color:  control.pressed ? "#555555" : "#3a3a3a"
            readonly property real midPy: parent.height / 2
            readonly property real posPy: 1 + control.visualPosition * (parent.height - 2)
            y:      Math.min(midPy, posPy)
            height: Math.max(0, Math.abs(posPy - midPy))
        }
    }

    handle: Rectangle {
        implicitWidth:  control.orientation === Qt.Vertical ? 26 : 18
        implicitHeight: control.orientation === Qt.Vertical ? 12 : 22
        x: control.orientation === Qt.Horizontal
           ? control.leftPadding + control.visualPosition * (control.availableWidth - width)
           : control.width / 2 - width / 2
        y: control.orientation === Qt.Horizontal
           ? control.height / 2 - height / 2
           : control.topPadding + control.visualPosition * (control.availableHeight - height)
        radius: 1
        color:  control.pressed ? "#e0e0e0" : "#c8c8c8"

        // Center tick
        Rectangle {
            anchors.centerIn: parent
            width:  control.orientation === Qt.Vertical ? parent.width * 0.48 : 2
            height: control.orientation === Qt.Vertical ? 1 : parent.height * 0.48
            color:  "#888888"
        }
    }
}
