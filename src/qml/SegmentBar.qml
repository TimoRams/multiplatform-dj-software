import QtQuick

Item {
    id: root

    property var segments: []
    property real totalTrackDuration: 0

    implicitHeight: 10

    Rectangle {
        anchors.fill: parent
        radius: 0
        color: "#171717"
        border.color: "#2a2a2a"
        border.width: 0
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
            readonly property bool trusted: (modelData.label || "") !== "Unknown"
                                            && Number(modelData.confidence || 0) >= 0.45

            x: startNorm * root.width
            width: trusted ? Math.max(1, rawWidth) : 0
            height: root.height
            color: (modelData.colorHex && modelData.colorHex !== "") ? modelData.colorHex : "#555"
            opacity: trusted ? 0.72 : 0.0
            radius: 0
            visible: trusted

            Text {
                anchors.centerIn: parent
                text: parent.trusted ? (modelData.label || "") : ""
                visible: parent.trusted && parent.width >= 72
                color: "#111"
                font.pixelSize: window.spViewport(9)
                font.bold: true
                elide: Text.ElideRight
            }
        }
    }
}
