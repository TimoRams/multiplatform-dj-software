import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Window
import DJSoftware

Controls.Button {
    id: control

    readonly property var hostWindow: control.Window.window
    property bool useViewportScaling: false
    property color accentColor: UiTheme.deckA

    function sp(px) {
        if (!hostWindow) return px
        if (useViewportScaling && typeof hostWindow.spViewport === "function")
            return hostWindow.spViewport(px)
        return (typeof hostWindow.sp === "function") ? hostWindow.sp(px) : px
    }

    implicitHeight: Math.max(22, sp(22))
    implicitWidth:  Math.max(48, contentItem ? contentItem.implicitWidth + leftPadding + rightPadding : 64)
    padding:        0
    leftPadding:    Math.max(8, Math.round(implicitHeight * 0.28))
    rightPadding:   leftPadding
    topPadding:     0
    bottomPadding:  0

    background: Rectangle {
        radius: 0
        color:  UiTheme.buttonBg(control.checked || control.down, control.hovered, control.down)

        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: control.checked || control.down ? 2 : 1
            color: !control.enabled
                   ? UiTheme.separatorSubtle
                   : (control.checked || control.down)
                     ? accentColor
                     : (control.hovered ? UiTheme.borderHover : UiTheme.border)
            opacity: control.checked || control.down ? 1.0 : (control.hovered ? 0.55 : 0.35)
        }
    }

    contentItem: Text {
        text:               control.text
        color:              control.enabled
                            ? (control.checked || control.down ? UiTheme.textPrimary : UiTheme.textSecondary)
                            : UiTheme.textMuted
        font.pixelSize:     control.sp(12)
        font.bold:          control.checked || control.down
        font.letterSpacing: 0.4
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment:   Text.AlignVCenter
        elide:               Text.ElideRight
    }
}
