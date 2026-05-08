import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Item {
    id: root

    property var engine: null
    property string accentColor: "#ff9900"
    property int activeTab: 0

    property double hotCueHoldPressedIndex: -1
    property double hotCueHoldCuePosition: 0.0
    property bool hotCueHoldWasPlaying: false

    readonly property var tabs: ["HOT CUE", "PAD FX", "BEATJUMP", "SAMPLER"]
    readonly property var beatJumpPads: [-16, -8, -4, -2, 2, 4, 8, 16]

    property int colorTargetIndex: -1

    // ── PAD FX state ─────────────────────────────────────────────────────────
    // Pad 0-3: momentary (held); Pad 4-7: toggle (latch on/off)
    // Index of the currently latched toggle pad (-1 = none)
    property int padFxActiveToggle: -1
    // Index of the momentary pad currently held (-1 = none)
    property int padFxMomentaryHeld: -1

    readonly property var padFxDefs: [
        // Top row — momentary while held (0-3)
        { name: "ECHO",    effect: "Echo",    amount: 1.0, baseColor: "#0d2244", activeColor: "#1a4488" },
        { name: "FLANGE",  effect: "Flanger", amount: 1.0, baseColor: "#0d2030", activeColor: "#1a5080" },
        { name: "REVERB",  effect: "Reverb",  amount: 0.6, baseColor: "#0d3030", activeColor: "#1a6666" },
        { name: "REPEAT",  effect: "Roll",    amount: 1.0, baseColor: "#221440", activeColor: "#4428a0" },
        // Bottom row — toggle latch (4-7)
        { name: "ECHO OUT",effect: "EchoOut",    baseColor: "#1a2820", activeColor: "#2a6640" },
        { name: "BACKSPIN",effect: "Backspin",   baseColor: "#2a1a30", activeColor: "#6a3080" },
        { name: "BRAKE",   effect: "VinylBrake", baseColor: "#2a1a08", activeColor: "#886020" },
        { name: "ROLLOUT", effect: "RollOut",    baseColor: "#1e1440", activeColor: "#4428a0" },
    ]

    // Clear all active pad FX and reset visual state — call on tab switch or lost focus.
    function padFxClearAll() {
        if (padFxMomentaryHeld >= 0) {
            padFxRelease(padFxMomentaryHeld)
            padFxMomentaryHeld = -1
        }
        if (padFxActiveToggle >= 0) {
            padFxDeactivate(padFxActiveToggle)
            padFxActiveToggle = -1
        }
    }

    onActiveTabChanged: {
        // Always clean up any held/toggled FX when leaving the PAD FX tab.
        padFxClearAll()
    }

    // ── PAD FX helpers ────────────────────────────────────────────────────────
    function padFxApply(defIndex) {
        if (!root.engine) return
        var def = root.padFxDefs[defIndex]
        if (def.effect === "VinylBrake")       root.engine.startVinylBrake()
        else if (def.effect === "EchoOut")     root.engine.startEchoOut()
        else if (def.effect === "Backspin")    root.engine.startBackspin()
        else if (def.effect === "RollOut")     root.engine.startRollOut()
        else                                   root.engine.setPadFx(def.effect, def.amount !== undefined ? def.amount : 1.0)
    }

    function padFxDeactivate(defIndex) {
        if (!root.engine) return
        var def = root.padFxDefs[defIndex]
        if (def.effect === "VinylBrake")       root.engine.stopVinylBrake()
        else if (def.effect === "EchoOut")     root.engine.stopEchoOut()
        else if (def.effect === "Backspin")    root.engine.stopBackspin()
        else if (def.effect === "RollOut")     root.engine.stopRollOut()
        else                                   root.engine.clearPadFx()
    }

    function padFxRelease(defIndex) {
        if (!root.engine) return
        // Momentary pad (0-3) released — all bottom-row toggles are MixerDspSource stop effects,
        // none use FxProcessor, so just clear the pad FX slot
        root.engine.clearPadFx()
    }

    // ─────────────────────────────────────────────────────────────────────────

    readonly property var palette16: [
        "#e04040", "#e08030", "#e0d030", "#30b050",
        "#30a0d0", "#6060e0", "#c040c0", "#e06080",
        "#ff4d4d", "#ff9f43", "#f6e05e", "#48bb78",
        "#38b2ac", "#4299e1", "#9f7aea", "#ed64a6"
    ]

    function hotCueAt(index) {
        if (!root.engine || !root.engine.hotCues || index < 0 || index >= root.engine.hotCues.length)
            return null
        return root.engine.hotCues[index]
    }

    function formatCueTime(seconds) {
        var sec = Math.max(0, seconds || 0)
        var mins = Math.floor(sec / 60)
        var s = Math.floor(sec % 60)
        return mins.toString().padStart(2, "0") + ":" + s.toString().padStart(2, "0")
    }

    function beatJumpLabel(index) {
        var b = root.beatJumpPads[index]
        return (b > 0 ? "+" : "") + b + "B"
    }

    function beatDurationSeconds() {
        if (!root.engine) return 0.5
        var bpm = root.engine.currentBpm
        if (!bpm || bpm <= 0.0) return 0.5
        return 60.0 / bpm
    }

    function doBeatJump(beats) {
        if (!root.engine) return
        var duration = root.engine.getDuration()
        if (!duration || duration <= 0.0) return
        var current = root.engine.getPlayheadPositionAtomic()
        if (current === undefined || isNaN(current))
            current = root.engine.progress * duration
        var nextPos = current + beats * root.beatDurationSeconds()
        nextPos = Math.max(0.0, Math.min(duration, nextPos))
        root.engine.setPosition(nextPos / duration)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Mode tabs ─────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            spacing: 1

            Repeater {
                model: root.tabs

                Rectangle {
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 2
                    color: root.activeTab === index ? "#252525" : "#161616"

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left; anchors.right: parent.right
                        height: 2; radius: 1
                        color: root.accentColor
                        visible: root.activeTab === index
                    }

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: root.activeTab === index ? "#f0f0f0" : "#666"
                        font.pixelSize: window.spViewport(8)
                        font.bold: root.activeTab === index
                        font.letterSpacing: 0.4
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.activeTab = index
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#0e0e0e" }

        // ── Pads + Turntable ──────────────────────────────────────────────
        RowLayout {
            id: contentRow
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 4

            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: 4
                rows: 2
                columnSpacing: 3
                rowSpacing: 3

                Repeater {
                    model: 8

                    Rectangle {
                        id: padRect
                        Layout.fillWidth:  true
                        Layout.fillHeight: true
                        Layout.row:    Math.floor(index / 4)
                        Layout.column: index % 4

                        readonly property var cue: root.hotCueAt(index)
                        readonly property bool isHotCueTab:      root.activeTab === 0
                        readonly property bool isPadFxTab:       root.activeTab === 1
                        readonly property bool isBeatJumpTab:    root.activeTab === 2
                        readonly property bool isPlaceholderTab: root.activeTab === 3
                        readonly property bool cueSet: isHotCueTab && cue && cue.set

                        // ── PAD FX derived state ──────────────────────────
                        readonly property bool isPadFxMomentary: isPadFxTab && index < 4
                        readonly property bool isPadFxToggle:    isPadFxTab && index >= 4
                        readonly property bool padFxToggleOn:    isPadFxToggle && root.padFxActiveToggle === index
                        // True when this pad's effect is visually "lit"
                        readonly property bool padFxLit:
                            isPadFxMomentary
                                ? (root.padFxMomentaryHeld === index)
                                : padFxToggleOn

                        // ── Effective background color ────────────────────
                        readonly property color activeColor: {
                            if (cueSet)          return cue.color
                            if (isBeatJumpTab)   return "#3a2e14"
                            if (isPlaceholderTab)return "#1a1a1a"
                            if (isPadFxTab) {
                                var def = root.padFxDefs[index]
                                return padFxLit ? def.activeColor : def.baseColor
                            }
                            return "#1e1e1e"
                        }

                        radius: 3
                        color: padMouse.pressed
                               ? Qt.lighter(activeColor, 1.5)
                               : padMouse.containsMouse
                                 ? Qt.lighter(activeColor, 1.2)
                                 : activeColor

                        border.color: {
                            if (cueSet)   return Qt.lighter(cue.color, 1.7)
                            if (padFxLit) return Qt.lighter(root.padFxDefs[index].activeColor, 1.8)
                            return "#282828"
                        }
                        border.width: 1

                        // Pad number (top-left)
                        Text {
                            anchors.top: parent.top; anchors.left: parent.left
                            anchors.margins: 4
                            text: (index + 1).toString()
                            color: {
                                if (cueSet)     return "#cccccc"
                                if (padFxLit)   return "#aaaaaa"
                                return "#3a3a3a"
                            }
                            font.pixelSize: window.spViewport(7)
                            font.bold: true
                            font.family: "monospace"
                        }

                        // Main label
                        Text {
                            anchors.centerIn: parent
                            text: {
                                if (isHotCueTab)    return cueSet ? root.formatCueTime(cue.positionSec) : "+"
                                if (isBeatJumpTab)  return root.beatJumpLabel(index)
                                if (isPadFxTab)     return root.padFxDefs[index].name
                                return "—"
                            }
                            color: {
                                if (isHotCueTab)    return cueSet ? "#ffffff" : "#444"
                                if (isBeatJumpTab)  return "#ffd38a"
                                if (isPadFxTab) {
                                    if (padFxLit)   return "#ffffff"
                                    // Momentary row slightly brighter hint
                                    return index < 4 ? "#606060" : "#505050"
                                }
                                return "#333"
                            }
                            font.pixelSize: (isHotCueTab && !cueSet) ? window.spViewport(16) : window.spViewport(8)
                            font.bold: isHotCueTab ? cueSet : true
                            font.letterSpacing: isPadFxTab ? 0.4 : 0.0
                            font.family: "monospace"
                        }

                        // "HOLD" hint for momentary pads
                        Text {
                            visible: isPadFxMomentary && !padFxLit && padMouse.containsMouse
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottomMargin: 3
                            text: "HOLD"
                            color: "#555"
                            font.pixelSize: window.spViewport(5)
                            font.family: "monospace"
                        }

                        MouseArea {
                            id: padMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton

                            onPressed: (mouse) => {
                                if (!root.engine) return

                                // ── Hot cue (momentary trigger) ───────────────
                                if (mouse.button === Qt.LeftButton && isHotCueTab) {
                                    root.hotCueHoldPressedIndex = index
                                    root.hotCueHoldCuePosition = cueSet ? cue.positionSec : 0.0
                                    root.hotCueHoldWasPlaying = root.engine.isPlaying
                                    if (cueSet) {
                                        root.engine.triggerHotCue(index)
                                        if (!root.hotCueHoldWasPlaying) root.engine.play()
                                    } else {
                                        root.engine.storeHotCue(index)
                                    }
                                    return
                                }

                                // ── PAD FX momentary (top row, held) ──────────
                                if (mouse.button === Qt.LeftButton && isPadFxMomentary) {
                                    root.padFxMomentaryHeld = index
                                    root.padFxApply(index)
                                }
                            }

                            onReleased: (mouse) => {
                                if (!root.engine) return

                                // ── Hot cue release ───────────────────────────
                                if (isHotCueTab) {
                                    if (root.hotCueHoldPressedIndex !== index) return
                                    if (!root.hotCueHoldWasPlaying) {
                                        root.engine.pause()
                                        var trackLen = root.engine.getDuration()
                                        if (trackLen && trackLen > 0) {
                                            var normalizedPos = root.hotCueHoldCuePosition / trackLen
                                            root.engine.setPosition(Math.max(0, Math.min(1.0, normalizedPos)))
                                        }
                                    }
                                    root.hotCueHoldPressedIndex = -1
                                    root.hotCueHoldCuePosition = 0.0
                                    return
                                }

                                // ── PAD FX momentary release ──────────────────
                                if (root.padFxMomentaryHeld === index) {
                                    root.padFxMomentaryHeld = -1
                                    root.padFxRelease(index)
                                }
                            }

                            onCanceled: {
                                // Pointer cancelled (window focus lost, gesture, etc.) — always clean up.
                                if (root.hotCueHoldPressedIndex === index) {
                                    root.hotCueHoldPressedIndex = -1
                                    root.hotCueHoldCuePosition  = 0.0
                                }
                                if (root.padFxMomentaryHeld === index) {
                                    root.padFxMomentaryHeld = -1
                                    if (root.engine) root.padFxRelease(index)
                                }
                            }

                            onClicked: (mouse) => {
                                if (!root.engine) return

                                // ── Beat jump ─────────────────────────────────
                                if (isBeatJumpTab) {
                                    if (mouse.button === Qt.LeftButton) root.doBeatJump(root.beatJumpPads[index])
                                    return
                                }

                                // ── Placeholder (STEMS etc.) ──────────────────
                                if (isPlaceholderTab) return

                                // ── PAD FX toggle (bottom row) ────────────────
                                if (isPadFxToggle && mouse.button === Qt.LeftButton) {
                                    if (root.padFxActiveToggle === index) {
                                        root.padFxActiveToggle = -1
                                        root.padFxDeactivate(index)
                                    } else {
                                        if (root.padFxActiveToggle >= 0)
                                            root.padFxDeactivate(root.padFxActiveToggle)
                                        root.padFxActiveToggle = index
                                        root.padFxApply(index)
                                    }
                                    return
                                }

                                // ── Hot cue right-click / middle-click ────────
                                if (!isHotCueTab) return
                                if (mouse.button === Qt.MiddleButton) {
                                    root.engine.clearHotCue(index); return
                                }
                                if (mouse.button === Qt.RightButton) {
                                    root.colorTargetIndex = index
                                    var p = padMouse.mapToItem(root, mouse.x, mouse.y)
                                    colorPopup.x = Math.max(0, Math.min(root.width - colorPopup.width, p.x - colorPopup.width / 2))
                                    colorPopup.y = Math.max(0, Math.min(root.height - colorPopup.height, p.y - colorPopup.height / 2))
                                    colorPopup.open()
                                }
                            }
                        }

                        Behavior on color { ColorAnimation { duration: 80 } }
                    }
                }
            }

            TurntableIndicator {
                engine: root.engine
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth:  Math.min(contentRow.height, Math.max(72, root.width * 0.14))
                Layout.preferredHeight: Layout.preferredWidth
                Layout.maximumWidth:    Layout.preferredWidth
                Layout.maximumHeight:   Layout.preferredHeight
            }
        }
    }

    Popup {
        id: colorPopup
        width: 132; height: 132
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 6

        background: Rectangle {
            color: "#242424"; border.color: "#111"; border.width: 1; radius: 2
        }

        GridLayout {
            anchors.fill: parent
            columns: 4; rowSpacing: 4; columnSpacing: 4

            Repeater {
                model: root.palette16
                Rectangle {
                    required property var modelData
                    Layout.preferredWidth: 26; Layout.preferredHeight: 26
                    radius: 2; color: modelData
                    border.color: Qt.lighter(modelData, 1.3); border.width: 1

                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (root.colorTargetIndex >= 0 && root.engine) {
                                var cue = root.hotCueAt(root.colorTargetIndex)
                                if (!cue || !cue.set) root.engine.storeHotCue(root.colorTargetIndex)
                                root.engine.setHotCueColor(root.colorTargetIndex, modelData)
                            }
                            colorPopup.close()
                        }
                    }
                }
            }
        }
    }
}
