import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Window

Controls.ComboBox {
    id: control

    readonly property var hostWindow: control.Window.window

    // Set true inside a `scale: window.uiScale` context
    property bool useViewportScaling: false

    function sp(px) {
        if (!hostWindow) return px
        if (useViewportScaling && typeof hostWindow.spViewport === "function")
            return hostWindow.spViewport(px)
        return (typeof hostWindow.sp === "function") ? hostWindow.sp(px) : px
    }

    implicitHeight: Math.max(22, sp(22))
    implicitWidth:  Math.max(120, sp(140))
    padding: 0

    background: Rectangle {
        radius: 0
        color:  control.pressed ? "#2d2d2d" : "#1e1e1e"

        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: 1
            color:  control.visualFocus ? "#ff9900" : "#333333"
        }
    }

    contentItem: Text {
        text:              control.displayText
        color:             control.enabled ? "#e8e8e8" : "#444444"
        font.pixelSize:    control.sp(12)
        verticalAlignment: Text.AlignVCenter
        leftPadding:  Math.max(8, Math.round(control.implicitHeight * 0.3))
        rightPadding: Math.max(20, Math.round(control.implicitHeight * 0.8))
        elide: Text.ElideRight
    }

    indicator: Canvas {
        x: control.width - width - Math.max(7, Math.round(control.implicitHeight * 0.28))
        y: control.topPadding + (control.availableHeight - height) / 2
        width:  Math.max(8, Math.round(control.implicitHeight * 0.32))
        height: Math.round(width * 0.65)
        contextType: "2d"

        onPaint: {
            context.reset()
            context.moveTo(0, 0)
            context.lineTo(width, 0)
            context.lineTo(width / 2, height)
            context.closePath()
            context.fillStyle = control.enabled ? "#999999" : "#444444"
            context.fill()
        }
    }

    popup.background: Rectangle {
        radius: 0
        color:  "#181818"

        Rectangle {
            anchors.left:  parent.left
            anchors.right: parent.right
            anchors.top:   parent.top
            height: 1
            color:  "#333333"
        }
    }
}
