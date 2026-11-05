import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    objectName: "effectsPage"
    property var manager: typeof fxManager !== "undefined" ? fxManager : null
    property int selectedUnit: 1
    property int section: 0
    property bool touching: false
    property real touchX: 0
    property real touchY: 0.5
    property var touchSnapshot: null
    property var touchOwner: null
    readonly property var effectNames: ["---", "Echo", "Low Cut Echo", "Multi-Tap Delay", "Roll", "Roll Out", "Slip Roll",
        "Mobius Saw", "Mobius Tri", "Tremolo", "Flanger", "Phaser", "Spiral", "Enigma Jet", "Reverb", "Bitcrusher", "Stretch"]
    readonly property var divisions: [0.0625, 0.125, 0.25, 0.5, 1, 2, 4]
    readonly property var divisionLabels: ["1/16", "1/8", "1/4", "1/2", "1", "2", "4"]
    readonly property string effect: manager ? manager["effectType" + selectedUnit] : "---"
    readonly property bool effectEnabled: manager ? manager["enabled" + selectedUnit] : false
    readonly property real mix: manager ? manager["wetDry" + selectedUnit] : 0
    readonly property real parameter: manager ? manager["primaryParam" + selectedUnit] : 0.5
    readonly property real division: manager ? manager["beatDiv" + selectedUnit] : 0.25
    readonly property real bpm: manager ? manager["displayBpm" + selectedUnit] : 0
    readonly property bool synced: manager ? manager["syncEnabled" + selectedUnit] : false
    readonly property bool quantized: manager ? manager["quantizeEnabled" + selectedUnit] : false
    readonly property bool reverb: effect === "Reverb"
    readonly property bool timed: ["Echo", "Low Cut Echo", "MT Delay", "Multi-Tap Delay", "Roll", "Roll Out", "Slip Roll", "Mobius Saw", "Mobius Tri", "Trans", "Tremolo", "Flanger", "Phaser", "Spiral", "Enigma Jet"].indexOf(effect) >= 0
    readonly property bool routed: assigned(0) || assigned(1) || assigned(2) || assigned(3)
    readonly property bool touchAvailable: !!manager && routed && (reverb || (timed && bpm > 0))
    readonly property color accent: UiTheme.blue
    color: UiTheme.panel
    radius: 0

    function assigned(deck) { return !!manager && manager["deck" + selectedUnit + "ABCD"[deck]] === true }
    function nearestDivision(value) {
        let nearest = 0
        for (let i = 1; i < divisions.length; ++i)
            if (Math.abs(divisions[i] - value) < Math.abs(divisions[nearest] - value)) nearest = i
        return nearest
    }
    function moveTouch(x, y) {
        if (!touching || !touchOwner) return
        touchX = Math.max(0, Math.min(1, x))
        touchY = Math.max(0, Math.min(1, y))
        const unit = touchSnapshot.unit
        touchOwner.setWetDry(unit, touchX)
        if (touchSnapshot.reverb) touchOwner.setPrimaryParam(unit, Math.max(0.01, touchY))
        else {
            touchOwner.setSyncEnabled(unit, true)
            touchOwner.setBeatDivision(unit, divisions[Math.min(6, Math.floor(touchY * 7))])
        }
    }
    function beginTouch(x, y) {
        if (touching || !touchAvailable) return
        touchOwner = manager
        touchSnapshot = { unit: selectedUnit, enabled: effectEnabled, mix: mix, parameter: parameter,
            division: division, sync: synced, quantize: quantized, reverb: reverb }
        touching = true
        moveTouch(x, y)
        touchOwner.setUnitEnabled(selectedUnit, true)
    }
    function endTouch() {
        if (!touching) return
        touching = false
        const previous = touchSnapshot, owner = touchOwner
        touchSnapshot = null; touchOwner = null
        if (!owner || !previous) return
        if (!previous.enabled) owner.setUnitEnabled(previous.unit, false)
        owner.setWetDry(previous.unit, previous.mix)
        owner.setPrimaryParam(previous.unit, previous.parameter)
        owner.setBeatDivision(previous.unit, previous.division)
        owner.setSyncEnabled(previous.unit, previous.sync)
        owner.setQuantizeEnabled(previous.unit, previous.quantize)
        owner.setUnitEnabled(previous.unit, previous.enabled)
    }
    onVisibleChanged: if (!visible) endTouch()
    onSelectedUnitChanged: endTouch()
    onSectionChanged: endTouch()
    onManagerChanged: endTouch()
    Component.onDestruction: endTouch()
    Connections {
        target: root.Window.window
        function onActiveChanged() { if (!root.Window.window.active) root.endTouch() }
    }
    Connections {
        target: root.manager
        function onEffectType1Changed() { if (root.selectedUnit === 1) root.endTouch() }
        function onEffectType2Changed() { if (root.selectedUnit === 2) root.endTouch() }
    }

    Flickable {
        anchors.fill: parent; anchors.margins: UiTheme.px(20)
        clip: true; contentWidth: width; contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: content
            width: parent.width
            spacing: UiTheme.px(14)
            RowLayout {
                Layout.fillWidth: true
                Text { text: "EFFECTS"; color: UiTheme.textPrimary; font.pixelSize: 20; font.bold: true }
                Item { Layout.fillWidth: true }
                Repeater {
                    model: 2
                    Button {
                        required property int index
                        objectName: "fxPageUnit" + (index + 1)
                        text: "FX " + (index + 1)
                        checked: root.selectedUnit === index + 1
                        onClicked: root.selectedUnit = index + 1
                    }
                }
                Button {
                    objectName: "fxPageEnabled"
                    text: root.effectEnabled ? "FX ON" : "FX OFF"
                    checked: root.effectEnabled
                    enabled: !!root.manager && root.effect !== "---" && root.routed && !root.touching
                    onClicked: root.manager.setUnitEnabled(root.selectedUnit, !root.effectEnabled)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ComboBox {
                    id: effectSelector
                    objectName: "fxPageEffect"
                    Layout.fillWidth: true; Layout.maximumWidth: 300
                    model: root.effectNames
                    currentIndex: root.effectNames.indexOf(root.effect === "MT Delay" ? "Multi-Tap Delay" : root.effect === "Trans" ? "Tremolo" : root.effect)
                    displayText: root.effect
                    onActivated: if (root.manager) { root.endTouch(); root.manager.setEffectType(root.selectedUnit, currentText) }
                }
                Text { text: "TO"; color: UiTheme.textSecondary; font.pixelSize: 11 }
                Repeater {
                    model: 4
                    Button {
                        required property int index
                        objectName: "fxPageDeck" + index
                        text: "ABCD"[index]
                        Layout.preferredWidth: UiTheme.touchTarget
                        checked: root.assigned(index)
                        enabled: !!root.manager && !root.touching
                        onClicked: root.manager.setDeckAssignment(root.selectedUnit, index + 1, !root.assigned(index))
                    }
                }
                Text { text: root.bpm > 0 ? root.bpm.toFixed(1) + " BPM" : "— BPM"; color: UiTheme.textSecondary; font.family: UiTheme.numericFontFamily; font.pixelSize: 12 }
            }
            RowLayout {
                Layout.fillWidth: true
                Button { objectName: "touchFxSection"; text: "TOUCH FX"; checked: root.section === 0; onClicked: root.section = 0 }
                Button { objectName: "fxSettingsSection"; text: "FX SETTINGS"; checked: root.section === 1; onClicked: root.section = 1 }
                Item { Layout.fillWidth: true }
            }
            ColumnLayout {
                visible: root.section === 0
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    id: xyPad
                    objectName: "touchFxPad"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(UiTheme.px(220), Math.min(UiTheme.px(440), root.height - UiTheme.px(245)))
                    color: UiTheme.displayBackground
                    border.color: root.touching ? root.accent : UiTheme.borderSubtle
                    radius: 0
                    Repeater {
                        model: 3
                        Rectangle { required property int index; x: (index + 1) * xyPad.width / 4; width: 1; height: xyPad.height; color: UiTheme.borderSubtle; opacity: 0.5 }
                    }
                    Repeater {
                        model: 3
                        Rectangle { required property int index; y: (index + 1) * xyPad.height / 4; height: 1; width: xyPad.width; color: UiTheme.borderSubtle; opacity: 0.5 }
                    }
                    Rectangle { visible: root.touching; x: root.touchX * (parent.width - 1); width: 1; height: parent.height; color: root.accent }
                    Rectangle { visible: root.touching; y: (1 - root.touchY) * (parent.height - 1); height: 1; width: parent.width; color: root.accent }
                    Text {
                        anchors.centerIn: parent
                        text: !root.routed ? "SELECT A DECK ABOVE" : root.effect === "---" ? "SELECT AN EFFECT"
                            : !root.touchAvailable ? "SELECT REVERB OR A BEAT FX WITH BPM"
                            : root.touching ? Math.round(root.touchX * 100) + "%  ·  " + (root.reverb ? Math.round(root.touchY * 100) + "%" : root.divisionLabels[Math.min(6, Math.floor(root.touchY * 7))])
                            : "HOLD + DRAG"
                        color: root.touching ? root.accent : UiTheme.textSecondary
                        font.pixelSize: 18; font.family: UiTheme.numericFontFamily
                    }
                    Text { anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 12; text: "DRY  →  WET"; color: UiTheme.textSecondary; font.pixelSize: 11 }
                    Text { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 12; text: root.reverb ? "ROOM SIZE ↑" : "BEAT LENGTH ↑"; color: UiTheme.textSecondary; font.pixelSize: 11 }
                    MouseArea {
                        anchors.fill: parent
                        enabled: root.touchAvailable
                        preventStealing: true
                        onPressed: mouse => root.beginTouch(mouse.x / width, 1 - mouse.y / height)
                        onPositionChanged: mouse => { if (pressed) root.moveTouch(mouse.x / width, 1 - mouse.y / height) }
                        onReleased: root.endTouch()
                        onCanceled: root.endTouch()
                    }
                }
                Text { Layout.fillWidth: true; text: "Hold to apply. Release restores the selected FX unit's previous settings."; color: UiTheme.textSecondary; wrapMode: Text.Wrap; font.pixelSize: 12 }
            }
            ColumnLayout {
                visible: root.section === 1
                Layout.fillWidth: true
                spacing: UiTheme.px(14)
                Text { text: "FX " + root.selectedUnit + " · " + root.effect; color: UiTheme.textPrimary; font.pixelSize: 16 }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "DRY / WET"; color: UiTheme.textSecondary; Layout.preferredWidth: 110 }
                    Slider { objectName: "fxPageMix"; Layout.fillWidth: true; from: 0; to: 1; value: root.mix; onMoved: if (root.manager) root.manager.setWetDry(root.selectedUnit, value) }
                    Text { text: Math.round(root.mix * 100) + "%"; color: UiTheme.textPrimary; Layout.preferredWidth: 44 }
                }
                RowLayout {
                    Button { text: "BPM SYNC"; checked: root.synced; onClicked: if (root.manager) root.manager.setSyncEnabled(root.selectedUnit, !root.synced) }
                    Button { text: "QUANTIZE"; checked: root.quantized; onClicked: if (root.manager) root.manager.setQuantizeEnabled(root.selectedUnit, !root.quantized) }
                    ComboBox { model: root.divisionLabels; currentIndex: root.nearestDivision(root.division); onActivated: if (root.manager) root.manager.setBeatDivision(root.selectedUnit, root.divisions[currentIndex]) }
                }
                RowLayout {
                    visible: root.reverb
                    Layout.fillWidth: true
                    Text { text: "ROOM SIZE"; color: UiTheme.textSecondary; Layout.preferredWidth: 110 }
                    Slider { Layout.fillWidth: true; from: 0.01; to: 1; value: root.parameter; onMoved: if (root.manager) root.manager.setPrimaryParam(root.selectedUnit, value) }
                    Text { text: Math.round(root.parameter * 100) + "%"; color: UiTheme.textPrimary; Layout.preferredWidth: 44 }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: UiTheme.borderSubtle }
                Text { text: "SOUND COLOR"; color: UiTheme.textPrimary; font.pixelSize: 16 }
                RowLayout {
                    ComboBox {
                        readonly property var modes: ["None", "Filter", "Space", "D.Echo", "Crush", "Pitch", "Noise", "Sweep"]
                        model: modes
                        currentIndex: root.manager ? modes.indexOf(root.manager.soundColorMode) : 0
                        onActivated: if (root.manager) root.manager.setSoundColorMode(currentText)
                    }
                    Button { text: "CHARACTER ADJUST"; checked: root.manager ? root.manager.soundColorAdjustEnabled : false; onClicked: if (root.manager) root.manager.soundColorAdjustEnabled = !root.manager.soundColorAdjustEnabled }
                }
                Slider { Layout.fillWidth: true; from: 0; to: 1; enabled: !!root.manager && root.manager.soundColorAdjustEnabled; value: root.manager ? root.manager.soundColorParam : 0.5; onMoved: if (root.manager) root.manager.soundColorParam = value }
            }
        }
    }
}
