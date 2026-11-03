import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Window
import DJSoftware

Controls.Button {
    id: control

    readonly property var hostWindow: control.Window.window
    property bool useViewportScaling: false
    property bool touchTarget: !!hostWindow && hostWindow.allInOneMode === true
    property bool emphasized: checked
    property int labelSize: 12
    Accessible.name: text
    focusPolicy: Qt.StrongFocus
    property color accentColor: UiTheme.blue

    function sp(px) {
        if (!hostWindow) return px
        if (useViewportScaling && typeof hostWindow.spViewport === "function")
            return hostWindow.spViewport(px)
        return (typeof hostWindow.sp === "function") ? hostWindow.sp(px) : px
    }

    implicitHeight: touchTarget ? UiTheme.touchTarget : Math.max(UiTheme.controlHeightNormal, sp(30))
    implicitWidth:  Math.max(48, contentItem ? contentItem.implicitWidth + leftPadding + rightPadding : 64)
    padding:        0
    leftPadding:    Math.max(8, Math.round(implicitHeight * 0.28))
    rightPadding:   leftPadding
    topPadding:     0
    bottomPadding:  0

    background: Rectangle {
        radius: UiTheme.radiusSmall
        color: control.down ? UiTheme.controlHover
               : control.emphasized ? Qt.tint(UiTheme.surface, Qt.rgba(control.accentColor.r, control.accentColor.g, control.accentColor.b, 0.14))
               : control.hovered ? UiTheme.controlHover : UiTheme.surfaceRaised
        border.color: control.visualFocus ? UiTheme.focus
                      : (control.hovered ? UiTheme.borderHover : UiTheme.borderSubtle)

        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 1
            anchors.rightMargin: 1
            anchors.bottomMargin: 1
            height: 2
            visible: control.enabled && (control.emphasized || control.down)
            color: control.accentColor
        }
    }

    contentItem: Text {
        text:               control.text
        color:              control.enabled
                            ? (control.emphasized || control.down ? UiTheme.textPrimary : UiTheme.textSecondary)
                            : UiTheme.textMuted
        font.pixelSize:     control.sp(control.labelSize)
        font.family:        UiTheme.uiFontFamily
        font.weight:        Font.Medium
        renderType:         Text.QtRendering
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment:   Text.AlignVCenter
        elide:               Text.ElideRight
    }
}
