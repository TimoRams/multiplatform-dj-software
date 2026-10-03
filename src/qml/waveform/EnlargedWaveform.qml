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
    readonly property bool waveformMotionActive:
        root.visible && root.engine !== null
        && (root.engine.isPlaying || root.engine.scratchVisualActive)
    readonly property bool slipPreviewActive:
        root.engine !== null && root.engine.slipPreviewActive
    readonly property bool slipRendererReady:
        root.slipPreviewActive && slipWaveLoader.item !== null
        && slipWaveLoader.item.contentReady
    readonly property int waveformMotionIntervalMs: {
        if (typeof renderPressurePolicy === "undefined" || !renderPressurePolicy)
            return 16
        return root.engine && root.engine.scratchVisualActive
            ? renderPressurePolicy.interactiveWaveformUpdateIntervalMs
            : renderPressurePolicy.waveformUpdateIntervalMs
    }

    component BeatgridEditorPanel: Rectangle {
        id: root

        property var engine: null
        property string deckName: "A"
        property color accentColor: "#888888"
        property bool editMode: false
        property bool expanded: false

        readonly property real deckHeight: parent ? parent.height : 80
        readonly property real deckWidth: parent ? parent.width : 400
        readonly property real btnSize: Math.max(20, Math.min(28, Math.floor(deckHeight / 3.2)))
        readonly property real maxRows: Math.min(3, Math.max(1, Math.floor((deckHeight - 6) / (btnSize + 2))))
        readonly property real cellW: btnSize + 2
        readonly property real cellH: btnSize + 2

        readonly property var toolItems: [
            { id: "collapse", label: "◂", tip: "Collapse", type: "action", action: "collapse" },
            { id: "edit",     label: "✎", tip: "Click waveform → set downbeat", type: "toggle", action: "edit" },
            { id: "bpm",      label: "",  tip: "BPM", type: "bpm" },
            { id: "downbeat", label: "I", tip: "Set downbeat at playhead", type: "downbeat", action: "downbeat" },
            { id: "double",   label: "×2",tip: "Double BPM", type: "action", action: "double" },
            { id: "halve",    label: "/2",tip: "Halve BPM", type: "action", action: "halve" },
            { id: "nudge-1b", label: "-1b", tip: "-1 beat", type: "action", action: "nudge-1b" },
            { id: "nudge-10", label: "-10", tip: "-10 ms", type: "action", action: "nudge-10" },
            { id: "nudge+10", label: "+10", tip: "+10 ms", type: "action", action: "nudge+10" },
            { id: "nudge+1b", label: "+1b", tip: "+1 beat", type: "action", action: "nudge+1b" },
            { id: "lock",     label: "🔓", tip: "Lock grid", type: "toggle", action: "lock" }
        ]

        readonly property int flowCols: Math.max(1, Math.ceil(toolItems.length / maxRows))
        readonly property real flowContentW: flowCols * cellW + 6
        readonly property real collapsedStripWidth: 30
        readonly property real expandedWidth: Math.min(deckWidth * 0.55, Math.max(96, flowContentW))
        readonly property real occupiedWidth: expanded ? expandedWidth : collapsedStripWidth

        width: occupiedWidth
        height: parent ? parent.height : implicitHeight
        color: "#dd0a0a0a"
        z: 25
        clip: true

        Behavior on width { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }

        function runAction(action) {
            if (!root.engine && action !== "collapse" && action !== "edit") return
            switch (action) {
            case "collapse": root.expanded = false; break
            case "edit":     root.editMode = !root.editMode; break
            case "downbeat": root.engine.setDownbeatAtCurrentPosition(); break
            case "double":   root.engine.doubleBpm(); break
            case "halve":    root.engine.halveBpm(); break
            case "nudge-1b": root.engine.nudgeBeatgridBeats(-1); break
            case "nudge-10": root.engine.nudgeBeatgridMs(-10); break
            case "nudge+10": root.engine.nudgeBeatgridMs(10); break
            case "nudge+1b": root.engine.nudgeBeatgridBeats(1); break
            case "lock":     lockBox.checked = !lockBox.checked; break
            }
        }

        CheckBox {
            id: lockBox
            visible: false
            enabled: root.engine !== null
            checked: root.engine ? root.engine.beatgridLocked : false
            onToggled: { if (root.engine) root.engine.beatgridLocked = checked }
        }

        Connections {
            target: root.engine
            function onBeatgridLockedChanged() {
                if (root.engine) lockBox.checked = root.engine.beatgridLocked
            }
        }

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: "#30ffffff"
        }

        // Collapsed strip
        Item {
            anchors.fill: parent
            visible: !root.expanded

            Column {
                anchors.centerIn: parent
                spacing: 1
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.deckName
                    color: root.accentColor
                    font.pixelSize: Math.max(12, Math.min(18, root.deckHeight * 0.25))
                    font.bold: true
                    font.family: "monospace"
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "›"
                    color: stripHover.containsMouse ? "#d7dde2" : "#77818a"
                    font.pixelSize: Math.max(11, Math.min(15, root.deckHeight * 0.2))
                    font.bold: true
                }
            }
            MouseArea {
                id: stripHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.expanded = true
            }
            ToolTip.visible: stripHover.containsMouse
            ToolTip.text: "Deck " + root.deckName + " beat grid menu"
            ToolTip.delay: 500
        }

        // Expanded — Flow: max 3 rows, rest flows horizontally
        Item {
            anchors.fill: parent
            visible: root.expanded

            Flow {
                id: toolFlow
                anchors.centerIn: parent
                width: root.expandedWidth - 4
                spacing: 2

                Repeater {
                    model: root.toolItems

                    Item {
                        required property var modelData
                        width: modelData.type === "bpm" ? root.btnSize * 2.2 : root.btnSize
                        height: root.btnSize

                        // BPM field
                        TextField {
                            id: bpmField
                            anchors.fill: parent
                            visible: modelData.type === "bpm"
                            enabled: root.engine && root.engine.trackData && root.engine.trackData.isBpmAnalyzed
                            color: "#eee"
                            placeholderText: "BPM"
                            placeholderTextColor: "#555"
                            background: Rectangle { color: "#111"; border.color: "#333"; radius: 2 }
                            font.pixelSize: Math.max(7, root.btnSize * 0.34)
                            font.family: "monospace"
                            horizontalAlignment: TextInput.AlignHCenter
                            validator: DoubleValidator { bottom: 20; top: 300; decimals: 1 }
                            text: {
                                if (!root.engine || !root.engine.trackData) return ""
                                var bpm = root.engine.trackData.bpm
                                return bpm > 0 ? bpm.toFixed(1) : ""
                            }
                            onEditingFinished: {
                                if (!root.engine) return
                                var v = parseFloat(text)
                                if (!isNaN(v) && v > 0) root.engine.setManualBpm(v)
                            }
                        }

                        // Button cell
                        Rectangle {
                            anchors.fill: parent
                            visible: modelData.type !== "bpm"
                            radius: 2
                            color: {
                                if (modelData.action === "edit" && root.editMode) return "#331e7bd4"
                                if (modelData.action === "lock" && lockBox.checked) return "#331e7bd4"
                                return cellHover.containsMouse ? "#252525" : "#161616"
                            }
                            border.color: {
                                if (modelData.action === "edit" && root.editMode) return "#1e7bd4"
                                if (modelData.action === "lock" && lockBox.checked) return "#1e7bd4"
                                if (modelData.type === "downbeat") return "#55e60000"
                                return "#2a2a2a"
                            }

                            Text {
                                anchors.centerIn: parent
                                visible: modelData.type !== "downbeat"
                                text: modelData.action === "lock"
                                      ? (lockBox.checked ? "🔒" : "🔓")
                                      : modelData.label
                                color: "#ccc"
                                font.pixelSize: Math.max(7, root.btnSize * 0.36)
                                font.bold: true
                                font.family: modelData.label.length <= 3 ? "monospace" : undefined
                            }

                            Text {
                                anchors.centerIn: parent
                                visible: modelData.type === "downbeat"
                                text: "I"
                                color: "#e60000"
                                font.pixelSize: Math.max(14, root.btnSize * 0.72)
                                font.bold: true
                            }

                            MouseArea {
                                id: cellHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.runAction(modelData.action)
                            }
                            ToolTip.visible: cellHover.containsMouse && modelData.tip
                            ToolTip.text: modelData.tip
                            ToolTip.delay: 350
                        }
                    }
                }
            }
        }
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
            height: root.slipRendererReady ? parent.height * 0.5 : parent.height
            clip: true
            z: root.slipRendererReady ? 0 : 1

            ScrollingWaveformItem {
                id: waveItem
                width: parent.width
                height: deckRect.height
                engine: root.engine
                pixelsPerPoint: root.waveformZoom
                backgroundColor: root.backgroundColor
                renderStyle: (typeof settingsManager !== "undefined" && settingsManager)
                             ? settingsManager.waveformRenderStyle : 0
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
            z: 0

            sourceComponent: Item {
                readonly property bool contentReady: slipWaveItem.contentReady

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
                    renderStyle: (typeof settingsManager !== "undefined" && settingsManager)
                                 ? settingsManager.waveformRenderStyle : 0
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

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.5
            visible: root.slipRendererReady
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
            objectName: "beatgridEditorPanel"
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
