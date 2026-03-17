import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

Item {
    id: root
    
    property var engine: null 
    property color backgroundColor: "#1e1e1e"
    
    // Zoom controlled externally (main.qml) so both decks zoom in sync.
    property real waveformZoom: 1.5

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

        // Keep the engine's pixel-scale in sync so scrubBy() can do correct math.
        // Formula mirrors ScrollingWaveformItem: 150 waveform-points/s × ppp.
        Binding {
            target: root.engine
            property: "pixelsPerSecond"
            value: root.waveformZoom * 150.0
            when: root.engine !== null
        }

        // ─── Scrub / Scratch MouseArea ─────────────────────────────────────
        // Dragging horizontally "grabs" the waveform like a record:
        //   drag right → pull back in time   (playhead moves backward)
        //   drag left  → push forward in time (playhead moves forward)
        MouseArea {
            id: scrubArea
            anchors.fill: parent
            // Let vertical scrolling pass through to parent if needed.
            preventStealing: true

            property real lastMouseX: 0
            property bool wasPlayingBeforeScrub: false

            onPressed: (mouse) => {
                if (root.engine === null) return
                wasPlayingBeforeScrub = root.engine.isPlaying
                root.engine.pauseForScrub()
                lastMouseX = mouse.x
            }

            onPositionChanged: (mouse) => {
                if (root.engine === null) return
                let deltaX = mouse.x - lastMouseX
                root.engine.scrubBy(deltaX)
                // Keep waveform repainting during scrub (transport is stopped).
                waveItem.requestUpdate()
                lastMouseX = mouse.x
            }

            onReleased: {
                if (root.engine === null) return
                root.engine.resumeAfterScrub()
                wasPlayingBeforeScrub = false
            }
        }
        // ────────────────────────────────────────────────────────────────────

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
        // Also repaint when position changes while paused (e.g. seeking via overview).
        Connections {
            target: root.engine
            function onPlayingChanged() {
                if (!root.engine.isPlaying)
                    waveItem.requestUpdate()
            }
            function onProgressChanged() {
                // Repaint scrolling waveform when paused and position is changed
                // (e.g. clicking on the overview waveform to seek).
                if (root.engine && !root.engine.isPlaying)
                    waveItem.requestUpdate()
            }
            function onLoopChanged() {
                waveItem.requestUpdate()
            }
        }

        // beatgridChanged is emitted by TrackData, not DjEngine.
        // Connect to it separately so grid edits while paused refresh the display.
        Connections {
            target: root.engine ? root.engine.trackData : null
            function onBeatgridChanged() {
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

        // ─── Beat-grid toolbar (left edge overlay) ───────────────────────────
        // Three buttons stacked vertically:
        //   ▽  Set Downbeat  – make current playhead position beat-1 / bar-1
        //   ×2              – double the BPM (half-time correction)
        //   /2              – halve  the BPM (double-time correction)
        Rectangle {
            id: gridToolbar
            anchors.left:           parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin:     3
            width:  26
            height: 72
            color:  "#aa000000"
            radius: 4
            z: 20

            visible: root.engine !== null && root.engine.trackData !== undefined

            Column {
                anchors.centerIn: parent
                spacing: 2

                // ── Set-Downbeat button ──────────────────────────────────────
                // Icon: red vertical bar + downward triangle (Rekordbox style).
                Rectangle {
                    id: setDownbeatBtn
                    width: 20; height: 20
                    color:  setDownbeatHover.containsMouse ? "#55ffffff" : "transparent"
                    radius: 3

                    // Red downbeat bar
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom:           parent.bottom
                        anchors.bottomMargin:     2
                        width: 1.5; height: 10
                        color: "#e60000"; radius: 1
                    }
                    // Downward-pointing triangle (Canvas)
                    Canvas {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top:              parent.top
                        anchors.topMargin:        2
                        width: 7; height: 5
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.beginPath()
                            ctx.moveTo(0, 0)
                            ctx.lineTo(width, 0)
                            ctx.lineTo(width / 2, height)
                            ctx.closePath()
                            ctx.fillStyle = "#e60000"
                            ctx.fill()
                        }
                    }
                    MouseArea {
                        id: setDownbeatHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape:  Qt.PointingHandCursor
                        onClicked: { if (root.engine) root.engine.setDownbeatAtCurrentPosition() }
                    }
                    ToolTip.visible: setDownbeatHover.containsMouse
                    ToolTip.text:    "Set Downbeat here (rebuilds beat grid)"
                    ToolTip.delay:   600
                }

                // ── BPM ×2 button ────────────────────────────────────────────
                // Doubles BPM and rebuilds the grid (half-time correction).
                Rectangle {
                    id: doubleBpmBtn
                    width: 20; height: 20
                    color:  doubleBpmHover.containsMouse ? "#55ffffff" : "transparent"
                    radius: 3

                    Text {
                        anchors.centerIn: parent
                        text:  "×2"
                        color: "#e6e600"   // yellow — visually distinct
                        font.pixelSize: 9
                        font.bold: true
                    }
                    MouseArea {
                        id: doubleBpmHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape:  Qt.PointingHandCursor
                        onClicked: { if (root.engine) root.engine.doubleBpm() }
                    }
                    ToolTip.visible: doubleBpmHover.containsMouse
                    ToolTip.text:    "Double BPM (×2) — half-time correction"
                    ToolTip.delay:   600
                }

                // ── BPM ÷2 button ────────────────────────────────────────────
                // Halves BPM and rebuilds the grid (double-time correction).
                Rectangle {
                    id: halveBpmBtn
                    width: 20; height: 20
                    color:  halveBpmHover.containsMouse ? "#55ffffff" : "transparent"
                    radius: 3

                    Text {
                        anchors.centerIn: parent
                        text:  "/2"
                        color: "#00aaff"   // blue — visually distinct
                        font.pixelSize: 9
                        font.bold: true
                    }
                    MouseArea {
                        id: halveBpmHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape:  Qt.PointingHandCursor
                        onClicked: { if (root.engine) root.engine.halveBpm() }
                    }
                    ToolTip.visible: halveBpmHover.containsMouse
                    ToolTip.text:    "Halve BPM (/2) — double-time correction"
                    ToolTip.delay:   600
                }
            } // Column
        }
        // ─────────────────────────────────────────────────────────────────────
    }   // Rectangle (background + waveform stack)
}       // Item (root)

