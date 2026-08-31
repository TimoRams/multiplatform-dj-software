import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

Item {
    id: root

    property var engine: null
    property string deckName: "A"
    property color backgroundColor: UiTheme.bgDisplay
    property real waveformZoom: 0.22
    property bool dropHovered: false
    property bool beatgridEditMode: false
    property bool showBeatgridEditor: true
    property bool sameTrackDoubleHint: false

    readonly property real renderDpr: {
        var value = Screen.devicePixelRatio
        return isFinite(value) && value > 0 ? value : 1.0
    }
    readonly property real physicalPixel: 1.0 / renderDpr
    readonly property real playheadCenterX:
        (Math.floor(width * 0.5 * renderDpr) + 0.5) / renderDpr
    readonly property bool waveformMotionActive:
        root.visible && root.engine !== null
        && (root.engine.isPlaying || root.engine.scratchVisualActive)
    readonly property bool slipPreviewActive:
        root.engine !== null && root.engine.slipPreviewActive
    readonly property int waveformMotionIntervalMs: {
        if (typeof renderPressurePolicy === "undefined" || !renderPressurePolicy)
            return 16
        return root.engine && root.engine.scratchVisualActive
            ? renderPressurePolicy.interactiveWaveformUpdateIntervalMs
            : renderPressurePolicy.waveformUpdateIntervalMs
    }

    function snappedLength(value) {
        return Math.max(physicalPixel, Math.round(value * renderDpr) / renderDpr)
    }

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
        id: deckRect
        anchors.fill: parent
        color: root.backgroundColor

        // Keep one full-height waveform geometry and reveal only its upper half
        // while diverted. This avoids compressing a complete waveform into each
        // pane: together, both panes still look like one waveform split at zero.
        Item {
            id: audibleWaveClip
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: root.slipPreviewActive ? parent.height * 0.5 : parent.height
            clip: true

            ScrollingWaveformItem {
                id: waveItem
                width: parent.width
                height: deckRect.height
                engine: root.engine
                pixelsPerPoint: root.waveformZoom
                backgroundColor: root.backgroundColor
                rasterWorkEnabled: (typeof renderPressurePolicy === "undefined"
                                    || !renderPressurePolicy)
                                   ? true
                                   : renderPressurePolicy.waveformRasterWorkEnabled
            }
        }

        Loader {
            id: slipWaveLoader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.5
            active: root.slipPreviewActive
            visible: active
            clip: true

            sourceComponent: Item {
                function requestUpdate() {
                    slipWaveItem.requestUpdate()
                }

                ScrollingWaveformItem {
                    id: slipWaveItem
                    width: slipWaveLoader.width
                    height: deckRect.height
                    y: -deckRect.height * 0.5
                    engine: root.engine
                    pixelsPerPoint: root.waveformZoom
                    backgroundColor: root.backgroundColor
                    slipPreview: true
                    opacity: 0.52
                    rasterWorkEnabled: (typeof renderPressurePolicy === "undefined"
                                        || !renderPressurePolicy)
                                       ? true
                                       : renderPressurePolicy.waveformRasterWorkEnabled
                }
            }
        }

        Binding {
            target: root.engine
            property: "pixelsPerSecond"
            value: waveItem.effectivePixelsPerSecond
            when: root.engine !== null
        }

        MouseArea {
            id: scrubArea
            anchors.fill: parent
            // Let clicks pass through to the grid overlay on the left strip.
            anchors.leftMargin: root.showBeatgridEditor ? beatgridPanel.occupiedWidth : 0
            preventStealing: true

            property real pressMouseX: 0
            property real pressPlayheadSec: 0
            property real lastDragPx: 0
            property bool scrubEngaged: false
            property real scrubDeadzonePx: 0.0

            onPressed: (mouse) => {
                if (root.engine === null) return
                scrubEngaged = false
                lastDragPx = 0
                pressMouseX = mouse.x + (root.showBeatgridEditor ? beatgridPanel.occupiedWidth : 0)
                if (root.beatgridEditMode && mouse.button === Qt.LeftButton) {
                    pressPlayheadSec = root.engine.getVisualPositionQml()
                    return
                }
                pressPlayheadSec = root.engine.getVisualPositionQml()
                root.engine.pauseForScrub(pressPlayheadSec)
            }

            onPositionChanged: (mouse) => {
                if (root.engine === null || root.beatgridEditMode) return
                const dragPx = (mouse.x + (root.showBeatgridEditor ? beatgridPanel.occupiedWidth : 0)) - pressMouseX

                if (!scrubEngaged) {
                    if (Math.abs(dragPx) < scrubDeadzonePx)
                        return
                    scrubEngaged = true
                    lastDragPx = dragPx
                    return
                }

                if (waveItem.effectivePixelsPerSecond <= 0.0)
                    return

                const deltaPx = dragPx - lastDragPx
                lastDragPx = dragPx
                if (Math.abs(deltaPx) < 0.01)
                    return

                // Vinyl pull: drag right = earlier in track (negative delta seconds).
                const deltaSec = waveItem.screenDeltaToSeconds(-deltaPx)
                root.engine.scratchBySeconds(deltaSec)
            }

            onReleased: (mouse) => {
                if (root.engine === null) return
                if (root.beatgridEditMode && mouse.button === Qt.LeftButton && !scrubEngaged) {
                    // Renderer: x = w/2 + (t - playhead) * pxPerSec  →  t = playhead + (x - w/2) / pxPerSec
                    // (Minus was wrong — felt mirrored; scrub drag uses minus because it's vinyl-pull.)
                    if (waveItem.effectivePixelsPerSecond > 0) {
                        var wavePos = scrubArea.mapToItem(waveItem, mouse.x, mouse.y)
                        var clickedSec = waveItem.timelineSecondsAtX(
                            wavePos.x, pressPlayheadSec)
                        root.engine.setDownbeatAtPosition(clickedSec)
                    }
                    return
                }
                root.engine.resumeAfterScrub()
                scrubEngaged = false
                lastDragPx = 0
            }

            onCanceled: {
                if (root.engine)
                    root.engine.resumeAfterScrub()
                scrubEngaged = false
                lastDragPx = 0
            }
        }

        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: (event) => {
                if (!waveformZoomController) {
                    event.accepted = false
                    return
                }
                if (event.angleDelta.y > 0)
                    waveformZoomController.zoomIn()
                else if (event.angleDelta.y < 0)
                    waveformZoomController.zoomOut()
                event.accepted = true
            }
        }

        // Normal motion is driven by Qt Quick's animation clock, so the
        // playhead snapshot is sampled exactly once for the frame the scene
        // graph is about to render. A free-running 16 ms timer slowly beats
        // against 59.94/90/120/144 Hz presentation and produces duplicate and
        // skipped waveform positions. Under audio pressure the frame driver is
        // stopped completely and the lower-rate disposable timer takes over.
        FrameAnimation {
            running: root.waveformMotionActive
                     && root.waveformMotionIntervalMs <= 17
            onTriggered: {
                waveItem.requestUpdate()
                if (slipWaveLoader.item)
                    slipWaveLoader.item.requestUpdate()
            }
        }

        Timer {
            id: reducedWaveUpdateTimer
            interval: root.waveformMotionIntervalMs
            repeat: true
            running: root.waveformMotionActive
                     && root.waveformMotionIntervalMs > 17
            onTriggered: {
                waveItem.requestUpdate()
                if (slipWaveLoader.item)
                    slipWaveLoader.item.requestUpdate()
            }
        }

        Connections {
            target: (typeof controlClock !== "undefined") ? controlClock : null
            property int pausedTickDivider: 0
            function onWaveformTick() {
                if (root.engine !== null && !root.engine.isPlaying
                        && !root.engine.scratchVisualActive
                        && (++pausedTickDivider % 4) === 0)
                    waveItem.requestUpdate()
            }
        }

        Connections {
            target: root.engine
            function onPlayingChanged() {
                if (!root.engine.isPlaying) waveItem.requestUpdate()
            }
            function onProgressChanged() {
                // The adaptive timer repaints during play/scratch; only refresh when paused idle.
                if (root.engine && !root.engine.isPlaying && !root.engine.scratchVisualActive)
                    waveItem.requestUpdate()
            }
            function onScrubbingChanged() { waveItem.requestUpdate() }
            function onSlipPreviewChanged() {
                waveItem.requestUpdate()
                if (slipWaveLoader.item)
                    slipWaveLoader.item.requestUpdate()
            }
        }

        // Playhead — always dead center of the full deck.
        Rectangle {
            id: playhead
            width: 3.0 / root.renderDpr
            height: parent.height
            color: UiTheme.playhead
            x: root.playheadCenterX - width * 0.5
            y: 0
            z: 10

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                width: root.snappedLength(12)
                height: root.snappedLength(6)
                color: UiTheme.playhead
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: root.snappedLength(12)
                height: root.snappedLength(6)
                color: UiTheme.playhead
            }
        }

        Rectangle {
            x: 0
            y: Math.floor(parent.height * 0.5 * root.renderDpr) / root.renderDpr
            width: parent.width
            height: root.physicalPixel
            color: "#24ffffff"
            z: 9
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.5
            visible: root.slipPreviewActive
            color: "#18000000"
            z: 8
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: root.physicalPixel
            color: UiTheme.separatorSubtle
            z: 22
        }

        // Grid editor overlays the left edge — does not shift waveform/playhead.
        BeatgridEditorPanel {
            id: beatgridPanel
            visible: root.showBeatgridEditor
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            engine: root.engine
            deckName: root.deckName
            accentColor: UiTheme.deckColor(root.deckName)
        }

        Binding {
            target: root
            property: "beatgridEditMode"
            value: root.showBeatgridEditor && beatgridPanel.editMode
        }

        // Same file doubled on multiple playing decks — comb-filtering hint.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 22
            z: 30
            visible: root.sameTrackDoubleHint
            color: "#cc1a1408"
            border.color: "#66ffb000"

            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.rightMargin: 8
                text: "Same track on multiple decks — may sound thin (comb filtering). Nudge, EQ, or polarity (−)."
                color: "#dfc08a"
                font.pixelSize: 9
                elide: Text.ElideRight
            }
        }
    }

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
}
