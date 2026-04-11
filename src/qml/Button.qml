import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Window

Controls.Button {
    id: control
    readonly property var hostWindow: control.Window.window
    
    // Set to true for instances inside a `scale: window.uiScale` context (e.g. Deck, Mixer, Waveform)
    property bool useViewportScaling: false

    function sp(px) {
        if (!hostWindow) return px;
        if (useViewportScaling && typeof hostWindow.spViewport === "function") {
            return hostWindow.spViewport(px);
        }
        return (typeof hostWindow.sp === "function") ? hostWindow.sp(px) : px;
    }

    implicitWidth: Math.max(48, contentItem ? contentItem.implicitWidth + leftPadding + rightPadding : 72)
    implicitHeight: Math.max(30, sp(28))
    padding: 0
    leftPadding: Math.max(8, Math.round(implicitHeight * 0.28))
    rightPadding: leftPadding
    topPadding: 0
    bottomPadding: 0

    background: Rectangle {
        radius: 0
        color: control.down ? "#2b2b2b" : (control.hovered ? "#232323" : "#1f1f1f")
        border.width: 0

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: !control.enabled
                   ? "#2a2a2a"
                   : (control.down || control.checked)
                     ? "#ff9900"
                     : (control.hovered ? "#575757" : "#3a3a3a")
        }
    }

    contentItem: Text {
        text: control.text
        color: control.enabled ? "#f0f0f0" : "#777777"
        font.pixelSize: control.sp(12)
        font.bold: control.checked || control.down
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}