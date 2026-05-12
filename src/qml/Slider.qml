import QtQuick
import QtQuick.Controls as Controls

Controls.Slider {
    id: control

    property bool centerFill: false
    property real defaultValue: 0.0
    property bool dragActive: false

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
        color:  control.pressed || control.dragActive ? "#e0e0e0" : "#c8c8c8"

        // Center tick
        Rectangle {
            anchors.centerIn: parent
            width:  control.orientation === Qt.Vertical ? parent.width * 0.48 : 2
            height: control.orientation === Qt.Vertical ? 1 : parent.height * 0.48
            color:  "#888888"
        }
    }

    // ── Drag-lock: cursor vanishes on drag, reappears at click origin ─────
    MouseArea {
        id: sliderDrag
        anchors.fill: parent
        z: 100
        acceptedButtons: Qt.LeftButton
        preventStealing: true

        property real _pressGX:  0
        property real _pressGY:  0
        property real _pressVal: 0
        property bool _active:   false

        onPressed: (mouse) => {
            var g    = sliderDrag.mapToGlobal(mouse.x, mouse.y)
            _pressGX  = g.x
            _pressGY  = g.y
            _pressVal = control.value
            _active   = false
            mouse.accepted = true
        }

        onPositionChanged: (mouse) => {
            var g     = sliderDrag.mapToGlobal(mouse.x, mouse.y)
            var isV   = control.orientation === Qt.Vertical
            // Vertical: up = increase (dy positive when mouse moved up)
            // Horizontal: right = increase (dx positive when mouse moved right)
            var delta = isV ? (_pressGY - g.y) : (g.x - _pressGX)
            if (!_active) {
                if (Math.abs(delta) < 4) return
                _active = true
                control.dragActive = true
                cursorControl.hideCursor()
            }
            var newVal = _pressVal + delta * (control.to - control.from) / 150.0
            var lo = Math.min(control.from, control.to)
            var hi = Math.max(control.from, control.to)
            control.value = Math.max(lo, Math.min(hi, newVal))
        }

        onReleased: {
            if (_active) {
                _active = false
                control.dragActive = false
                cursorControl.restoreCursor()
                cursorControl.moveCursor(_pressGX, _pressGY)
            }
        }

        onDoubleClicked: {
            control.enabled = false
            control.value   = control.defaultValue
            control.enabled = true
        }
    }
}
