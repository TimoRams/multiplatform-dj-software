import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Window

Controls.Button {
    id: control

    readonly property var hostWindow: control.Window.window

    // Set true inside a `scale: window.uiScale` context (Deck, Mixer, Waveform)
    property bool useViewportScaling: false

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
        color:  control.down ? "#2d2d2d" : (control.hovered ? "#252525" : "#1e1e1e")

        // Bottom accent line – replaces a full border for a cleaner look
        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: !control.enabled
                   ? "#2a2a2a"
                   : (control.down || control.checked)
                     ? "#ff9900"
                     : (control.hovered ? "#555555" : "#333333")
        }
    }

    contentItem: Text {
        text:               control.text
        color:              control.enabled ? "#e8e8e8" : "#444444"
        font.pixelSize:     control.sp(12)
        font.bold:          control.checked || control.down
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment:   Text.AlignVCenter
        elide:               Text.ElideRight
    }
}
