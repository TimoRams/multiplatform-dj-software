import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls

// Traktor-style mirrored mixer layout:
//
//  [A outer: GAIN / SC / CUE]  |  [VU A | EQ A | VOL A]  ║  [EQ B | VU B | VOL B]  |  [B outer: GAIN / SC / CUE]
//
// VU meters span exactly the height of the three EQ knobs.
// GAIN and SC knobs sit alongside the EQ area; CUE button below them.
// Volume faders fill the remaining height beneath the EQ area.

Rectangle {
    id: mixer
    color: "#0a0a0a"

    property var  engineA: null
    property var  engineB: null
    property bool cueAActive: false
    property bool cueBActive: false
    property string channelAId: "deckA"
    property string channelBId: "deckB"

    readonly property real vuACombined: engineA ? Math.max(engineA.preFaderVuLevelL, engineA.preFaderVuLevelR) : 0.0
    readonly property real vuBCombined: engineB ? Math.max(engineB.preFaderVuLevelL, engineB.preFaderVuLevelR) : 0.0

    readonly property color clrA:     "#ff9900"
    readonly property color clrB:     "#00ccff"
    readonly property color clrAKnob: Qt.darker(clrA, 1.4)
    readonly property color clrBKnob: Qt.darker(clrB, 1.4)

    readonly property int   labelH:   24   // channel header height
    readonly property int   outerW:   52   // outer strip width (GAIN/SC/CUE column)
    readonly property int   vuW:       9   // VU meter width
    readonly property int   btnH:     22   // CUE button height
    readonly property int   outerKnob: 20  // knob size in outer strip
    readonly property int   eqRowH:   3 * (22 + 13)  // locked EQ+VU row height (3 KnobCells)

    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if      (id === mixer.channelAId + "_vol") volFaderA.value = value
            else if (id === mixer.channelBId + "_vol") volFaderB.value = value
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

    // ── KnobCell: Knob + label text ───────────────────────────────────────
    component KnobCell: Item {
        id: kc
        property alias knob:    innerKnob
        property string label:  ""
        property real   size:   22

        implicitWidth:  size + 16
        implicitHeight: size + 13
        Layout.preferredWidth:  implicitWidth
        Layout.minimumWidth:    implicitWidth
        Layout.maximumWidth:    implicitWidth
        Layout.preferredHeight: implicitHeight
        Layout.minimumHeight:   implicitHeight
        Layout.maximumHeight:   implicitHeight
        Layout.alignment: Qt.AlignHCenter

        Knob {
            id: innerKnob
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width:  kc.size
            height: kc.size
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: innerKnob.bottom
            anchors.topMargin: 2
            text:           kc.label
            color:          "#555555"
            font.pixelSize: 7
            font.bold:      true
            font.family:    "monospace"
            font.letterSpacing: 0.3
        }
    }

    // ── VU meter ──────────────────────────────────────────────────────────
    component VuMeterVertical: Rectangle {
        id: vu
        required property real   levelLinear
        required property string deckName

        color:  "#060606"
        radius: 1

        property real peakHoldLevel: 0.0
        readonly property int  segs: 24
        readonly property real segH: Math.max(1, (height - (segs - 1)) / segs)

        function segColor(i) {
            if (i >= Math.floor(segs * 0.88)) return "#e03535"
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
            anchors.fill: parent; spacing: 1
            Repeater {
                model: vu.segs
                delegate: Rectangle {
                    required property int index
                    width: vu.width; height: vu.segH; radius: 0
                    color: {
                        const ri   = vu.segs - 1 - index
                        const lit  = Math.floor(vu.levelLinear  * vu.segs)
                        const peak = Math.floor(vu.peakHoldLevel * vu.segs) - 1
                        if (ri === peak && peak >= 0) return "#ffffff"
                        if (ri < lit)                 return vu.segColor(ri)
                        return "#141414"
                    }
                }
            }
        }
    }

    // ── Volume fader ──────────────────────────────────────────────────────
    component MixerSlider: Controls.Slider {
        id: ms
        property bool centerFill: false

        background: Rectangle {
            x: ms.orientation === Qt.Horizontal ? ms.leftPadding  : ms.width  / 2 - 2
            y: ms.orientation === Qt.Horizontal ? ms.height / 2 - 2 : ms.topPadding
            width:  ms.orientation === Qt.Horizontal ? ms.availableWidth  : 4
            height: ms.orientation === Qt.Horizontal ? 4 : ms.availableHeight
            radius: 1; color: "#111111"
            border.width: 1; border.color: "#080808"

            Rectangle {
                visible: ms.orientation === Qt.Vertical
                x: 1
                y: parent.height - 1 - Math.max(0, (1.0 - ms.visualPosition) * (parent.height - 2))
                width:  parent.width  - 2
                height: Math.max(0, (1.0 - ms.visualPosition) * (parent.height - 2))
                radius: 1; color: ms.pressed ? "#555555" : "#333333"
            }
        }

        handle: Rectangle {
            implicitWidth:  ms.orientation === Qt.Vertical ? 26 : 18
            implicitHeight: ms.orientation === Qt.Vertical ? 12 : 22
            x: ms.orientation === Qt.Horizontal
               ? ms.leftPadding + ms.visualPosition * (ms.availableWidth - width)
               : ms.width / 2 - width / 2
            y: ms.orientation === Qt.Horizontal
               ? ms.height / 2 - height / 2
               : ms.topPadding + ms.visualPosition * (ms.availableHeight - height)
            radius: 1; color: ms.pressed ? "#e0e0e0" : "#c8c8c8"
            border.width: 1; border.color: "#555555"

            Rectangle {
                anchors.centerIn: parent
                width:  ms.orientation === Qt.Vertical ? parent.width * 0.48 : 2
                height: ms.orientation === Qt.Vertical ? 1 : parent.height * 0.48
                color:  "#888888"
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Main layout
    // ─────────────────────────────────────────────────────────────────────
    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ════════════════════════════════════════════════════════════════
        // DECK A (left side)
        // ════════════════════════════════════════════════════════════════
        ColumnLayout {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            spacing: 0

            // Header
            Rectangle {
                Layout.fillWidth:      true
                Layout.preferredHeight: mixer.labelH
                color: "#080808"

                Rectangle { width: 3; height: parent.height; color: mixer.clrA; opacity: 0.85 }
                Text {
                    anchors.centerIn:   parent
                    text:               "A"
                    color:              mixer.clrA
                    font.pixelSize:     window.spViewport(10)
                    font.bold:          true
                    font.family:        "monospace"
                    font.letterSpacing: 2.0
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
            }

            // Content row: [outer controls] | [EQ area + vol fader]
            RowLayout {
                Layout.fillWidth:  true
                Layout.fillHeight: true
                spacing: 0

                // ── Outer A (GAIN / SC / CUE) ─────────────────────────
                ColumnLayout {
                    Layout.preferredWidth: mixer.outerW
                    Layout.minimumWidth:   mixer.outerW
                    Layout.maximumWidth:   mixer.outerW
                    Layout.fillHeight:     true
                    Layout.alignment:      Qt.AlignTop
                    spacing: 0

                    KnobCell {
                        id: gainCellA
                        label: "GAIN"; size: mixer.outerKnob
                        Layout.alignment: Qt.AlignHCenter
                        knob.from: 0; knob.to: 2; knob.value: 1.0
                        knob.defaultValue: 1.0
                        knob.accentColor: mixer.clrAKnob
                        knob.onValueChanged: { if (engineA) engineA.trim = knob.value }
                    }

                    KnobCell {
                        id: scCellA
                        label: "SC"; size: mixer.outerKnob
                        Layout.alignment: Qt.AlignHCenter
                        knob.from: -1; knob.to: 1; knob.value: 0.0; knob.defaultValue: 0.0
                        knob.accentColor: Qt.darker(mixer.clrA, 1.6)
                        knob.onValueChanged: {
                            if (typeof fxManager !== "undefined") {
                                fxManager.setSoundColorDeck(1, knob.value)
                                if (fxManager.soundColorMode === "Filter") { if (engineA) engineA.filter = knob.value }
                                else { if (engineA) engineA.filter = 0.0 }
                            } else { if (engineA) engineA.filter = knob.value }
                        }
                    }

                    // CUE button
                    Rectangle {
                        Layout.fillWidth:      true
                        Layout.preferredHeight: mixer.btnH
                        color:        mixer.cueAActive ? "#0c2016" : "#141414"
                        border.width: 1
                        border.color: mixer.cueAActive ? "#1e5030" : "#1c1c1c"

                        HoverHandler { id: cueAHov }
                        Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: cueAHov.hovered && !mixer.cueAActive ? 0.04 : 0 }

                        Text {
                            anchors.centerIn: parent; text: "CUE"
                            color:          mixer.cueAActive ? "#4dd98a" : "#555555"
                            font.pixelSize: window.spViewport(8)
                            font.bold:      true; font.family: "monospace"; font.letterSpacing: 0.6
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 2; color: "#4dd98a"; visible: mixer.cueAActive
                        }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                mixer.cueAActive = !mixer.cueAActive
                                if (engineA) engineA.cueEnabled = mixer.cueAActive
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }  // spacer aligned with vol fader area
                }

                Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                // ── Center A (VU | EQ H/M/L | Vol fader) ─────────────────
                ColumnLayout {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    spacing: 0

                    // EQ + VU row – locked height, EQ pushed flush to centre divider
                    RowLayout {
                        Layout.fillWidth:       true
                        Layout.preferredHeight: mixer.eqRowH
                        Layout.minimumHeight:   mixer.eqRowH
                        Layout.maximumHeight:   mixer.eqRowH
                        spacing: 0

                        Item { Layout.fillWidth: true }  // spacer → EQ sits at right edge

                        // VU A – left of EQ knobs
                        VuMeterVertical {
                            Layout.fillHeight:    true
                            Layout.preferredWidth: mixer.vuW
                            levelLinear: mixer.vuACombined
                            deckName: "A"
                        }

                        Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                        // EQ knobs A
                        ColumnLayout {
                            spacing: 0
                            Layout.preferredHeight: mixer.eqRowH
                            Layout.minimumHeight:   mixer.eqRowH
                            Layout.maximumHeight:   mixer.eqRowH

                            KnobCell {
                                label: "H"; size: 22
                                Layout.alignment: Qt.AlignHCenter
                                knob.from: -1; knob.to: 1; knob.value: 0
                                knob.accentColor: mixer.clrAKnob
                                knob.onValueChanged: { if (engineA) engineA.eqHigh = knob.value }
                            }
                            KnobCell {
                                label: "M"; size: 22
                                Layout.alignment: Qt.AlignHCenter
                                knob.from: -1; knob.to: 1; knob.value: 0
                                knob.accentColor: mixer.clrAKnob
                                knob.onValueChanged: { if (engineA) engineA.eqMid = knob.value }
                            }
                            KnobCell {
                                label: "L"; size: 22
                                Layout.alignment: Qt.AlignHCenter
                                knob.from: -1; knob.to: 1; knob.value: 0
                                knob.accentColor: mixer.clrAKnob
                                knob.onValueChanged: { if (engineA) engineA.eqLow = knob.value }
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

                    MixerSlider {
                        id: volFaderA
                        Layout.fillHeight: true
                        Layout.fillWidth:  true
                        orientation: Qt.Vertical
                        from: 0.0; to: 1.0; value: 1.0
                        onValueChanged: {
                            if (parameterStore && parameterStore.getParameter(mixer.channelAId + "_vol") !== value)
                                parameterStore.setParameter(mixer.channelAId + "_vol", value)
                        }
                        TapHandler {
                            onDoubleTapped: { volFaderA.enabled = false; volFaderA.value = 1.0; volFaderA.enabled = true }
                        }
                    }
                }
            }
        }

        // Center divider
        Rectangle { width: 1; Layout.fillHeight: true; color: "#333333" }

        // ════════════════════════════════════════════════════════════════
        // DECK B (right side – mirrored)
        // ════════════════════════════════════════════════════════════════
        ColumnLayout {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            spacing: 0

            // Header
            Rectangle {
                Layout.fillWidth:      true
                Layout.preferredHeight: mixer.labelH
                color: "#080808"

                Rectangle { anchors.right: parent.right; width: 3; height: parent.height; color: mixer.clrB; opacity: 0.85 }
                Text {
                    anchors.centerIn:   parent
                    text:               "B"
                    color:              mixer.clrB
                    font.pixelSize:     window.spViewport(10)
                    font.bold:          true
                    font.family:        "monospace"
                    font.letterSpacing: 2.0
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
            }

            // Content row: [EQ area + vol fader] | [outer controls]
            RowLayout {
                Layout.fillWidth:  true
                Layout.fillHeight: true
                spacing: 0

                // ── Center B (EQ H/M/L | VU | Vol fader) ─────────────────
                ColumnLayout {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    spacing: 0

                    // EQ + VU row – locked height, EQ flush to centre divider
                    RowLayout {
                        Layout.fillWidth:       true
                        Layout.preferredHeight: mixer.eqRowH
                        Layout.minimumHeight:   mixer.eqRowH
                        Layout.maximumHeight:   mixer.eqRowH
                        spacing: 0

                        // EQ knobs B
                        ColumnLayout {
                            spacing: 0
                            Layout.preferredHeight: mixer.eqRowH
                            Layout.minimumHeight:   mixer.eqRowH
                            Layout.maximumHeight:   mixer.eqRowH

                            KnobCell {
                                label: "H"; size: 22
                                Layout.alignment: Qt.AlignHCenter
                                knob.from: -1; knob.to: 1; knob.value: 0
                                knob.accentColor: mixer.clrBKnob
                                knob.onValueChanged: { if (engineB) engineB.eqHigh = knob.value }
                            }
                            KnobCell {
                                label: "M"; size: 22
                                Layout.alignment: Qt.AlignHCenter
                                knob.from: -1; knob.to: 1; knob.value: 0
                                knob.accentColor: mixer.clrBKnob
                                knob.onValueChanged: { if (engineB) engineB.eqMid = knob.value }
                            }
                            KnobCell {
                                label: "L"; size: 22
                                Layout.alignment: Qt.AlignHCenter
                                knob.from: -1; knob.to: 1; knob.value: 0
                                knob.accentColor: mixer.clrBKnob
                                knob.onValueChanged: { if (engineB) engineB.eqLow = knob.value }
                            }
                        }

                        Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                        // VU B – right of EQ knobs
                        VuMeterVertical {
                            Layout.fillHeight:    true
                            Layout.preferredWidth: mixer.vuW
                            levelLinear: mixer.vuBCombined
                            deckName: "B"
                        }

                        Item { Layout.fillWidth: true }  // spacer → EQ sits at left edge
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

                    MixerSlider {
                        id: volFaderB
                        Layout.fillHeight: true
                        Layout.fillWidth:  true
                        orientation: Qt.Vertical
                        from: 0.0; to: 1.0; value: 1.0
                        onValueChanged: {
                            if (parameterStore && parameterStore.getParameter(mixer.channelBId + "_vol") !== value)
                                parameterStore.setParameter(mixer.channelBId + "_vol", value)
                        }
                        TapHandler {
                            onDoubleTapped: { volFaderB.enabled = false; volFaderB.value = 1.0; volFaderB.enabled = true }
                        }
                    }
                }

                Rectangle { width: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                // ── Outer B (GAIN / SC / CUE) ─────────────────────────
                ColumnLayout {
                    Layout.preferredWidth: mixer.outerW
                    Layout.minimumWidth:   mixer.outerW
                    Layout.maximumWidth:   mixer.outerW
                    Layout.fillHeight:     true
                    Layout.alignment:      Qt.AlignTop
                    spacing: 0

                    KnobCell {
                        label: "GAIN"; size: mixer.outerKnob
                        Layout.alignment: Qt.AlignHCenter
                        knob.from: 0; knob.to: 2; knob.value: 1.0
                        knob.defaultValue: 1.0
                        knob.accentColor: mixer.clrBKnob
                        knob.onValueChanged: { if (engineB) engineB.trim = knob.value }
                    }

                    KnobCell {
                        label: "SC"; size: mixer.outerKnob
                        Layout.alignment: Qt.AlignHCenter
                        knob.from: -1; knob.to: 1; knob.value: 0.0; knob.defaultValue: 0.0
                        knob.accentColor: Qt.darker(mixer.clrB, 1.6)
                        knob.onValueChanged: {
                            if (typeof fxManager !== "undefined") {
                                fxManager.setSoundColorDeck(2, knob.value)
                                if (fxManager.soundColorMode === "Filter") { if (engineB) engineB.filter = knob.value }
                                else { if (engineB) engineB.filter = 0.0 }
                            } else { if (engineB) engineB.filter = knob.value }
                        }
                    }

                    // CUE button
                    Rectangle {
                        Layout.fillWidth:      true
                        Layout.preferredHeight: mixer.btnH
                        color:        mixer.cueBActive ? "#0c2016" : "#141414"
                        border.width: 1
                        border.color: mixer.cueBActive ? "#1e5030" : "#1c1c1c"

                        HoverHandler { id: cueBHov }
                        Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: cueBHov.hovered && !mixer.cueBActive ? 0.04 : 0 }

                        Text {
                            anchors.centerIn: parent; text: "CUE"
                            color:          mixer.cueBActive ? "#4dd98a" : "#555555"
                            font.pixelSize: window.spViewport(8)
                            font.bold:      true; font.family: "monospace"; font.letterSpacing: 0.6
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 2; color: "#4dd98a"; visible: mixer.cueBActive
                        }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                mixer.cueBActive = !mixer.cueBActive
                                if (engineB) engineB.cueEnabled = mixer.cueBActive
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
