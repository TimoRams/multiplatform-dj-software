import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

Rectangle {
    id: root

    color: UiTheme.panel
    height: window.fxBarHeight

    component FxDarkBtn: Rectangle {
        id: db
        required property string label
        property bool  active:   false
        required property color accent
        property bool  isHeader: false   // section-header style (no click)

        implicitWidth:  28
        implicitHeight: 18
        radius: 0
        color: active ? UiTheme.panelRaised : UiTheme.panelDeep

        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            height: 1
            color: db.active ? db.accent : (db.isHeader ? UiTheme.separator : UiTheme.borderSubtle)
        }

        Text {
            anchors.centerIn: parent
            text:           db.label
            color:          db.active ? db.accent : (db.isHeader ? UiTheme.textDim : UiTheme.textMuted)
            font.pixelSize: 8
            font.bold:      db.active
            font.family:    UiTheme.uiFontFamily
        }
    }

    component FxAssignBtn: Rectangle {
        id: ab
        required property string label
        property bool  active: false
        required property color accent

        implicitWidth:  26
        implicitHeight: 22
        radius: 0
        color:  active ? UiTheme.panelRaised : UiTheme.panelDeep

        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            height: 1
            color:  ab.active ? ab.accent : UiTheme.borderSubtle
        }

        Text {
            anchors.centerIn: parent
            text:           ab.label
            color:          ab.active ? ab.accent : UiTheme.textDim
            font.pixelSize: 10
            font.bold:      ab.active
            font.family:    UiTheme.uiFontFamily
        }

        HoverHandler { id: abHov }
        Rectangle { anchors.fill: parent; color: UiTheme.textPrimary; opacity: abHov.hovered ? 0.03 : 0 }

        MouseArea {
            anchors.fill: parent
            cursorShape:  Qt.PointingHandCursor
            onClicked:    ab.active = !ab.active
        }
    }

    component FxUnit: Rectangle {
        id: root
        objectName: "fxUnit" + unitId

        property int   unitId:      1
        property bool  deck1Active: btnDeck1.active
        property bool  deck2Active: btnDeck2.active
        property alias wetDry:      mixKnob.value
        property color accentColor: unitId === 1 ? UiTheme.deckB : UiTheme.deckA

        signal deck1Toggled(bool active)
        signal deck2Toggled(bool active)

        color: UiTheme.panelDeep

        // ── Beat divisions ────────────────────────────────────────────────────
        readonly property var kDivValues: [0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0]
        readonly property var kDivLabels: ["1/16", "1/8", "1/4", "1/2", "1", "2", "4"]

        // ── Room size steps (Reverb) ──────────────────────────────────────────
        readonly property var kRoomValues: [0.10, 0.20, 0.30, 0.50, 0.75, 1.00]
        readonly property var kRoomLabels: ["10%", "20%", "30%", "50%", "75%", "100%"]

        // ── Effect metadata ───────────────────────────────────────────────────
        // paramType:
        //   "beatDiv"  → beat-division selector (timing effects, LFO effects, loops)
        //   "percent"  → discrete percentage steps (Reverb room size)
        //   "none"     → no top parameter (knob alone)
        // knobLabel: label shown above the large knob
        // bpmSync: whether SYNC button and BPM display are relevant
        readonly property var effectsList: [
            { name: "---",             paramType: "none",    knobLabel: "MIX",   bpmSync: false, wip: false },
            // ── Delay / echo ─────────────────────────────────────────────────
            { name: "Echo",            paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
            { name: "Low Cut Echo",    paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
            { name: "Multi-Tap Delay", paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
            // ── Loop / stutter ───────────────────────────────────────────────
            { name: "Roll",            paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
            { name: "Roll Out",        paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
            { name: "Slip Roll",       paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
            { name: "Mobius Saw",      paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
            { name: "Mobius Tri",      paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
            // ── LFO-rate (BPM-sync sets sweep rate) ──────────────────────────
            { name: "Tremolo",         paramType: "beatDiv", knobLabel: "DEPTH", bpmSync: true,  wip: false },
            { name: "Flanger",         paramType: "beatDiv", knobLabel: "DEPTH", bpmSync: true,  wip: false },
            { name: "Phaser",          paramType: "beatDiv", knobLabel: "DEPTH", bpmSync: true,  wip: false },
            { name: "Spiral",          paramType: "beatDiv", knobLabel: "DEPTH", bpmSync: true,  wip: false },
            { name: "Enigma Jet",      paramType: "beatDiv", knobLabel: "DEPTH", bpmSync: true,  wip: false },
            // ── Character / no timing ─────────────────────────────────────────
            { name: "Reverb",          paramType: "percent", knobLabel: "MIX",   bpmSync: false, wip: false },
            { name: "Bitcrusher",      paramType: "none",    knobLabel: "CRUSH", bpmSync: false, wip: false },
            { name: "Stretch",         paramType: "none",    knobLabel: "TIME",  bpmSync: false, wip: false },
            // ── Work in progress ─────────────────────────────────────────────
            { name: "Pitch Shifter",   paramType: "none",    knobLabel: "PITCH", bpmSync: false, wip: true  }
        ]

        // ── Active effect metadata ────────────────────────────────────────────
        readonly property var currentEffect: (effectCombo.currentIndex >= 0 && effectCombo.currentIndex < effectsList.length)
            ? effectsList[effectCombo.currentIndex]
            : effectsList[0]
        readonly property string paramType: currentEffect.paramType
        readonly property string knobLabel: currentEffect.knobLabel
        readonly property bool   hasBpmSync: currentEffect.bpmSync

        // ── Sync state (mirrors fxManager properties) ────────────────────────
        readonly property bool   syncOn:   unitId === 1
            ? (fxManager != null ? fxManager.syncEnabled1 : false)
            : (fxManager != null ? fxManager.syncEnabled2 : false)
        readonly property real   activeDiv: unitId === 1
            ? (fxManager != null ? fxManager.beatDiv1 : 0.25)
            : (fxManager != null ? fxManager.beatDiv2 : 0.25)
        readonly property double deckBpm:  unitId === 1
            ? (fxManager != null ? fxManager.displayBpm1 : 0.0)
            : (fxManager != null ? fxManager.displayBpm2 : 0.0)

        // ── Room-size primary param (Reverb) ──────────────────────────────────
        // Stored in QML; pushed to fxManager when changed.
        property real activePrimaryParam: unitId === 1
            ? (fxManager != null ? fxManager.primaryParam1 : 0.5)
            : (fxManager != null ? fxManager.primaryParam2 : 0.5)

        // ═════════════════════════════════════════════════════════════════════
        ColumnLayout {
            anchors.fill:         parent
            anchors.leftMargin:   8
            anchors.rightMargin:  8
            anchors.topMargin:    4
            anchors.bottomMargin: 4
            spacing: 3

            // ── Row 1: identity, deck assignment, effect selector, knob ───────
            RowLayout {
                Layout.fillWidth: true
                spacing: 5

                // FX unit label
                Text {
                    text:             "FX" + root.unitId
                    color:            root.accentColor
                    font.pixelSize:   9
                    font.bold:        true
                    font.family:      UiTheme.uiFontFamily
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 22
                    opacity: 0.7
                }

                FxAssignBtn {
                    id: btnDeck1
                    label:  "1"
                    accent: root.accentColor
                    Layout.alignment: Qt.AlignVCenter
                    onActiveChanged: {
                        root.deck1Toggled(active)
                        if (fxManager != null)
                            fxManager.setDeckAssignment(root.unitId, 1, active)
                    }
                }
                FxAssignBtn {
                    id: btnDeck2
                    label:  "2"
                    accent: root.accentColor
                    Layout.alignment: Qt.AlignVCenter
                    onActiveChanged: {
                        root.deck2Toggled(active)
                        if (fxManager != null)
                            fxManager.setDeckAssignment(root.unitId, 2, active)
                    }
                }

                // Effect selector combo
                ComboBox {
                    id: effectCombo
                    objectName: "fxEffectCombo"
                    model: root.effectsList.length
                    Layout.fillWidth:       true
                    Layout.preferredHeight: 22
                    Layout.alignment:       Qt.AlignVCenter

                    contentItem: Text {
                        leftPadding:       6
                        rightPadding:      16
                        text: {
                            if (effectCombo.currentIndex < 0) return "---"
                            const e = root.effectsList[effectCombo.currentIndex]
                            return e.wip ? e.name + " ·WIP" : e.name
                        }
                        color: {
                            if (effectCombo.currentIndex < 0) return UiTheme.textDim
                            return root.effectsList[effectCombo.currentIndex].wip ? UiTheme.textMuted : UiTheme.textPrimary
                        }
                        font.pixelSize:    10
                        font.family:       UiTheme.uiFontFamily
                        verticalAlignment: Text.AlignVCenter
                        elide:             Text.ElideRight
                    }

                    indicator: Canvas {
                        x: effectCombo.width - width - 6
                        y: effectCombo.topPadding + (effectCombo.availableHeight - height) / 2
                        width: 7; height: 5
                        contextType: "2d"
                        onPaint: {
                            context.reset()
                            context.moveTo(0, 0); context.lineTo(width, 0)
                            context.lineTo(width / 2, height); context.closePath()
                            context.fillStyle = UiTheme.textDim; context.fill()
                        }
                    }

                    background: Rectangle {
                        color:  effectCombo.pressed ? UiTheme.panelRaised : UiTheme.panelDeep
                        radius: 0
                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 1
                            color:  effectCombo.visualFocus ? root.accentColor : UiTheme.borderSubtle
                        }
                    }

                    delegate: ItemDelegate {
                        readonly property var entry: root.effectsList[index]
                        width:       effectCombo.width
                        height:      22
                        enabled:     !entry.wip
                        highlighted: effectCombo.highlightedIndex === index

                        contentItem: RowLayout {
                            spacing: 4
                            Text {
                                Layout.fillWidth:  true
                                text:              entry.name
                                color: entry.wip ? UiTheme.textMuted : (highlighted ? UiTheme.textPrimary : UiTheme.textSecondary)
                                font.pixelSize:    10
                                font.family:       UiTheme.uiFontFamily
                                leftPadding:       8
                                verticalAlignment: Text.AlignVCenter
                            }
                            // WIP badge
                            Rectangle {
                                visible:           entry.wip
                                width: 28; height: 13; radius: 0
                                color:             UiTheme.panelInset
                                Layout.rightMargin: 6
                                Text {
                                    anchors.centerIn: parent
                                    text: "WIP"; color: UiTheme.textMuted
                                    font.pixelSize: 7; font.bold: true; font.family: UiTheme.uiFontFamily
                                }
                            }
                        }

                        background: Rectangle {
                            color:  (highlighted && !entry.wip) ? UiTheme.panelRaised : UiTheme.panelDeep
                            radius: 0
                            Rectangle {
                                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                                height: 1
                                color: (highlighted && !entry.wip) ? root.accentColor : UiTheme.borderSubtle
                            }
                        }
                    }

                    popup.background: Rectangle {
                        color: UiTheme.panelDeep; radius: 0
                        Rectangle {
                            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                            height: 1; color: UiTheme.separatorSubtle
                        }
                    }

                    onCurrentIndexChanged: {
                        if (currentIndex < 0) return
                        const e = root.effectsList[currentIndex]
                        if (e.wip) { currentIndex = 0; return }
                        if (fxManager != null)
                            fxManager.setEffectType(root.unitId, e.name)
                    }
                }

                // ── Knob + value display ─────────────────────────────────────
                Column {
                    Layout.preferredWidth:  52
                    Layout.alignment:       Qt.AlignVCenter
                    spacing: 1

                    // Label + live value on same line
                    RowLayout {
                        width: parent.width
                        spacing: 0
                        Text {
                            text:           root.knobLabel
                            color:          UiTheme.textMuted
                            font.pixelSize: 8
                            font.family:    UiTheme.uiFontFamily
                            Layout.fillWidth: true
                        }
                        Text {
                            text:           Math.round(mixKnob.value * 100) + "%"
                            color:          mixKnob.value > 0.02 ? UiTheme.textSecondary : UiTheme.textMuted
                            font.pixelSize: 8
                            font.family:    UiTheme.numericFontFamily
                        }
                    }

                    Knob {
                        id: mixKnob
                        anchors.horizontalCenter: parent.horizontalCenter
                        width:        22
                        height:       22
                        from:         0.0
                        to:           1.0
                        value:        0.0
                        stepSize:     0.01
                        accentColor:  root.accentColor
                        defaultValue: 0.0

                        onValueChanged: {
                            if (fxManager == null)
                                return
                            fxManager.setWetDry(root.unitId, value)
                            // This knob is the only engage control on the strip, so
                            // it has to say so explicitly: the mix amount alone
                            // never engages a unit, otherwise a hardware knob
                            // reporting its resting position switches FX on unasked.
                            if (value > 0.001 && !fxManager.unitEnabled(root.unitId))
                                fxManager.setUnitEnabled(root.unitId, true)
                        }
                    }
                }
            }

            // ── Row 2: BPM + SYNC (context-sensitive) + primary parameter ─────
            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                // ── BPM readout (only shown when effect supports BPM sync) ────
                Rectangle {
                    Layout.preferredWidth:  50
                    Layout.preferredHeight: 18
                    visible: root.hasBpmSync
                    color: UiTheme.displayBackground
                    radius: 0

                    Rectangle {
                        anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                        height: 1
                        color: root.deckBpm > 0 ? UiTheme.separator : UiTheme.borderSubtle
                    }

                    Text {
                        anchors.centerIn: parent
                        text:           root.deckBpm > 0 ? root.deckBpm.toFixed(1) : "---"
                        color:          root.deckBpm > 0 ? UiTheme.textSecondary : UiTheme.textMuted
                        font.pixelSize: 9
                        font.family:    UiTheme.numericFontFamily
                    }
                }

                // ── SYNC toggle (only when effect supports BPM sync) ──────────
                Rectangle {
                    Layout.preferredWidth:  32
                    Layout.preferredHeight: 18
                    visible: root.hasBpmSync
                    radius: 0
                    color: root.syncOn ? UiTheme.greenDim : UiTheme.panelDeep

                    Rectangle {
                        anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                        height: 1
                        color: root.syncOn ? UiTheme.green : UiTheme.borderSubtle
                    }

                    Text {
                        anchors.centerIn: parent
                        text:           "SYNC"
                        color:          root.syncOn ? UiTheme.green : UiTheme.textMuted
                        font.pixelSize: 8
                        font.bold:      root.syncOn
                        font.family:    UiTheme.uiFontFamily
                    }

                    HoverHandler { id: syncHov }
                    Rectangle { anchors.fill: parent; color: UiTheme.textPrimary; opacity: syncHov.hovered ? 0.04 : 0 }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape:  Qt.PointingHandCursor
                        onClicked:
                            if (fxManager != null)
                                fxManager.setSyncEnabled(root.unitId, !root.syncOn)
                    }
                }

                // ── Primary parameter area (fills remaining width) ────────────
                Item {
                    Layout.fillWidth:       true
                    Layout.preferredHeight: 18

                    // ── beatDiv: 7 beat-division buttons ─────────────────────
                    RowLayout {
                        anchors.fill: parent
                        spacing: 2
                        visible: root.paramType === "beatDiv"

                        Repeater {
                            model: root.kDivLabels
                            delegate: Rectangle {
                                readonly property real divVal:   root.kDivValues[index]
                                readonly property bool isActive: root.syncOn
                                    && Math.abs(root.activeDiv - divVal) < 0.001

                                Layout.fillWidth:       true
                                Layout.preferredHeight: 18
                                radius: 0
                                color: isActive ? UiTheme.panelRaised : UiTheme.panelDeep

                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                                    height: 1
                                    color: isActive ? root.accentColor : UiTheme.borderSubtle
                                }

                                Text {
                                    anchors.centerIn: parent
                                    text:           modelData
                                    color:          isActive ? root.accentColor : UiTheme.textMuted
                                    font.pixelSize: 8
                                    font.bold:      isActive
                                    font.family:    UiTheme.numericFontFamily
                                }

                                HoverHandler { id: divHov }
                                Rectangle { anchors.fill: parent; color: UiTheme.textPrimary; opacity: divHov.hovered ? 0.04 : 0 }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked: {
                                        if (fxManager != null) {
                                            fxManager.setBeatDivision(root.unitId, divVal)
                                            if (!root.syncOn)
                                                fxManager.setSyncEnabled(root.unitId, true)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ── percent: room-size percentage buttons ─────────────────
                    RowLayout {
                        anchors.fill: parent
                        spacing: 2
                        visible: root.paramType === "percent"

                        Repeater {
                            model: root.kRoomLabels
                            delegate: Rectangle {
                                readonly property real roomVal:  root.kRoomValues[index]
                                readonly property bool isActive: Math.abs(root.activePrimaryParam - roomVal) < 0.01

                                Layout.fillWidth:       true
                                Layout.preferredHeight: 18
                                radius: 0
                                color: isActive ? UiTheme.panelRaised : UiTheme.panelDeep

                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                                    height: 1
                                    color: isActive ? root.accentColor : UiTheme.borderSubtle
                                }

                                Text {
                                    anchors.centerIn: parent
                                    text:           modelData
                                    color:          isActive ? root.accentColor : UiTheme.textMuted
                                    font.pixelSize: 8
                                    font.bold:      isActive
                                    font.family:    UiTheme.numericFontFamily
                                }

                                HoverHandler { id: roomHov }
                                Rectangle { anchors.fill: parent; color: UiTheme.textPrimary; opacity: roomHov.hovered ? 0.04 : 0 }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked: {
                                        root.activePrimaryParam = roomVal
                                        if (fxManager != null)
                                            fxManager.setPrimaryParam(root.unitId, roomVal)
                                    }
                                }
                            }
                        }
                    }

                    // ── none: knob-only hint label ────────────────────────────
                    Item {
                        anchors.fill: parent
                        visible: root.paramType === "none"

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            text:    root.currentEffect.name !== "---"
                                ? root.knobLabel + " controlled by knob"
                                : ""
                            color:          UiTheme.textMuted
                            font.pixelSize: 8
                            font.family:    UiTheme.uiFontFamily
                            font.italic:    true
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: UiTheme.separatorSubtle
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        FxUnit {
            id: fxUnit1
            unitId:      1
            accentColor: UiTheme.deckB
            Layout.fillWidth:  true
            Layout.fillHeight: true
        }

        Rectangle { width: 1; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        // ── Sound Color panel ─────────────────────────────────────────────
        Rectangle {
            id: scPanel
            color: UiTheme.panel
            Layout.preferredWidth: 248
            Layout.fillHeight:     true

            property string fallbackMode:  "Filter"
            property real   fallbackParam: 0.5
            readonly property var modes: ["Space", "D.Echo", "Crush", "Pitch", "Noise", "Sweep", "Filter"]

            // Per-mode label for the param knob — shown below the knob
            readonly property var paramLabels: ({
                "Space":  "SIZE",
                "D.Echo": "TIME",
                "Crush":  "DEPTH",
                "Pitch":  "RANGE",
                "Noise":  "LEVEL",
                "Sweep":  "RESO",
                "Filter": "RESO"
            })

            readonly property string activeMode: {
                if (typeof fxManager !== "undefined" && fxManager !== null)
                    return fxManager.soundColorMode
                return fallbackMode
            }

            function isActiveMode(name) { return scPanel.activeMode === name }

            Connections {
                target: (typeof fxManager !== "undefined" && fxManager !== null) ? fxManager : null
                function onSoundColorModeChanged()  { scPanel.fallbackMode  = fxManager.soundColorMode }
                function onSoundColorParamChanged() { scPanel.fallbackParam = fxManager.soundColorParam }
            }

            Component.onCompleted: {
                if (typeof fxManager !== "undefined" && fxManager !== null) {
                    fallbackMode  = fxManager.soundColorMode
                    fallbackParam = fxManager.soundColorParam
                }
            }

            ColumnLayout {
                anchors.fill:         parent
                anchors.topMargin:    5
                anchors.bottomMargin: 5
                anchors.leftMargin:   6
                anchors.rightMargin:  6
                spacing: 4

                // Row 1: Section label
                Text {
                    Layout.fillWidth: true
                    text:           "COLOR FX"
                    color:          UiTheme.textLabel
                    font.pixelSize: 7
                    font.family:    UiTheme.uiFontFamily
                    font.letterSpacing: 1
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    // Param knob with mode-specific label
                    Column {
                        Layout.preferredWidth: 32
                        Layout.alignment:      Qt.AlignVCenter
                        spacing: 2

                        Knob {
                            id: scKnob
                            anchors.horizontalCenter: parent.horizontalCenter
                            width:        24
                            height:       24
                            from:         0.0
                            to:           1.0
                            stepSize:     0.01
                            value:        scPanel.fallbackParam
                            accentColor:  UiTheme.textSecondary
                            defaultValue: 0.5

                            onValueChanged: {
                                scPanel.fallbackParam = value
                                if (typeof fxManager !== "undefined" && fxManager !== null)
                                    fxManager.setSoundColorParam(value)
                            }
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text:           scPanel.paramLabels[scPanel.activeMode] ?? "PARAM"
                            color:          UiTheme.textMuted
                            font.pixelSize: 7
                            font.family:    UiTheme.uiFontFamily
                        }
                    }

                    // Mode buttons — 4 columns, 2 rows
                    Grid {
                        Layout.fillWidth: true
                        columns: 4
                        rowSpacing: 2
                        columnSpacing: 2

                        Repeater {
                            model: scPanel.modes
                            delegate: Rectangle {
                                readonly property bool isActive: scPanel.isActiveMode(modelData)

                                width:  Math.floor((scPanel.width - 44 - 5 * 3) / 4)
                                height: 20
                                radius: 0
                                color: isActive ? UiTheme.panelRaised : UiTheme.panelDeep

                                // Active accent bar at top
                                Rectangle {
                                    anchors.top:   parent.top
                                    anchors.left:  parent.left
                                    anchors.right: parent.right
                                    height: isActive ? 2 : 1
                                    color:  isActive ? UiTheme.textSecondary : UiTheme.borderSubtle
                                }

                                Text {
                                    anchors.centerIn: parent
                                    text:           modelData
                                    font.pixelSize: 8
                                    font.bold:      isActive
                                    font.family:    UiTheme.uiFontFamily
                                    color:          isActive ? UiTheme.textPrimary : UiTheme.textDim
                                    elide:          Text.ElideRight
                                }

                                HoverHandler { id: modeHov }
                                Rectangle { anchors.fill: parent; radius: parent.radius; color: UiTheme.textPrimary; opacity: modeHov.hovered ? 0.04 : 0 }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked: {
                                        scPanel.fallbackMode = modelData
                                        if (typeof fxManager !== "undefined")
                                            fxManager.setSoundColorMode(modelData)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { width: 1; Layout.fillHeight: true; color: UiTheme.separatorSubtle }

        FxUnit {
            id: fxUnit2
            unitId:      2
            accentColor: UiTheme.deckA
            Layout.fillWidth:  true
            Layout.fillHeight: true
        }
    }
}
