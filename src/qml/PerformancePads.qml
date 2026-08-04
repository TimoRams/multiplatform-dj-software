import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

Item {
    id: root

    property var engine: null
    property string deckId: "deckA"
    property string accentColor: "#ff9900"
    property int activeTab: 0
    // Standalone/AIO surface: hot-cue mode and its pads are the only controls
    // retained on the primary display.
    property bool hotCueOnly: false
    property bool compact: false

    property double hotCueHoldPressedIndex: -1
    property double hotCueHoldCuePosition: 0.0
    property bool hotCueHoldWasPlaying: false
    property bool hotCueHoldHadCue: false
    property bool hotCueHoldReturnOnRelease: false

    readonly property var tabs: hotCueOnly ? ["HOT CUE"] : ["HOT CUE", "PAD FX", "BEATJUMP", "SAMPLER"]
    readonly property real tabBarHeight: 25
    readonly property real padsContentHeight: Math.max(0, (root.height - (hotCueOnly ? 0 : tabBarHeight + 1)) * (2 / 3))
    readonly property var beatJumpPads: [-16, -8, -4, -2, 2, 4, 8, 16]

    property int colorTargetIndex: -1

    property int padFxActiveToggle: -1
    property int padFxMomentaryHeld: -1

    function syncMidiPadMode() {
        if (root.hotCueOnly || typeof midiManager === "undefined" || !midiManager) {
            if (root.hotCueOnly) root.activeTab = 0
            return
        }
        if (root.deckId !== "deckA" && root.deckId !== "deckB")
            return
        var nextMode = root.deckId === "deckB"
            ? midiManager.deckBPadMode
            : midiManager.deckAPadMode
        if (nextMode >= 0 && nextMode < root.tabs.length && root.activeTab !== nextMode)
            root.activeTab = nextMode
    }

    Component.onCompleted: syncMidiPadMode()

    Connections {
        target: (typeof midiManager !== "undefined" && midiManager) ? midiManager : null
        function onDeckAPadModeChanged() {
            if (root.deckId === "deckA") root.syncMidiPadMode()
        }
        function onDeckBPadModeChanged() {
            if (root.deckId === "deckB") root.syncMidiPadMode()
        }
    }

    readonly property var padFxDefs: [
        { name: "ECHO",    effect: "Echo",    amount: 1.0, baseColor: "#0d2244", activeColor: "#1a4488" },
        { name: "FLANGE",  effect: "Flanger", amount: 1.0, baseColor: "#0d2030", activeColor: "#1a5080" },
        { name: "REVERB",  effect: "Reverb",  amount: 0.6, baseColor: "#0d3030", activeColor: "#1a6666" },
        { name: "REPEAT",  effect: "Roll",    amount: 1.0, baseColor: "#221440", activeColor: "#4428a0" },
        { name: "ECHO OUT",effect: "EchoOut",    baseColor: "#1a2820", activeColor: "#2a6640" },
        { name: "BACKSPIN",effect: "Backspin",   baseColor: "#2a1a30", activeColor: "#6a3080" },
        { name: "BRAKE",   effect: "VinylBrake", baseColor: "#2a1a08", activeColor: "#886020" },
        { name: "ROLLOUT", effect: "RollOut",    baseColor: "#1e1440", activeColor: "#4428a0" },
    ]

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

    onActiveTabChanged: padFxClearAll()
    onHotCueOnlyChanged: {
        if (hotCueOnly)
            activeTab = 0
    }

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
        root.engine.clearPadFx()
    }

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

    function savedLoopAt(index) {
        if (!root.engine || !root.engine.savedLoops || index < 0 || index >= root.engine.savedLoops.length)
            return null
        return root.engine.savedLoops[index]
    }

    // "loop" | "hot" | "empty"
    function padKind(index) {
        var loop = root.savedLoopAt(index)
        if (loop && loop.set) return "loop"
        var cue = root.hotCueAt(index)
        if (cue && cue.set) return "hot"
        return "empty"
    }

    function padLabel(index) {
        var kind = root.padKind(index)
        if (kind === "loop") return root.formatLoopLabel(root.savedLoopAt(index))
        if (kind === "hot")  return root.formatCueTime(root.hotCueAt(index).positionSec)
        return "+"
    }

    function padColor(index) {
        var kind = root.padKind(index)
        if (kind === "loop") return root.savedLoopAt(index).color
        if (kind === "hot")  return root.hotCueAt(index).color
        return "#454545"
    }

    function formatLoopLabel(loop) {
        if (!loop || !loop.set) return "+"
        var beats = loop.lengthBeats
        if (beats >= 1.0) return beats.toFixed(0) + "B"
        return root.formatCueTime(loop.inSec)
    }

    function formatCueTime(seconds) {
        var sec = Math.max(0, seconds || 0)
        var mins = Math.floor(sec / 60)
        var s = Math.floor(sec % 60)
        return mins.toString().padStart(2, "0") + ":" + s.toString().padStart(2, "0")
    }

    function consumeHotCueHoldPlayLatch() {
        if (root.hotCueHoldPressedIndex < 0 || !root.hotCueHoldReturnOnRelease)
            return false
        root.hotCueHoldReturnOnRelease = false
        return true
    }

    function clearHotCueHoldState() {
        root.hotCueHoldPressedIndex = -1
        root.hotCueHoldCuePosition = 0.0
        root.hotCueHoldWasPlaying = false
        root.hotCueHoldHadCue = false
        root.hotCueHoldReturnOnRelease = false
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
        var preRoll = root.engine.preRollSeconds || 0.0
        nextPos = Math.max(-preRoll, Math.min(duration, nextPos))
        root.engine.setPosition(nextPos / duration)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            visible: true
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            Layout.minimumHeight: 26
            Layout.maximumHeight: 26
            spacing: 2

            Repeater {
                model: root.tabs

                Rectangle {
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 0
                    color: root.activeTab === index ? "#666666" : "#454545"
                    border.width: 1
                    border.color: root.activeTab === index ? Qt.lighter(root.accentColor, 1.12) : "#737373"

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left; anchors.right: parent.right
                        height: root.activeTab === index ? 3 : 0
                        visible: root.activeTab === index
                        color: root.accentColor
                    }

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: root.activeTab === index ? "#ffffff" : "#dddddd"
                        font.pixelSize: 9
                        font.bold: root.activeTab === index
                        font.letterSpacing: 0.6
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.activeTab = index
                            if ((root.deckId === "deckA" || root.deckId === "deckB")
                                    && typeof midiManager !== "undefined" && midiManager)
                                midiManager.selectPerformancePadMode(root.deckId, index)
                        }
                    }
                }
            }
        }

        Rectangle {
            visible: true
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            Layout.minimumHeight: 1
            Layout.maximumHeight: 1
            color: UiTheme.divider
        }

        RowLayout {
            id: contentRow
            Layout.fillWidth: true
            Layout.preferredHeight: root.padsContentHeight
            Layout.maximumHeight: root.padsContentHeight
            spacing: 4

            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: 4
                rows: 2
                columnSpacing: 4
                rowSpacing: 4

                Repeater {
                    model: 8

                    Rectangle {
                        id: padRect
                        Layout.fillWidth:  true
                        Layout.fillHeight: true
                        Layout.row:    Math.floor(index / 4)
                        Layout.column: index % 4

                        readonly property bool isHotCueTab:   root.activeTab === 0
                        readonly property bool isPadFxTab:    root.activeTab === 1
                        readonly property bool isBeatJumpTab: root.activeTab === 2
                        readonly property bool isSamplerTab:  root.activeTab === 3
                        readonly property bool isCuePlaybackTab: isHotCueTab || isSamplerTab
                        readonly property string kind:        root.padKind(index)
                        readonly property bool padSet:        isCuePlaybackTab && kind !== "empty"

                        readonly property bool isPadFxMomentary: isPadFxTab && index < 4
                        readonly property bool isPadFxToggle:    isPadFxTab && index >= 4
                        readonly property bool padFxToggleOn:    isPadFxToggle && root.padFxActiveToggle === index
                        readonly property bool padFxLit:
                            isPadFxMomentary
                                ? (root.padFxMomentaryHeld === index)
                                : padFxToggleOn

                        readonly property color activeColor: {
                            if (padSet)        return root.padColor(index)
                            if (isBeatJumpTab) return "#2a2208"
                            if (isSamplerTab)  return padSet ? root.padColor(index) : "#202a24"
                            if (isPadFxTab) {
                                var def = root.padFxDefs[index]
                                return padFxLit ? def.activeColor : def.baseColor
                            }
                            return "#454545"
                        }

                        radius: 0
                        color: padMouse.pressed
                               ? Qt.lighter(activeColor, 1.12)
                               : padMouse.containsMouse
                                 ? Qt.lighter(activeColor, 1.06)
                                 : activeColor

                        Text {
                            anchors.top: parent.top; anchors.left: parent.left
                            anchors.margins: 4
                            text: (index + 1).toString()
                            color: padSet || padFxLit ? "#eeeeee" : "#c8c8c8"
                            font.pixelSize: 7
                            font.bold: true
                            font.family: "monospace"
                        }

                        // Loop-cue badge
                        Text {
                            visible: isHotCueTab && kind === "loop"
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 3
                            text: "L"
                            color: "#a8ffd0"
                            font.pixelSize: 6
                            font.bold: true
                        }

                        Text {
                            anchors.centerIn: parent
                            text: {
                                if (isHotCueTab)   return root.padLabel(index)
                                if (isSamplerTab)  return padSet ? root.padLabel(index) : "EMPTY"
                                if (isBeatJumpTab) return root.beatJumpLabel(index)
                                if (isPadFxTab)    return root.padFxDefs[index].name
                                return "—"
                            }
                            color: {
                                if (isCuePlaybackTab && padSet) return "#ffffff"
                                if (isHotCueTab)           return "#f0f0f0"
                                if (isSamplerTab)          return "#8fbfa4"
                                if (isBeatJumpTab)         return "#ffd38a"
                                if (isPadFxTab)            return padFxLit ? "#ffffff" : (index < 4 ? "#606060" : "#505050")
                                return "#333"
                            }
                            font.pixelSize: (isHotCueTab && !padSet) ? 11 : 8
                            font.bold: (isCuePlaybackTab && padSet) || isPadFxTab
                            font.letterSpacing: isPadFxTab ? 0.4 : 0.0
                            font.family: "monospace"
                        }

                        Text {
                            visible: isPadFxMomentary && !padFxLit && padMouse.containsMouse
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottomMargin: 3
                            text: "HOLD"
                            color: "#555"
                            font.pixelSize: 5
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

                                if (mouse.button === Qt.LeftButton && isCuePlaybackTab) {
                                    root.hotCueHoldPressedIndex = index
                                    root.hotCueHoldWasPlaying = root.engine.isPlaying
                                    root.hotCueHoldHadCue = padSet
                                    if (padSet) {
                                        if (kind === "loop")
                                            root.hotCueHoldCuePosition = root.savedLoopAt(index).inSec
                                        else
                                            root.hotCueHoldCuePosition = root.hotCueAt(index).positionSec
                                        root.hotCueHoldReturnOnRelease = !root.hotCueHoldWasPlaying
                                        root.engine.triggerCuePad(index)
                                        if (!root.hotCueHoldWasPlaying) root.engine.play()
                                    } else if (isHotCueTab) {
                                        root.hotCueHoldReturnOnRelease = false
                                        root.engine.storeCuePad(index)
                                    } else {
                                        root.clearHotCueHoldState()
                                    }
                                    return
                                }

                                if (mouse.button === Qt.LeftButton && isPadFxMomentary) {
                                    root.padFxMomentaryHeld = index
                                    root.padFxApply(index)
                                }
                            }

                            onReleased: (mouse) => {
                                if (!root.engine) return

                                if (isCuePlaybackTab) {
                                    if (root.hotCueHoldPressedIndex !== index) return
                                    if (root.hotCueHoldReturnOnRelease) {
                                        root.engine.pause()
                                        var trackLen = root.engine.getDuration()
                                        if (trackLen && trackLen > 0) {
                                            var normalizedPos = root.hotCueHoldCuePosition / trackLen
                                            root.engine.setPosition(Math.max(0, Math.min(1.0, normalizedPos)))
                                        }
                                    }
                                    root.clearHotCueHoldState()
                                    return
                                }

                                if (root.padFxMomentaryHeld === index) {
                                    root.padFxMomentaryHeld = -1
                                    root.padFxRelease(index)
                                }
                            }

                            onCanceled: {
                                if (root.hotCueHoldPressedIndex === index) {
                                    if (root.hotCueHoldReturnOnRelease && root.engine) {
                                        root.engine.pause()
                                        var trackLen = root.engine.getDuration()
                                        if (trackLen && trackLen > 0) {
                                            var normalizedPos = root.hotCueHoldCuePosition / trackLen
                                            root.engine.setPosition(Math.max(0, Math.min(1.0, normalizedPos)))
                                        }
                                    }
                                    root.clearHotCueHoldState()
                                }
                                if (root.padFxMomentaryHeld === index) {
                                    root.padFxMomentaryHeld = -1
                                    if (root.engine) root.padFxRelease(index)
                                }
                            }

                            onClicked: (mouse) => {
                                if (!root.engine) return

                                if (isBeatJumpTab) {
                                    if (mouse.button === Qt.LeftButton) root.doBeatJump(root.beatJumpPads[index])
                                    return
                                }

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

                                if (!isHotCueTab) return

                                if (mouse.button === Qt.MiddleButton) {
                                    root.engine.clearCuePad(index)
                                    return
                                }

                                if (mouse.button === Qt.RightButton) {
                                    if (kind === "loop") {
                                        root.engine.clearCuePad(index)
                                        return
                                    }
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
                // A hidden pad surface (for example in the development window)
                // must not keep a per-frame QML animation alive.
                animationEnabled: root.visible
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth:  Math.round(Math.min(contentRow.height, Math.max(root.compact ? 38 : 72, root.width * 0.14)))
                Layout.minimumWidth:    Layout.preferredWidth
                Layout.preferredHeight: Layout.preferredWidth
                Layout.maximumWidth:    Layout.preferredWidth
                Layout.maximumHeight:   Layout.preferredHeight
            }
        }

        Item { Layout.fillWidth: true; Layout.fillHeight: true }
    }

    Popup {
        id: colorPopup
        width: 132; height: 132
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 6

        background: Rectangle {
            color: UiTheme.panelRaised; border.color: UiTheme.separator; border.width: 1; radius: 0
        }

        GridLayout {
            anchors.fill: parent
            columns: 4; rowSpacing: 4; columnSpacing: 4

            Repeater {
                model: root.palette16
                Rectangle {
                    required property var modelData
                    Layout.preferredWidth: 26; Layout.preferredHeight: 26
                    radius: 0; color: modelData

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
