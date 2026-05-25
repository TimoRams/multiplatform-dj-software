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
    property bool dropHovered: false

    Layout.fillWidth: true
    Layout.fillHeight: true

    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list", "text/plain"]
        onEntered: (drag) => { drag.accept(Qt.CopyAction); root.dropHovered = true }
        onExited:  ()      => { root.dropHovered = false }
        onDropped: (drop)  => {
            root.dropHovered = false
            var path = ""
            if (drop.hasUrls && drop.urls.length > 0) path = drop.urls[0].toString()
            else if (drop.hasText)                    path = drop.text
            if (path.startsWith("file://")) path = path.substring(7)
            if (path !== "" && root.engine) root.engine.loadTrack(path)
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
        // Formula mirrors ScrollingWaveformItem: waveformPointsPerSecond × ppp.
        Binding {
            target: root.engine
            property: "pixelsPerSecond"
            value: root.engine ? (root.waveformZoom * root.engine.waveformPointsPerSecond) : 0
            when: root.engine !== null
        }

        // Loop overlay is rendered by ScrollingWaveformItem (C++ scene graph node).
        // No QML overlay needed here.

        // ─── Scrub / Scratch MouseArea ─────────────────────────────────────
        // Dragging horizontally "grabs" the waveform like a record:
        //   drag right → pull back in time   (playhead moves backward)
        //   drag left  → push forward in time (playhead moves forward)
        MouseArea {
            id: scrubArea
            anchors.fill: parent
            // Let vertical scrolling pass through to parent if needed.
            preventStealing: true

            property real pressMouseX: 0
            property real pressPlayheadSec: 0
            property bool scrubEngaged: false
            property real scrubDeadzonePx: 0.0

            onPressed: (mouse) => {
                if (root.engine === null) return
                scrubEngaged = false
                pressMouseX = mouse.x
                root.engine.pauseForScrub()
                // Anchor exact playhead at grab time for true 1:1 dragging.
                pressPlayheadSec = root.engine.getVisualPositionQml()
            }

            onPositionChanged: (mouse) => {
                if (root.engine === null) return
                const dragPx = mouse.x - pressMouseX

                if (!scrubEngaged && Math.abs(dragPx) < scrubDeadzonePx)
                    return

                scrubEngaged = true

                // Match renderer scale: effective pps = (waveformZoom * points/sec) / tempoRatio.
                var tempoRatio = Math.max(0.05, Math.abs(root.engine.tempoRatio))
                var effectivePixelsPerSecond = (root.waveformZoom * root.engine.waveformPointsPerSecond) / tempoRatio
                if (effectivePixelsPerSecond <= 0.0)
                    return

                // Send the raw (unbounded) virtual position to C++ — loop wrapping
                // is handled exclusively in setScrubPosition() so velocity remains
                // correct at any speed, including multiple loop crossings per frame.
                const targetSec = pressPlayheadSec - (dragPx / effectivePixelsPerSecond)
                root.engine.setScrubPosition(targetSec)

                // Keep waveform repainting during scrub (transport is stopped).
                waveItem.requestUpdate()
            }

            onReleased: {
                if (root.engine === null) return
                root.engine.resumeAfterScrub()
                scrubEngaged = false
            }

            onCanceled: {
                if (root.engine)
                    root.engine.resumeAfterScrub()
                scrubEngaged = false
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

        // While paused, keep a lightweight repaint heartbeat so manual turntable
        // movement and non-frame-driven position changes stay visually in sync.
        Timer {
            id: pausedWaveRefresh
            interval: 33
            repeat: true
            running: root.engine !== null && (!root.engine.isPlaying || root.engine.scrubbing)
            onTriggered: waveItem.requestUpdate()
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
                // Repaint scrolling waveform on paused seek and during scratch.
                if (root.engine && (!root.engine.isPlaying || root.engine.scrubbing))
                    waveItem.requestUpdate()
            }
            function onLoopChanged() {
                waveItem.requestUpdate()
            }
            function onScrubbingChanged() {
                waveItem.requestUpdate()
            }
            function onHotCuesChanged() {
                waveItem.requestUpdate()
            }
            function onTempoChanged() {
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

        // ─── Beat-grid toolbar (left edge, full deck height) ────────────────
        Rectangle {
            id: gridToolbar
            anchors.left:   parent.left
            anchors.top:    parent.top
            anchors.bottom: parent.bottom
            width: 30
            color: "#cc0d0d0d"
            z: 20
            visible: root.engine !== null && root.engine.trackData !== undefined

            // Right border for visual separation from waveform
            Rectangle {
                anchors.right:  parent.right
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: "#28ffffff"
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // ── Set-Downbeat button ──────────────────────────────────────
                Rectangle {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    color: setDownbeatHover.containsMouse ? "#22ffffff" : "transparent"

                    Rectangle {
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.bottom: parent.bottom; height: 1
                        color: "#22ffffff"
                    }

                    Item {
                        anchors.centerIn: parent
                        width: 12; height: 14

                        Canvas {
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 8; height: 5
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
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            width: 2; height: 7
                            color: "#e60000"
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
                Rectangle {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    color: doubleBpmHover.containsMouse ? "#22ffffff" : "transparent"

                    Rectangle {
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.bottom: parent.bottom; height: 1
                        color: "#22ffffff"
                    }

                    Text {
                        anchors.centerIn: parent
                        text:  "×2"
                        color: "#e6e600"
                        font.pixelSize: 10
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
                Rectangle {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    color: halveBpmHover.containsMouse ? "#22ffffff" : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text:  "/2"
                        color: "#00aaff"
                        font.pixelSize: 10
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
            }
        }
        // ─────────────────────────────────────────────────────────────────────
    }   // Rectangle (background + waveform stack)

    // ── Drop-hover overlay (above all waveform content) ───────────────────
    Rectangle {
        anchors.fill: parent
        z: 50
        color: "#5599ff"
        opacity: root.dropHovered ? 0.08 : 0.0
        Behavior on opacity { NumberAnimation { duration: 80 } }
    }
    Rectangle {
        anchors.fill: parent
        z: 50
        color: "transparent"
        border.color: "#5599ff"
        border.width: 3
        visible: root.dropHovered
    }
}       // Item (root)

