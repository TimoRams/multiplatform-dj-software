import QtQuick

Item {
    id: root

    property var segments: []
    property real totalTrackDuration: 0

    implicitHeight: 10

    Rectangle {
        anchors.fill: parent
        radius: 2
        color: "#171717"
        border.color: "#2a2a2a"
        border.width: 1
    }

    Repeater {
        model: root.segments

        delegate: Rectangle {
            required property var modelData

            readonly property real segStart: Number(modelData.startTime)
            readonly property real segEnd: Number(modelData.endTime)
            readonly property real safeDuration: Math.max(0.001, root.totalTrackDuration)
            readonly property real startNorm: Math.min(1, Math.max(0, segStart / safeDuration))
            readonly property real endNorm: Math.min(1, Math.max(startNorm, segEnd / safeDuration))
            readonly property real rawWidth: (endNorm - startNorm) * root.width

            x: startNorm * root.width
            width: Math.max(1, rawWidth)
            height: root.height
            color: (modelData.colorHex && modelData.colorHex !== "") ? modelData.colorHex : "#555"
            opacity: 0.9
            radius: 1

            Text {
                anchors.centerIn: parent
                text: modelData.label || ""
                visible: parent.width >= 52
                color: "#111"
                font.pixelSize: window.spViewport(9)
                font.bold: true
                elide: Text.ElideRight
            }
        }
    }
}
