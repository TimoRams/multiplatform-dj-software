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
    property bool hotCueOnly: false
    property bool compact: false

    property double hotCueHoldPressedIndex: -1
    property double hotCueHoldCuePosition: 0.0
    property bool hotCueHoldWasPlaying: false
    property bool hotCueHoldHadCue: false
    property bool hotCueHoldReturnOnRelease: false

    readonly property var tabs: hotCueOnly ? ["HOT CUE"] : ["HOT CUE", "PAD FX", "BEATJUMP", "SAMPLER"]
    readonly property real tabBarHeight: 28
    readonly property real padsContentHeight: Math.max(0, root.height - tabBarHeight - 1)
    readonly property var beatJumpPads: [-16, -8, -4, -2, 2, 4, 8, 16]
    readonly property bool usesSharedPadRouter:
        (deckId === "deckA" || deckId === "deckB")
        && typeof midiManager !== "undefined" && midiManager
    readonly property int midiPadMode: !usesSharedPadRouter ? activeTab
        : (deckId === "deckB" ? midiManager.deckBPadMode : midiManager.deckAPadMode)
    readonly property bool keyShiftMode: usesSharedPadRouter && midiPadMode === 4
    readonly property int keyShiftRange: !usesSharedPadRouter ? 1
        : (deckId === "deckB" ? midiManager.deckBKeyShiftRange : midiManager.deckAKeyShiftRange)
    readonly property var keyShiftValues: [
        [-3, -2, -1,  0, -7, -6, -5, -4],
        [ 0,  1,  2,  3, -4, -3, -2, -1],
        [ 4,  5,  6,  7,  0,  1,  2,  3]
    ]

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
        // Key Shift is the shifted SAMPLER hardware bank, represented by the
        // fourth tab even though it has its own semantic mode value.
        var nextTab = nextMode === 4 ? 3 : nextMode
        if (nextTab >= 0 && nextTab < root.tabs.length && root.activeTab !== nextTab)
            root.activeTab = nextTab
    }

    function syncSharedPadState() {
        if (!root.usesSharedPadRouter)
            return
        root.padFxMomentaryHeld = midiManager.performancePadFxMomentary(root.deckId)
        root.padFxActiveToggle = midiManager.performancePadFxToggle(root.deckId)
    }

    function selectPadPage(index) {
        if (index < 0 || index >= root.tabs.length)
            return
        root.activeTab = index
        if (root.usesSharedPadRouter)
            midiManager.selectPerformancePadMode(root.deckId, index)
    }

    Component.onCompleted: {
        syncMidiPadMode()
        syncSharedPadState()
    }

    Connections {
        target: (typeof midiManager !== "undefined" && midiManager) ? midiManager : null
        function onDeckAPadModeChanged() {
            if (root.deckId === "deckA") root.syncMidiPadMode()
        }
        function onDeckBPadModeChanged() {
            if (root.deckId === "deckB") root.syncMidiPadMode()
        }
        function onPerformancePadStateChanged(changedDeckId) {
            if (changedDeckId === root.deckId) root.syncSharedPadState()
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

    onActiveTabChanged: {
        if (root.usesSharedPadRouter)
            root.syncSharedPadState()
        else
            root.padFxClearAll()
    }
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
        if (root.usesSharedPadRouter)
            return midiManager.consumePerformancePadPlayLatch(root.deckId)
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

    function keyShiftValue(index) {
        var range = Math.max(0, Math.min(2, root.keyShiftRange))
        return root.keyShiftValues[range][index]
    }

    function keyShiftLabel(index) {
        var semitones = root.keyShiftValue(index)
        return semitones > 0 ? "+" + semitones : semitones.toString()
    }

    function keyShiftPadSelected(index) {
        return root.isKeyShiftSelectionAvailable
            && Math.abs(root.engine.keySemitoneOffset - root.keyShiftValue(index)) < 0.01
    }

    readonly property bool isKeyShiftSelectionAvailable:
        root.keyShiftMode && root.engine && root.engine.keySemitoneOffset !== undefined

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

    function beginPadPress(index) {
        if (!root.engine || index < 0 || index >= 8)
            return

        if (root.usesSharedPadRouter) {
            midiManager.setPerformancePadPressed(root.deckId, index, true)
            root.syncSharedPadState()
            return
        }

        const isCuePlaybackPage = root.activeTab === 0 || root.activeTab === 3
        const kind = root.padKind(index)
        const padSet = isCuePlaybackPage && kind !== "empty"
        if (isCuePlaybackPage) {
            root.hotCueHoldPressedIndex = index
            root.hotCueHoldWasPlaying = root.engine.isPlaying
            root.hotCueHoldHadCue = padSet
            if (padSet) {
                root.hotCueHoldCuePosition = kind === "loop"
                    ? root.savedLoopAt(index).inSec
                    : root.hotCueAt(index).positionSec
                root.hotCueHoldReturnOnRelease = !root.hotCueHoldWasPlaying
                root.engine.triggerCuePad(index)
                if (!root.hotCueHoldWasPlaying) root.engine.play()
            } else if (root.activeTab === 0) {
                root.hotCueHoldReturnOnRelease = false
                root.engine.storeCuePad(index)
            } else {
                root.clearHotCueHoldState()
            }
            return
        }

        if (root.activeTab === 1 && index < 4) {
            root.padFxMomentaryHeld = index
            root.padFxApply(index)
            return
        }

        if (root.activeTab === 1) {
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

        if (root.activeTab === 2)
            root.doBeatJump(root.beatJumpPads[index])
    }

    function endPadPress(index) {
        if (index < 0 || index >= 8)
            return
        if (root.usesSharedPadRouter) {
            midiManager.setPerformancePadPressed(root.deckId, index, false)
            root.syncSharedPadState()
            return
        }

        if (root.activeTab === 0 || root.activeTab === 3) {
            if (root.hotCueHoldPressedIndex !== index)
                return
            if (root.hotCueHoldReturnOnRelease && root.engine) {
                root.engine.pause()
                const trackLen = root.engine.getDuration()
                if (trackLen && trackLen > 0) {
                    const normalizedPos = root.hotCueHoldCuePosition / trackLen
                    root.engine.setPosition(Math.max(0, Math.min(1.0, normalizedPos)))
                }
            }
            root.clearHotCueHoldState()
            return
        }

        if (root.padFxMomentaryHeld === index) {
            root.padFxMomentaryHeld = -1
            if (root.engine) root.padFxRelease(index)
        }
    }

    function clearPad(index) {
        if (!root.engine || index < 0 || index >= 8)
            return
        if (root.usesSharedPadRouter)
            midiManager.clearPerformancePad(root.deckId, index)
        else
            root.engine.clearCuePad(index)
    }

    function openPadEditor(index, item, x, y) {
        if (!root.engine || root.activeTab !== 0)
            return
        root.colorTargetIndex = index
        const p = item.mapToItem(root, x, y)
        colorPopup.x = Math.max(0, Math.min(root.width - colorPopup.width,
                                            p.x - colorPopup.width * 0.5))
        colorPopup.y = Math.max(0, Math.min(root.height - colorPopup.height,
                                            p.y - colorPopup.height * 0.5))
        colorPopup.open()
    }

    function applyPadColor(color) {
        if (root.colorTargetIndex >= 0 && root.engine) {
            const cue = root.hotCueAt(root.colorTargetIndex)
            if (!cue || !cue.set) root.engine.storeHotCue(root.colorTargetIndex)
            root.engine.setHotCueColor(root.colorTargetIndex, color)
        }
        colorPopup.close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            visible: true
            Layout.fillWidth: true
            Layout.preferredHeight: root.tabBarHeight
            Layout.minimumHeight: root.tabBarHeight
            Layout.maximumHeight: root.tabBarHeight
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
                        text: root.keyShiftMode && index === 3 ? "KEY SHIFT" : modelData
                        color: root.activeTab === index ? "#ffffff" : "#dddddd"
                        font.pixelSize: 9
                        font.bold: root.activeTab === index
                        font.letterSpacing: 0.6
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.selectPadPage(index)
                    }

                    TapHandler {
                        acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: root.selectPadPage(index)
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
                        readonly property bool isKeyShiftTab: root.activeTab === 3 && root.keyShiftMode
                        readonly property bool isSamplerTab:  root.activeTab === 3 && !isKeyShiftTab
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
                            if (isKeyShiftTab) return root.keyShiftPadSelected(index)
                                ? Qt.lighter(root.accentColor, 1.15)
                                : "#20242a"
                            if (isSamplerTab)  return padSet ? root.padColor(index) : "#202a24"
                            if (isPadFxTab) {
                                var def = root.padFxDefs[index]
                                return padFxLit ? def.activeColor : def.baseColor
                            }
                            return "#454545"
                        }

                        radius: 0
                        color: padMouse.pressed || padTouch.pressed
                               ? Qt.lighter(activeColor, 1.12)
                               : padMouse.containsMouse
                                 ? Qt.lighter(activeColor, 1.06)
                                 : activeColor
                        border.width: isKeyShiftTab && root.keyShiftPadSelected(index) ? 2 : 0
                        border.color: "#ffffff"

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
                                if (isKeyShiftTab) return root.keyShiftLabel(index)
                                if (isSamplerTab)  return padSet ? root.padLabel(index) : "EMPTY"
                                if (isBeatJumpTab) return root.beatJumpLabel(index)
                                if (isPadFxTab)    return root.padFxDefs[index].name
                                return "—"
                            }
                            color: {
                                if (isCuePlaybackTab && padSet) return "#ffffff"
                                if (isHotCueTab)           return "#f0f0f0"
                                if (isKeyShiftTab)         return root.keyShiftPadSelected(index) ? "#ffffff" : "#9dc7ff"
                                if (isSamplerTab)          return "#8fbfa4"
                                if (isBeatJumpTab)         return "#ffd38a"
                                if (isPadFxTab)            return padFxLit ? "#ffffff" : (index < 4 ? "#606060" : "#505050")
                                return "#333"
                            }
                            font.pixelSize: (isHotCueTab && !padSet) ? 11 : 8
                            font.bold: (isCuePlaybackTab && padSet) || isPadFxTab
                                || (isKeyShiftTab && root.keyShiftPadSelected(index))
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
                                if (mouse.button === Qt.LeftButton)
                                    root.beginPadPress(index)
                            }

                            onReleased: (mouse) => {
                                if (mouse.button === Qt.LeftButton)
                                    root.endPadPress(index)
                            }

                            onCanceled: root.endPadPress(index)

                            onClicked: (mouse) => {
                                if (!root.engine) return

                                if (!isHotCueTab) return

                                if (mouse.button === Qt.MiddleButton) {
                                    root.clearPad(index)
                                    return
                                }

                                if (mouse.button === Qt.RightButton) {
                                    if (kind === "loop") {
                                        root.clearPad(index)
                                        return
                                    }
                                    root.openPadEditor(index, padMouse, mouse.x, mouse.y)
                                }
                            }
                        }

                        TapHandler {
                            id: padTouch
                            acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                            gesturePolicy: TapHandler.WithinBounds
                            onPressedChanged: {
                                if (pressed)
                                    root.beginPadPress(index)
                                else
                                    root.endPadPress(index)
                            }
                            onLongPressed: root.openPadEditor(
                                index, padRect, point.position.x, point.position.y)
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
        width: 132; height: 164
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 6

        background: Rectangle {
            color: UiTheme.panelRaised; border.color: UiTheme.separator; border.width: 1; radius: 0
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 5

            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: 4
                rowSpacing: 4
                columnSpacing: 4

                Repeater {
                    model: root.palette16
                    Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 0
                        color: modelData

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.applyPadColor(modelData)
                        }
                        TapHandler {
                            acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                            onTapped: root.applyPadColor(modelData)
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                color: "#3b2020"
                border.color: "#a84a4a"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "CLEAR"
                    color: "#ffd4d4"
                    font.pixelSize: 9
                    font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.clearPad(root.colorTargetIndex)
                        colorPopup.close()
                    }
                }
                TapHandler {
                    acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                    onTapped: {
                        root.clearPad(root.colorTargetIndex)
                        colorPopup.close()
                    }
                }
            }
        }
    }

    component TurntableIndicator: Item {
        id: root

        property var   engine: null
        property color ringColor: "#3a3a3a"
        property real  ringThickness: 7
        property real  cutoutAngleDeg: 16
        property real  baseRpm: 33.0 + 1.0/3.0   // 33⅓ RPM = 200 °/s
        property real  scratchDeadzoneDeg: 0.05
        property bool  animationEnabled: true
        readonly property bool motionActive:
            root.animationEnabled && root.visible && root.engine !== null
            && (root.engine.isPlaying || root.engine.scratchVisualActive
                || root.dragActive)
        readonly property int motionIntervalMs:
            (typeof renderPressurePolicy !== "undefined" && renderPressurePolicy)
            ? renderPressurePolicy.interactiveWaveformUpdateIntervalMs : 16

        readonly property real degreesPerSecond: 200.0

        property bool dragActive: false
        property bool _scratchEngaged: false
        property real _accumDragAngle: 0.0
        property real _totalScratchedAngle: 0.0
        property real _pressPlayheadSec: 0.0
        property real _lastCursorAngle: 0.0
        property bool _lastAngleValid: true

        function updateRotation() {
            if (!root.engine) { ringRotator.rotation = 0; return }
            var playheadSec = root.engine.scratchVisualActive
                            ? root.engine.getPlayheadPositionAtomic()
                            : root.engine.getVisualPositionQml()
            var angle = (playheadSec * root.degreesPerSecond) % 360.0
            if (angle < 0) angle += 360.0
            ringRotator.rotation = angle
        }

        function angleForPoint(xPos, yPos) {
            var dx = xPos - root.width  * 0.5
            var dy = yPos - root.height * 0.5
            var a = Math.atan2(dy, dx) * 180.0 / Math.PI
            if (a < 0) a += 360.0
            return a
        }

        // atan2-based angle tracking is numerically unstable close to the
        // platter's center: a one-pixel touch/mouse jitter there can swing the
        // angle by dozens of degrees. That instability is exactly what produces
        // the "wrong direction at the end" feeling — a scratch naturally slows
        // down and the finger can drift toward the center right as it lifts off,
        // and touchscreens commonly report a bit of contact-point jitter during
        // liftoff. Below this radius we keep reporting the last stable angle
        // instead of trusting a new, unreliable one.
        readonly property real minRadiusPx: Math.max(10, Math.min(root.width, root.height) * 0.12)

        function radiusForPoint(xPos, yPos) {
            var dx = xPos - root.width  * 0.5
            var dy = yPos - root.height * 0.5
            return Math.sqrt(dx * dx + dy * dy)
        }

        function shortestAngleDelta(from, to) {
            var d = to - from
            if (d >  180) d -= 360
            if (d < -180) d += 360
            return d
        }

        implicitWidth: 120
        implicitHeight: 120

        Item {
            id: ringRotator
            x: 0; y: 0
            width: parent.width; height: parent.height
            transformOrigin: Item.Center

            Canvas {
                id: ringCanvas
                anchors.fill: parent
                antialiasing: true

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)

                    var cx  = width  * 0.5
                    var cy  = height * 0.5
                    var lw  = Math.max(2, root.ringThickness)
                    var rad = Math.min(width, height) * 0.5 - lw * 0.5 - 1
                    var gap = root.cutoutAngleDeg * Math.PI / 180.0
                    var s   = -Math.PI * 0.5 + gap * 0.5
                    var e   = s + (Math.PI * 2.0 - gap)

                    ctx.beginPath()
                    ctx.arc(cx, cy, rad, s, e, false)
                    ctx.lineWidth   = lw
                    ctx.lineCap     = "butt"
                    ctx.strokeStyle = root.ringColor
                    ctx.stroke()
                }
            }
        }

        onWidthChanged:  ringCanvas.requestPaint()
        onHeightChanged: ringCanvas.requestPaint()

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: root.dragActive ? Qt.ClosedHandCursor : Qt.OpenHandCursor

            function inPlatter(mouse) {
                var r  = root.width * 0.5
                var dx = mouse.x - r
                var dy = mouse.y - r
                return dx * dx + dy * dy <= r * r
            }

            onPressed: (mouse) => {
                if (!root.engine || !inPlatter(mouse)) { mouse.accepted = false; return }

                root.dragActive           = true
                root._scratchEngaged      = false
                root._accumDragAngle      = 0.0
                root._totalScratchedAngle = 0.0
                root._pressPlayheadSec    = root.engine.getVisualPositionQml()
                // A press right near the center has no reliable angle either; the
                // first accepted onPositionChanged sample (outside the deadzone)
                // will resync the reference angle without producing a spurious
                // delta, see the "_lastAngleValid" handling below.
                root._lastAngleValid      = root.radiusForPoint(mouse.x, mouse.y) >= root.minRadiusPx
                root._lastCursorAngle     = root.angleForPoint(mouse.x, mouse.y)
                root.engine.pauseForScrub(root._pressPlayheadSec)
            }

            onPositionChanged: (mouse) => {
                if (!root.dragActive || !root.engine) return

                // Ignore samples too close to the platter center — atan2 there is
                // noise-amplifying, and using it would risk injecting a spurious,
                // wrongly-signed angle delta into the scratch velocity (most
                // noticeable as a release "flick" in the wrong direction).
                if (root.radiusForPoint(mouse.x, mouse.y) < root.minRadiusPx) {
                    root._lastAngleValid = false
                    return
                }

                var cur = root.angleForPoint(mouse.x, mouse.y)
                if (!root._lastAngleValid) {
                    // Re-entering the reliable radius after a center dropout:
                    // resync the reference angle instead of computing a delta
                    // against a stale (possibly noisy) previous angle.
                    root._lastCursorAngle = cur
                    root._lastAngleValid = true
                    return
                }
                var delta = root.shortestAngleDelta(root._lastCursorAngle, cur)
                root._lastCursorAngle = cur

                if (!root._scratchEngaged) {
                    root._accumDragAngle += Math.abs(delta)
                    if (root._accumDragAngle < root.scratchDeadzoneDeg) return
                    root._scratchEngaged = true
                }

                root._totalScratchedAngle += delta
                var deltaSec = delta / root.degreesPerSecond
                root.engine.scratchBySeconds(deltaSec)
            }

            onReleased: {
                if (!root.dragActive || !root.engine) return
                root.dragActive = false
                root._scratchEngaged      = false
                root._accumDragAngle      = 0.0
                root._totalScratchedAngle = 0.0
                root.engine.resumeAfterScrub()
            }

            onCanceled: {
                root.dragActive = false
                root._scratchEngaged      = false
                root._accumDragAngle      = 0.0
                root._totalScratchedAngle = 0.0
                if (root.engine) root.engine.resumeAfterScrub()
            }
        }

        // Share Qt Quick's VSync animation clock with both waveform views. The
        // platter is a position indicator, so sampling it from a drifting wall
        // timer makes an otherwise smooth waveform look as if it briefly slips.
        FrameAnimation {
            running: root.motionActive && root.motionIntervalMs <= 17
            onTriggered: root.updateRotation()
        }

        Timer {
            interval: root.motionIntervalMs
            repeat: true
            running: root.motionActive && root.motionIntervalMs > 17
            onTriggered: root.updateRotation()
        }

        Connections {
            target: root.engine
            function onPlayingChanged()  { root.updateRotation() }
            function onProgressChanged() {
                if (!root.engine || (!root.engine.isPlaying && !root.engine.scratchVisualActive))
                    root.updateRotation()
            }
            function onTempoChanged()    { root.updateRotation() }
            function onReverseChanged()  { root.updateRotation() }
            function onTrackLoaded()     { root.updateRotation() }
        }

        onEngineChanged:       updateRotation()
        Component.onCompleted: updateRotation()
    }
}
