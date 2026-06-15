import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import DJSoftware

// Mirrored centre mixer — each side:
//   A (left):  [VOL fader | VU | knobs↓ + CUE bottom] ── spine
//   B (right):  spine ── [knobs↓ + CUE bottom | VU | VOL fader]

Rectangle {
    id: mixer
    color: UiTheme.bgDeep

    property var  engineA: null
    property var  engineB: null
    property bool cueAActive: false
    property bool cueBActive: false
    property string channelAId: "deckA"
    property string channelBId: "deckB"
    property string deckNameA: "A"
    property string deckNameB: "B"

    readonly property real vuACombined: engineA ? Math.max(engineA.preFaderVuLevelL, engineA.preFaderVuLevelR) : 0.0
    readonly property real vuBCombined: engineB ? Math.max(engineB.preFaderVuLevelL, engineB.preFaderVuLevelR) : 0.0

    readonly property color clrA: UiTheme.deckColor(deckNameA)
    readonly property color clrB: UiTheme.deckColor(deckNameB)
    readonly property color clrKnob: "#aaaaaa"

    readonly property int labelH:   22
    readonly property int knobColW: 44
    readonly property int vuW:      11
    readonly property int faderW:   40
    readonly property int cueH:     20
    readonly property int knobSz:   21
    readonly property int spineW:   3
    readonly property int stripW:   faderW + 1 + vuW + 1 + knobColW

    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if      (id === mixer.channelAId + "_vol")    sideA.volFader.value      = value
            else if (id === mixer.channelBId + "_vol")    sideB.volFader.value      = value
            else if (id === mixer.channelAId + "_eqHigh") sideA.eqHighCell.knob.value = value * 2.0 - 1.0
            else if (id === mixer.channelAId + "_eqMid")  sideA.eqMidCell.knob.value  = value * 2.0 - 1.0
            else if (id === mixer.channelAId + "_eqLow")  sideA.eqLowCell.knob.value  = value * 2.0 - 1.0
            else if (id === mixer.channelBId + "_eqHigh") sideB.eqHighCell.knob.value = value * 2.0 - 1.0
            else if (id === mixer.channelBId + "_eqMid")  sideB.eqMidCell.knob.value  = value * 2.0 - 1.0
            else if (id === mixer.channelBId + "_eqLow")  sideB.eqLowCell.knob.value  = value * 2.0 - 1.0
            else if (id === mixer.channelAId + "_gain")   sideA.gainCell.knob.value   = value * 2.0
            else if (id === mixer.channelBId + "_gain")   sideB.gainCell.knob.value   = value * 2.0
            else if (id === mixer.channelAId + "_filter") sideA.scCell.knob.value     = value * 2.0 - 1.0
            else if (id === mixer.channelBId + "_filter") sideB.scCell.knob.value     = value * 2.0 - 1.0
        }
    }
    Connections {
        target: engineA
        function onCueEnabledChanged() { mixer.cueAActive = engineA ? engineA.cueEnabled : false }
    }
    Connections {
        target: engineB
        function onCueEnabledChanged() { mixer.cueBActive = engineB ? engineB.cueEnabled : false }
    }

    // ── Shared components ───────────────────────────────────────────────────

    component SilkLabel: Text {
        color: UiTheme.textLabel
        font.pixelSize: 6
        font.bold: true
        font.family: "monospace"
        font.letterSpacing: 0.4
        horizontalAlignment: Text.AlignHCenter
    }

    component KnobCell: Item {
        id: kc
        property alias knob: innerKnob
        property string label: ""
        property real size: mixer.knobSz

        implicitWidth: size + 14
        implicitHeight: size + 10
        Layout.preferredWidth: implicitWidth
        Layout.preferredHeight: implicitHeight
        Layout.alignment: Qt.AlignHCenter

        Knob {
            id: innerKnob
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: kc.size
            height: kc.size
            accentColor: mixer.clrKnob
        }
        SilkLabel {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: innerKnob.bottom
            text: kc.label
        }
    }

    component VuMeterVertical: Rectangle {
        id: vu
        required property real levelLinear

        color: UiTheme.bgDeep
        radius: 1
        border.width: 1
        border.color: UiTheme.bezelShadow

        property real peakHoldLevel: 0.0
        readonly property int segs: 28
        readonly property real segH: Math.max(1, (height - (segs - 1)) / segs)

        function segColor(i) {
            if (i >= Math.floor(segs * 0.88)) return UiTheme.red
            if (i >= Math.floor(segs * 0.72)) return "#d48000"
            if (i >= Math.floor(segs * 0.50)) return "#5aba52"
            return "#2a9640"
        }

        onLevelLinearChanged: {
            if (levelLinear > peakHoldLevel) { peakHoldLevel = levelLinear; decayTimer.restart() }
        }
        Timer { id: decayTimer; interval: 350; onTriggered: decayAnim.start() }
        NumberAnimation {
            id: decayAnim; target: vu; property: "peakHoldLevel"
            from: vu.peakHoldLevel; to: 0.0; duration: 900; easing.type: Easing.InQuad
        }

        Column {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 1
            Repeater {
                model: vu.segs
                delegate: Rectangle {
                    required property int index
                    width: vu.width - 2
                    height: vu.segH
                    color: {
                        const ri = vu.segs - 1 - index
                        const lit = Math.floor(vu.levelLinear * vu.segs)
                        const peak = Math.floor(vu.peakHoldLevel * vu.segs) - 1
                        if (ri === peak && peak >= 0) return "#ffffff"
                        if (ri < lit) return vu.segColor(ri)
                        return "#101010"
                    }
                }
            }
        }
    }

    component MixerSlider: Controls.Slider {
        id: ms
        property color capAccent: UiTheme.textSecondary
        property bool dragActive: false
        property real defaultValue: 1.0

        background: Item {
            x: ms.width / 2 - 4
            y: ms.topPadding
            width: 8
            height: ms.availableHeight

            Rectangle {
                anchors.fill: parent
                radius: 1
                color: UiTheme.bgDeep
                border.width: 1
                border.color: UiTheme.bezelShadow
            }
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.bottom: parent.bottom; anchors.margins: 1
                height: Math.max(0, (1.0 - ms.visualPosition) * (parent.height - 2))
                radius: 1
                color: ms.pressed ? UiTheme.borderHover : UiTheme.faderFill
                opacity: 0.85
            }
        }

        handle: Rectangle {
            implicitWidth: 26
            implicitHeight: 13
            x: ms.width / 2 - width / 2
            y: ms.topPadding + ms.visualPosition * (ms.availableHeight - height)
            radius: 1
            color: ms.pressed || ms.dragActive ? "#eeeeee" : UiTheme.faderCap
            border.width: 1
            border.color: UiTheme.borderHover

            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.top: parent.top; height: 2
                color: ms.capAccent
            }
        }

        MouseArea {
            id: msDragLock
            anchors.fill: parent; z: 100
            acceptedButtons: Qt.LeftButton
            preventStealing: true
            property real _pressGX: 0; property real _pressGY: 0; property real _pressVal: 0

            onPressed: (mouse) => {
                var g = msDragLock.mapToGlobal(mouse.x, mouse.y)
                _pressGX = g.x; _pressGY = g.y; _pressVal = ms.value
                ms.dragActive = false; mouse.accepted = true
            }
            onPositionChanged: (mouse) => {
                var g = msDragLock.mapToGlobal(mouse.x, mouse.y)
                var delta = _pressGY - g.y
                if (!ms.dragActive) { if (Math.abs(delta) < 4) return; ms.dragActive = true; cursorControl.hideCursor() }
                ms.value = Math.max(ms.from, Math.min(ms.to, _pressVal + delta * (ms.to - ms.from) / 150.0))
            }
            onReleased: {
                if (ms.dragActive) { ms.dragActive = false; cursorControl.restoreCursor(); cursorControl.moveCursor(_pressGX, _pressGY) }
            }
            onDoubleClicked: { ms.enabled = false; ms.value = ms.defaultValue; ms.enabled = true }
        }
    }

    component ChannelHeader: Rectangle {
        required property string deckName
        required property color accent
        required property bool mirrored

        Layout.fillWidth: true
        Layout.preferredHeight: mixer.labelH
        color: UiTheme.bg0

        Rectangle {
            width: 3
            height: parent.height
            anchors.left: mirrored ? parent.left : undefined
            anchors.right: mirrored ? undefined : parent.right
            color: accent
            opacity: 0.9
        }
        Text {
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: mirrored ? -10 : 10
            text: deckName
            color: accent
            font.pixelSize: window.spViewport(9)
            font.bold: true
            font.family: "monospace"
            font.letterSpacing: 2.0
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: UiTheme.divider }
    }

    component KnobStackColumn: ColumnLayout {
        required property var engine
        required property bool cueActive
        required property int soundColorDeck
        required property bool mirrored
        required property color accent

        property alias gainCell: gainKnob
        property alias scCell: scKnob
        property alias eqHighCell: eqHi
        property alias eqMidCell: eqMid
        property alias eqLowCell: eqLo

        Layout.preferredWidth: mixer.knobColW
        Layout.minimumWidth: mixer.knobColW
        Layout.maximumWidth: mixer.knobColW
        Layout.fillHeight: true
        spacing: 1

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: mixer.knobColW
            color: UiTheme.panelInset
            border.width: 1
            border.color: UiTheme.bezelInner

            Rectangle {
                width: 1
                height: parent.height
                anchors.left: mirrored ? parent.left : undefined
                anchors.right: mirrored ? undefined : parent.right
                color: accent
                opacity: 0.35
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 2
                spacing: 1

                KnobCell {
                    id: gainKnob
                    label: "GAIN"
                    knob.from: 0; knob.to: 2; knob.value: 1.0; knob.defaultValue: 1.0
                    knob.onValueChanged: { if (engine) engine.trim = knob.value }
                }
                KnobCell {
                    id: eqHi
                    label: "HI"
                    knob.from: -1; knob.to: 1; knob.value: 0
                    knob.onValueChanged: { if (engine) engine.eqHigh = knob.value }
                }
                KnobCell {
                    id: eqMid
                    label: "MID"
                    knob.from: -1; knob.to: 1; knob.value: 0
                    knob.onValueChanged: { if (engine) engine.eqMid = knob.value }
                }
                KnobCell {
                    id: eqLo
                    label: "LOW"
                    knob.from: -1; knob.to: 1; knob.value: 0
                    knob.onValueChanged: { if (engine) engine.eqLow = knob.value }
                }
                KnobCell {
                    id: scKnob
                    label: "SC"
                    knob.from: -1; knob.to: 1; knob.value: 0; knob.defaultValue: 0
                    knob.onValueChanged: {
                        if (typeof fxManager !== "undefined") {
                            fxManager.setSoundColorDeck(soundColorDeck, knob.value)
                            if (engine) engine.filter = fxManager.soundColorMode === "Filter" ? knob.value : 0.0
                        } else if (engine) {
                            engine.filter = knob.value
                        }
                    }
                }

                Item { Layout.fillHeight: true; Layout.minimumHeight: 2 }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: mixer.cueH
                    color: cueActive ? UiTheme.greenDim : UiTheme.bg3
                    border.width: 1
                    border.color: cueActive ? Qt.darker(UiTheme.green, 1.6) : UiTheme.border

                    SilkLabel {
                        anchors.centerIn: parent
                        text: "CUE"
                        color: cueActive ? UiTheme.green : UiTheme.textDim
                        font.pixelSize: 7
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                        height: cueActive ? 2 : 0; color: UiTheme.green
                    }
                    HoverHandler { id: cueHov; cursorShape: Qt.PointingHandCursor }
                    Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: cueHov.hovered && !cueActive ? 0.05 : 0 }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: { if (engine) engine.cueEnabled = !engine.cueEnabled }
                    }
                }
            }
        }
    }

    component FaderColumn: Item {
        required property color faderAccent
        required property string channelId
        property alias volFader: fader

        Layout.preferredWidth: mixer.faderW
        Layout.minimumWidth: mixer.faderW
        Layout.maximumWidth: mixer.faderW
        Layout.fillHeight: true

        Rectangle {
            anchors.fill: parent
            color: UiTheme.bgDeep
            border.width: 1
            border.color: UiTheme.bezelShadow

            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                height: 1; color: UiTheme.bezelHighlight; opacity: 0.15
            }

            MixerSlider {
                id: fader
                anchors.fill: parent
                anchors.margins: 6
                orientation: Qt.Vertical
                from: 0.0; to: 1.0; value: 1.0
                capAccent: faderAccent
                onValueChanged: {
                    if (parameterStore && parameterStore.getParameter(channelId + "_vol") !== value)
                        parameterStore.setParameter(channelId + "_vol", value)
                }
            }
        }

        SilkLabel {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            text: "VOL"
        }
    }

    component ChannelSide: ColumnLayout {
        required property string deckName
        required property color deckAccent
        required property bool mirrored
        required property var engine
        required property bool cueActive
        required property real vuLevel
        required property string channelId
        required property int soundColorDeck

        property alias gainCell: knobs.gainCell
        property alias scCell: knobs.scCell
        property alias eqHighCell: knobs.eqHighCell
        property alias eqMidCell: knobs.eqMidCell
        property alias eqLowCell: knobs.eqLowCell
        property alias volFader: faderCol.volFader

        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: 0

        ChannelHeader { deckName: deckName; accent: deckAccent; mirrored: mirrored }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            LayoutMirroring.enabled: mirrored
            LayoutMirroring.childrenInherit: false

            Item { Layout.fillWidth: true; Layout.fillHeight: true }

            FaderColumn {
                id: faderCol
                faderAccent: deckAccent
                channelId: channelId
            }

            Rectangle { width: 1; Layout.fillHeight: true; color: UiTheme.divider }

            VuMeterVertical {
                Layout.fillHeight: true
                Layout.preferredWidth: mixer.vuW
                levelLinear: vuLevel
            }

            Rectangle { width: 1; Layout.fillHeight: true; color: UiTheme.divider }

            KnobStackColumn {
                id: knobs
                engine: engine
                cueActive: cueActive
                soundColorDeck: soundColorDeck
                mirrored: mirrored
                accent: deckAccent
            }
        }
    }

    // ── Main layout ─────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        color: UiTheme.bg1
        border.width: 1
        border.color: UiTheme.bezelOuter

        RowLayout {
            anchors.fill: parent
            spacing: 0

            ChannelSide {
                id: sideA
                deckName: mixer.deckNameA
                deckAccent: mixer.clrA
                mirrored: false
                engine: mixer.engineA
                cueActive: mixer.cueAActive
                vuLevel: mixer.vuACombined
                channelId: mixer.channelAId
                soundColorDeck: 1
            }

            Rectangle {
                Layout.preferredWidth: mixer.spineW
                Layout.fillHeight: true
                color: UiTheme.bgDeep

                Rectangle {
                    anchors.left: parent.left
                    width: 1
                    height: parent.height
                    color: UiTheme.bezelShadow
                }
                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: UiTheme.bezelHighlight
                    opacity: 0.35
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 1
                    height: parent.height
                    color: UiTheme.dividerStrong
                }
            }

            ChannelSide {
                id: sideB
                deckName: mixer.deckNameB
                deckAccent: mixer.clrB
                mirrored: true
                engine: mixer.engineB
                cueActive: mixer.cueBActive
                vuLevel: mixer.vuBCombined
                channelId: mixer.channelBId
                soundColorDeck: 2
            }
        }
    }

    // Aliases for parameterStore / external bindings
    property alias volFaderA: sideA.volFader
    property alias volFaderB: sideB.volFader
    property alias gainCellA: sideA.gainCell
    property alias gainCellB: sideB.gainCell
    property alias scCellA: sideA.scCell
    property alias scCellB: sideB.scCell
    property alias eqHighCellA: sideA.eqHighCell
    property alias eqMidCellA: sideA.eqMidCell
    property alias eqLowCellA: sideA.eqLowCell
    property alias eqHighCellB: sideB.eqHighCell
    property alias eqMidCellB: sideB.eqMidCell
    property alias eqLowCellB: sideB.eqLowCell
}
