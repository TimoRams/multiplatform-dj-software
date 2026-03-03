import QtQuick
import QtQuick.Layouts
import DJSoftware

Item {
    id: root
    
    property var engine: null 
    property color backgroundColor: "#1e1e1e"
    
    // Zoom controlled externally (main.qml) so both decks zoom in sync.
    property real waveformZoom: 3.0

    Layout.fillWidth: true
    Layout.fillHeight: true

    DropArea {
        anchors.fill: parent
        // Visuelles Feedback
        Rectangle {
            anchors.fill: parent
            color: root.backgroundColor == "#222" ? "cyan" : "magenta" // deck A = cyan, B = magenta
            opacity: parent.containsDrag ? 0.2 : 0.0
            visible: parent.containsDrag
        }

        onEntered: (drag) => {
             drag.accept(Qt.LinkAction);
        }

         onDropped: (drop) => {
            if (drop.hasUrls && drop.urls.length > 0) {
                 var path = drop.urls[0].toString();
                 if (path.startsWith("file://")) {
                      path = path.substring(7);
                 }
                 console.log("Loading into Enlarged Waveform: " + path);
                 if (root.engine) {
                     root.engine.loadTrack(path);
                 }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
        
        // --- SCROLLING WAVEFORM (Echtes C++ Rendering) ---
        ScrollingWaveformItem {
            anchors.fill: parent
            engine: root.engine
            pixelsPerPoint: root.waveformZoom
        }

        // --- FIXED PLAYHEAD (STATISCHER ABSPIELKOPF) ---
        Rectangle {
            id: playhead
            width: 2
            height: parent.height
            color: "red"
            anchors.centerIn: parent
            z: 10
        }
    }
}
