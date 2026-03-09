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
            id: waveItem
            anchors.fill: parent
            engine: root.engine
            pixelsPerPoint: root.waveformZoom
        }

        // VSync-synchrone Pull-Architektur:
        // FrameAnimation läuft nur wenn isPlaying == true (gebunden).
        // Jeder Frame: C++ atomic-Position lesen → requestUpdate() → updatePaintNode()
        // Bei Pause stoppt FrameAnimation sofort → kein Drift, kein Nachrutschen.
        // Kein Behavior / SmoothedAnimation / SpringAnimation hier oder in ScrollingWaveformItem.
        FrameAnimation {
            id: waveFrameAnim
            // Strict binding to play state — stops immediately on pause.
            running: root.engine !== null && root.engine.isPlaying
            onTriggered: {
                // Poll atomic C++ position each VSync frame.
                // ScrollingWaveformItem.updatePaintNode() reads engine.getVisualPosition()
                // directly, so just requesting a repaint is enough.
                waveItem.requestUpdate()
            }
        }

        // Also repaint once when pausing so the waveform freezes at the exact
        // stopped position (FrameAnimation has already stopped at this point).
        Connections {
            target: root.engine
            function onPlayingChanged() {
                if (!root.engine.isPlaying)
                    waveItem.requestUpdate()
            }
        }

        // --- FIXED PLAYHEAD (STATISCHER ABSPIELKOPF) ---
        Rectangle {
            id: playhead
            width: 2
            height: parent.height
            color: "red"
            anchors.centerIn: parent
            z: 10
            // NO Behavior, NO SmoothedAnimation, NO SpringAnimation on this element.
        }
    }
}
