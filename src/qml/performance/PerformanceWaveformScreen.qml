import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DJSoftware

Item {
    id: root
    property var deckAEngine: null
    property var deckBEngine: null
    property var fx: null
    property real waveformZoom: 0.22
    property string selectedDeck: "A"
    property string leftPanel: "deck" // closed | deck | grid
    property bool rightPanelOpen: false

    readonly property real panelWidth: Math.min(230, Math.max(176, width * 0.155))
    readonly property real handleWidth: 22
    readonly property var selectedEngine: selectedDeck === "A" ? deckAEngine : deckBEngine
    readonly property real renderDpr: {
        var value = Screen.devicePixelRatio
        return isFinite(value) && value > 0 ? value : 1.0
    }
    readonly property real separatorHeight:
        Math.max(1.0 / renderDpr, Math.round(2.0 * renderDpr) / renderDpr)
    readonly property real deckAHeight:
        Math.floor(Math.max(0, height - separatorHeight) * renderDpr * 0.5) / renderDpr
    readonly property real deckBY: deckAHeight + separatorHeight

    function toggleDeckPanel() { leftPanel = leftPanel === "deck" ? "closed" : "deck" }
    function openGrid() { leftPanel = "grid" }
    function closeLeftPanel() { leftPanel = "closed" }

    Rectangle { anchors.fill: parent; color: "#181B1E" }

    EnlargedWaveform {
        x: 0
        y: 0
        width: root.width
        height: root.deckAHeight
        deckName: "A"
        engine: root.deckAEngine
        backgroundColor: "#181B1E"
        waveformZoom: root.waveformZoom
        showBeatgridEditor: false
    }

    Rectangle {
        x: 0
        y: root.deckAHeight
        width: root.width
        height: root.separatorHeight
        color: "#555C62"
    }

    EnlargedWaveform {
        x: 0
        y: root.deckBY
        width: root.width
        height: Math.max(0, root.height - y)
        deckName: "B"
        engine: root.deckBEngine
        backgroundColor: "#181B1E"
        waveformZoom: root.waveformZoom
        showBeatgridEditor: false
    }

    // Waveform context controls stay independent from both side panels.
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        spacing: 3
        z: 20
        Rectangle {
            width: 42; height: 34; radius: 0; color: "#31363A"; border.color: "#555C62"; border.width: 1
            Text { anchors.centerIn: parent; text: "−"; color: "#F2F0D7"; font.pixelSize: 18 }
            MouseArea { anchors.fill: parent; onClicked: if (waveformZoomController) waveformZoomController.zoomOut() }
        }
        Rectangle {
            width: 54; height: 34; radius: 0; color: "#31363A"; border.color: "#555C62"; border.width: 1
            Text { anchors.centerIn: parent; text: "ZOOM"; color: "#F2F0D7"; font.pixelSize: 10; font.weight: Font.DemiBold }
        }
        Rectangle {
            width: 42; height: 34; radius: 0; color: "#31363A"; border.color: "#555C62"; border.width: 1
            Text { anchors.centerIn: parent; text: "+"; color: "#F2F0D7"; font.pixelSize: 18 }
            MouseArea { anchors.fill: parent; onClicked: if (waveformZoomController) waveformZoomController.zoomIn() }
        }
        Rectangle {
            width: 54; height: 34; radius: 0; color: root.leftPanel === "grid" ? "#4A3A23" : "#31363A"; border.color: "#E99128"; border.width: 1
            Text { anchors.centerIn: parent; text: "GRID"; color: "#F2F0D7"; font.pixelSize: 10; font.weight: Font.DemiBold }
            MouseArea { anchors.fill: parent; onClicked: root.openGrid() }
        }
    }

    Item {
        id: leftHost
        width: root.panelWidth
        anchors.top: parent.top; anchors.bottom: parent.bottom
        x: root.leftPanel === "closed" ? -width : 0
        z: 30
        Behavior on x { NumberAnimation { duration: 190; easing.type: Easing.OutCubic } }

        PerformanceBeatgridPanel {
            anchors.fill: parent
            visible: root.leftPanel === "grid"
            engine: root.selectedEngine
            deckName: root.selectedDeck
            onCloseRequested: root.closeLeftPanel()
        }

        Rectangle {
            anchors.fill: parent
            visible: root.leftPanel === "deck"
            color: "#171A1D"
            border.color: "#343B40"; border.width: 1
            ColumnLayout {
                anchors.fill: parent; spacing: 1
                PerformanceDeckQuickPanel {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    engine: root.deckAEngine; deckName: "A"; selected: root.selectedDeck === "A"
                    onSelectedRequested: root.selectedDeck = "A"
                    onGridRequested: root.openGrid()
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#343B40" }
                PerformanceDeckQuickPanel {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    engine: root.deckBEngine; deckName: "B"; selected: root.selectedDeck === "B"
                    onSelectedRequested: root.selectedDeck = "B"
                    onGridRequested: root.openGrid()
                }
            }
        }
    }

    Rectangle {
        id: leftHandle
        width: root.handleWidth; height: 42
        anchors.left: root.leftPanel === "closed" ? parent.left : leftHost.right
        anchors.verticalCenter: parent.verticalCenter
        color: "#24292D"; border.color: "#3D454B"; border.width: 1; radius: 2; z: 32
        Text { anchors.centerIn: parent; text: root.leftPanel === "closed" ? "›" : "‹"; color: "#D8DCDF"; font.pixelSize: 15 }
        MouseArea { anchors.fill: parent; onClicked: root.toggleDeckPanel() }
    }

    Item {
        id: rightHost
        width: root.panelWidth
        anchors.top: parent.top; anchors.bottom: parent.bottom
        x: root.rightPanelOpen ? root.width - width : root.width
        z: 30
        Behavior on x { NumberAnimation { duration: 190; easing.type: Easing.OutCubic } }
        PerformanceBeatFxPanel { anchors.fill: parent; fx: root.fx; onCloseRequested: root.rightPanelOpen = false }
    }

    Rectangle {
        id: rightHandle
        width: root.handleWidth; height: 42
        anchors.right: root.rightPanelOpen ? rightHost.left : parent.right
        anchors.verticalCenter: parent.verticalCenter
        color: "#24292D"; border.color: "#3D454B"; border.width: 1; radius: 2; z: 32
        Text { anchors.centerIn: parent; text: root.rightPanelOpen ? "›" : "‹"; color: "#D8DCDF"; font.pixelSize: 15 }
        MouseArea { anchors.fill: parent; onClicked: root.rightPanelOpen = !root.rightPanelOpen }
    }
}
