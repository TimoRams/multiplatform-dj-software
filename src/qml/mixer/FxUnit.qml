import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: root

    property int   unitId:      1
    property bool  deck1Active: btnDeck1.active
    property bool  deck2Active: btnDeck2.active
    property alias wetDry:      mixKnob.value
    property color accentColor: unitId === 1 ? "#1e90ff" : "#ff6a00"

    signal deck1Toggled(bool active)
    signal deck2Toggled(bool active)

    color: "#181818"

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
        { name: "Mobius",          paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
        { name: "Nobius",          paramType: "beatDiv", knobLabel: "MIX",   bpmSync: true,  wip: false },
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

    // ── Small dark button shared component ────────────────────────────────
    component DarkBtn: Rectangle {
        id: db
        required property string label
        property bool  active:   false
        property color accent:   root.accentColor
        property bool  isHeader: false   // section-header style (no click)

        implicitWidth:  28
        implicitHeight: 18
        radius: 0
        color: active ? "#1e1e2e" : "#181818"

        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            height: 1
            color: db.active ? db.accent : (db.isHeader ? "#333333" : "#222222")
        }

        Text {
            anchors.centerIn: parent
            text:           db.label
            color:          db.active ? db.accent : (db.isHeader ? "#666666" : "#555555")
            font.pixelSize: 8
            font.bold:      db.active
            font.family:    "monospace"
        }
    }

    // ── Deck assign toggle ────────────────────────────────────────────────
    component AssignBtn: Rectangle {
        id: ab
        required property string label
        property bool  active: false
        property color accent: root.accentColor

        implicitWidth:  26
        implicitHeight: 22
        radius: 0
        color:  active ? "#222222" : "#1e1e1e"

        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            height: 1
            color:  ab.active ? ab.accent : "#333333"
        }

        Text {
            anchors.centerIn: parent
            text:           ab.label
            color:          ab.active ? ab.accent : "#666666"
            font.pixelSize: 10
            font.bold:      ab.active
            font.family:    "monospace"
        }

        HoverHandler { id: abHov }
        Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: abHov.hovered ? 0.03 : 0 }

        MouseArea {
            anchors.fill: parent
            cursorShape:  Qt.PointingHandCursor
            onClicked:    ab.active = !ab.active
        }
    }

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
                font.family:      "monospace"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 22
                opacity: 0.7
            }

            AssignBtn {
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
            AssignBtn {
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
                        if (effectCombo.currentIndex < 0) return "#666666"
                        return root.effectsList[effectCombo.currentIndex].wip ? "#555555" : "#e8e8e8"
                    }
                    font.pixelSize:    10
                    font.family:       "monospace"
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
                        context.fillStyle = "#666666"; context.fill()
                    }
                }

                background: Rectangle {
                    color:  effectCombo.pressed ? "#2d2d2d" : "#1e1e1e"
                    radius: 0
                    Rectangle {
                        anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                        height: 1
                        color:  effectCombo.visualFocus ? root.accentColor : "#333333"
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
                            color: entry.wip ? "#3a3a3a" : (highlighted ? "#e8e8e8" : "#999999")
                            font.pixelSize:    10
                            font.family:       "monospace"
                            leftPadding:       8
                            verticalAlignment: Text.AlignVCenter
                        }
                        // WIP badge
                        Rectangle {
                            visible:           entry.wip
                            width: 28; height: 13; radius: 2
                            color:             "#1a1000"
                            Layout.rightMargin: 6
                            Text {
                                anchors.centerIn: parent
                                text: "WIP"; color: "#664400"
                                font.pixelSize: 7; font.bold: true; font.family: "monospace"
                            }
                        }
                    }

                    background: Rectangle {
                        color:  (highlighted && !entry.wip) ? "#252525" : "#181818"
                        radius: 0
                        Rectangle {
                            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                            height: 1
                            color: (highlighted && !entry.wip) ? root.accentColor : "#1c1c1c"
                        }
                    }
                }

                popup.background: Rectangle {
                    color: "#181818"; radius: 0
                    Rectangle {
                        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                        height: 1; color: "#333333"
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
                        color:          "#555555"
                        font.pixelSize: 8
                        font.family:    "monospace"
                        Layout.fillWidth: true
                    }
                    Text {
                        text:           Math.round(mixKnob.value * 100) + "%"
                        color:          mixKnob.value > 0.02 ? "#888888" : "#333333"
                        font.pixelSize: 8
                        font.family:    "monospace"
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
                        if (fxManager != null)
                            fxManager.setWetDry(root.unitId, value)
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
                color: "#111111"
                radius: 0

                Rectangle {
                    anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                    height: 1
                    color: root.deckBpm > 0 ? "#3a3a3a" : "#1e1e1e"
                }

                Text {
                    anchors.centerIn: parent
                    text:           root.deckBpm > 0 ? root.deckBpm.toFixed(1) : "---"
                    color:          root.deckBpm > 0 ? "#888888" : "#333333"
                    font.pixelSize: 9
                    font.family:    "monospace"
                }
            }

            // ── SYNC toggle (only when effect supports BPM sync) ──────────
            Rectangle {
                Layout.preferredWidth:  32
                Layout.preferredHeight: 18
                visible: root.hasBpmSync
                radius: 0
                color: root.syncOn ? "#152015" : "#1e1e1e"

                Rectangle {
                    anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                    height: 1
                    color: root.syncOn ? "#3acc3a" : "#2a2a2a"
                }

                Text {
                    anchors.centerIn: parent
                    text:           "SYNC"
                    color:          root.syncOn ? "#3acc3a" : "#444444"
                    font.pixelSize: 8
                    font.bold:      root.syncOn
                    font.family:    "monospace"
                }

                HoverHandler { id: syncHov }
                Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: syncHov.hovered ? 0.04 : 0 }

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
                            color: isActive ? "#1a1a2a" : "#181818"

                            Rectangle {
                                anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                                height: 1
                                color: isActive ? root.accentColor : "#222222"
                            }

                            Text {
                                anchors.centerIn: parent
                                text:           modelData
                                color:          isActive ? root.accentColor : "#484848"
                                font.pixelSize: 8
                                font.bold:      isActive
                                font.family:    "monospace"
                            }

                            HoverHandler { id: divHov }
                            Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: divHov.hovered ? 0.04 : 0 }

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
                            color: isActive ? "#1a1a2a" : "#181818"

                            Rectangle {
                                anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                                height: 1
                                color: isActive ? root.accentColor : "#222222"
                            }

                            Text {
                                anchors.centerIn: parent
                                text:           modelData
                                color:          isActive ? root.accentColor : "#484848"
                                font.pixelSize: 8
                                font.bold:      isActive
                                font.family:    "monospace"
                            }

                            HoverHandler { id: roomHov }
                            Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: roomHov.hovered ? 0.04 : 0 }

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
                        color:          "#333333"
                        font.pixelSize: 8
                        font.family:    "monospace"
                        font.italic:    true
                    }
                }
            }
        }
    }
}
