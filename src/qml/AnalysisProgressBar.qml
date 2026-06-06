import QtQuick

Item {
    id: root

    property var engine: null

    readonly property real progress: {
        if (!engine || !engine.trackData)
            return 0.0
        return Math.max(0.0, Math.min(1.0, engine.trackData.analysisProgress))
    }
    readonly property bool analyzing: engine && engine.trackData && engine.trackData.analyzing

    implicitHeight: 4

    Rectangle {
        anchors.fill: parent
        color: "#121212"
    }

    Rectangle {
        id: fill
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: parent.width * root.progress
        color: "#2b7cff"
        opacity: root.analyzing ? 0.92 : 0.0
        visible: root.analyzing && width > 0.5

        Behavior on width {
            enabled: root.analyzing
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 2
        x: Math.max(0, fill.width - 1)
        color: "#9fd0ff"
        opacity: root.analyzing ? 0.95 : 0.0
        visible: root.analyzing && fill.width > 2
    }

    Connections {
        target: root.engine && root.engine.trackData ? root.engine.trackData : null
        function onAnalysisProgressChanged() { }
    }
}
