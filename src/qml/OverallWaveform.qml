import QtQuick
import QtQuick.Layouts
import DJSoftware

Item {
    id: root

    property var engine: null
    property color stripeColor: "#2a2a2a"

    Layout.fillWidth: true
    Layout.preferredHeight: 44

    Rectangle {
        anchors.fill: parent
        color: root.stripeColor
        border.color: "#333"
        border.width: 1

        // Rectified (half-wave) overview — baseline at bottom, draws upward only
        RgbWaveformItem {
            id: overview
            anchors.fill: parent
            anchors.margins: 2
            engine: root.engine
            rectified: true
        }

        // Scrubbing / seeking
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            
            onPressed: (mouse) => seekTo(mouse.x)
            
            onPositionChanged: (mouse) => seekTo(mouse.x)

            function seekTo(xPos) {
                if (root.engine) {
                    var progress = xPos / width;
                    if (progress < 0.0) progress = 0.0;
                    if (progress > 1.0) progress = 1.0;

                    root.engine.setPosition(progress);
                }
            }
        }
    }
}
