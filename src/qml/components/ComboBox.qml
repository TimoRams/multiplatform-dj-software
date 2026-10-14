import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Window
import DJSoftware

Controls.ComboBox {
    id: control

    readonly property var hostWindow: control.Window.window

    // Set true inside a `scale: window.uiScale` context
    property bool useViewportScaling: false
    property string placeholderText: "Select..."

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
        color: UiTheme.buttonBg(false, control.hovered, control.pressed)
        border.width: 1
        border.color: control.visualFocus ? UiTheme.blue
                      : (control.hovered ? UiTheme.borderHover : UiTheme.border)
    }

    contentItem: Text {
        text:              control.displayText.length > 0 ? control.displayText : control.placeholderText
        color:             control.enabled ? UiTheme.textPrimary : UiTheme.textMuted
        font.family:       UiTheme.uiFontFamily
        font.pixelSize:    control.sp(12)
        renderType:        Text.QtRendering
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
            context.fillStyle = control.enabled ? UiTheme.textSecondary : UiTheme.textMuted
            context.fill()
        }
    }

    popup.background: Rectangle {
        radius: 0
        color: UiTheme.surfaceInset
        border.width: 1
        border.color: UiTheme.borderStrong
    }
}
