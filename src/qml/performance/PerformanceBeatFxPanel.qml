import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Beat FX column in the layout used by professional DJ players: section
// headers, then flat rows carrying one value each in large type. The controls
// read top to bottom in the order they are used — tempo source, effect, beat
// division, resulting time, mix amount, routing.
Rectangle {
    id: root
    property var fx: null
    signal closeRequested()

    readonly property int headerHeight: 28
    readonly property color panelText: "#ECEFF1"
    readonly property color mutedText: "#7D858B"
    readonly property color lineColor: "#2C3237"
    readonly property color rowColor: "#1B1F23"
    readonly property color activeColor: "#168FC4"
    readonly property color accentColor: "#E99128"

    readonly property var divisions: [
        { label: "1/16", value: 0.0625 }, { label: "1/8", value: 0.125 },
        { label: "1/4", value: 0.25 }, { label: "1/2", value: 0.5 },
        { label: "1", value: 1.0 }, { label: "2", value: 2.0 }, { label: "4", value: 4.0 }
    ]
    readonly property real currentDiv: fx ? fx.beatDiv1 : 0.25
    readonly property int currentDivIndex: {
        var index = 0
        var nearest = Number.MAX_VALUE
        for (var i = 0; i < divisions.length; ++i) {
            var distance = Math.abs(currentDiv - divisions[i].value)
            if (distance < nearest) { nearest = distance; index = i }
        }
        return index
    }
    readonly property real effectMs: {
        var bpm = fx && fx.displayBpm1 > 0 ? fx.displayBpm1 : 0
        return bpm > 0 ? divisions[currentDivIndex].value * 60000 / bpm : 0
    }
    readonly property bool routedA: fx ? fx.deck1A : false
    readonly property bool routedB: fx ? fx.deck1B : false
    // The unit's own engage flag, not a guess derived from the mix amount.
    // Reading it off wetDry1 meant a mix of zero — the resting position of the
    // hardware LEVEL knob — looked identical to "off", so pressing ON appeared
    // to do nothing.
    readonly property bool effectOn: fx && fx.effectType1 !== "---" && fx.enabled1

    color: "#14171A"
    border.color: lineColor
    border.width: 1
    radius: 0

    readonly property var effectOptions: ["Echo", "Reverb", "Flanger", "Roll", "Phaser", "---"]

    function selectNextEffect() {
        if (!fx) return
        var index = effectOptions.indexOf(fx.effectType1)
        fx.setEffectType(1, effectOptions[(index + 1) % effectOptions.length])
    }

    // ON needs a selected effect: raising the mix on a slot still set to "---"
    // is silent, so the button would look dead. The mix amount itself is left
    // alone — switching back on restores whatever the user had dialled in.
    function toggleEffect() {
        if (!fx) return
        if (effectOn) {
            fx.setUnitEnabled(1, false)
            return
        }
        if (fx.effectType1 === "---")
            fx.setEffectType(1, effectOptions[0])
        fx.setUnitEnabled(1, true)
    }

    function setDivisionIndex(index) {
        if (!fx) return
        index = Math.max(0, Math.min(divisions.length - 1, index))
        fx.setBeatDivision(1, divisions[index].value)
    }

    // Dragging the mix is a request to hear the effect, so it engages the unit
    // explicitly. The amount itself never engages anything on its own — a
    // hardware knob streams its resting position and would switch FX on unasked.
    function setMix(amount) {
        if (!fx) return
        amount = Math.max(0, Math.min(1, amount))
        fx.setWetDry(1, amount)
        if (amount > 0.001 && !fx.enabled1) {
            if (fx.effectType1 === "---")
                fx.setEffectType(1, effectOptions[0])
            fx.setUnitEnabled(1, true)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 1

        // ── Section header ──────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: root.headerHeight
            color: "#1E2429"
            Text {
                anchors.centerIn: parent; text: "BEAT FX"; color: root.panelText
                font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 1.0
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: "×"; color: root.mutedText; font.pixelSize: 16
            }
            MouseArea { anchors.fill: parent; onClicked: root.closeRequested() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── Tempo the effect is locked to ───────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 44
            color: root.rowColor

            Text {
                id: fxBpmDecimals
                anchors.right: bpmBadge.left; anchors.rightMargin: 6
                anchors.baseline: fxBpmWhole.baseline
                text: root.fx && root.fx.displayBpm1 > 0
                      ? "." + (Math.round(root.fx.displayBpm1 * 10) % 10) : ".-"
                color: root.panelText; font.pixelSize: 13; font.family: "monospace"
            }
            Text {
                id: fxBpmWhole
                anchors.right: fxBpmDecimals.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.fx && root.fx.displayBpm1 > 0
                      ? Math.floor(root.fx.displayBpm1).toString() : "---"
                color: root.panelText; font.pixelSize: 24; font.family: "monospace"
            }

            // AUTO/MAN over the BPM caption, the way a player labels the tempo
            // source right next to the number it is following.
            Column {
                id: bpmBadge
                anchors.right: parent.right; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                Rectangle {
                    width: 34; height: 12
                    color: root.fx && root.fx.syncEnabled1 ? root.panelText : "#2C3237"
                    Text {
                        anchors.centerIn: parent
                        text: root.fx && root.fx.syncEnabled1 ? "AUTO" : "MAN"
                        color: root.fx && root.fx.syncEnabled1 ? "#14171A" : root.mutedText
                        font.pixelSize: 8; font.weight: Font.DemiBold
                    }
                }
                Text {
                    width: 34; horizontalAlignment: Text.AlignHCenter
                    text: "BPM"; color: root.mutedText
                    font.pixelSize: 8; font.weight: Font.DemiBold; font.letterSpacing: 0.4
                }
            }
            MouseArea {
                anchors.fill: parent
                onClicked: if (root.fx) root.fx.setSyncEnabled(1, !root.fx.syncEnabled1)
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── Selected effect ─────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 40
            color: root.rowColor
            Text {
                anchors.centerIn: parent
                width: parent.width - 12
                horizontalAlignment: Text.AlignHCenter
                text: root.fx ? root.fx.effectType1.toUpperCase() : "---"
                color: root.effectOn ? root.panelText : root.mutedText
                font.pixelSize: 20; font.letterSpacing: 0.5
                elide: Text.ElideRight
            }
            MouseArea { anchors.fill: parent; onClicked: root.selectNextEffect() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── Beat division: previous / current / next, all directly selectable ─
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 34
            spacing: 1
            Repeater {
                model: 3
                Rectangle {
                    required property int index
                    readonly property int divIndex: root.currentDivIndex + index - 1
                    readonly property bool current: index === 1
                    readonly property bool available: divIndex >= 0 && divIndex < root.divisions.length
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: current ? "#5A6167" : root.rowColor
                    Text {
                        anchors.centerIn: parent
                        text: available ? root.divisions[divIndex].label : ""
                        color: current ? "#FFFFFF" : root.mutedText
                        font.pixelSize: current ? 14 : 12
                        font.weight: current ? Font.DemiBold : Font.Normal
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: available && !current
                        onClicked: root.setDivisionIndex(divIndex)
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── Resulting effect time ───────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 32
            color: root.rowColor
            Text {
                anchors.left: parent.left; anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: "TIME"; color: root.mutedText
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.6
            }
            Text {
                id: timeUnit
                anchors.right: parent.right; anchors.rightMargin: 9
                anchors.baseline: timeValue.baseline
                text: "ms"; color: root.mutedText; font.pixelSize: 9
            }
            Text {
                id: timeValue
                anchors.right: timeUnit.left; anchors.rightMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                text: root.effectMs > 0 ? Math.round(root.effectMs).toString() : "---"
                color: root.panelText; font.pixelSize: 17; font.family: "monospace"
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        // ── Mix amount ──────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: root.headerHeight - 6
            color: "#1E2429"
            Text {
                anchors.centerIn: parent; text: "FX PARAMETER"; color: root.mutedText
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.8
            }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 18
            color: root.activeColor
            Text {
                anchors.centerIn: parent; text: "LEVEL / DEPTH"; color: "#0B1216"
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.6
            }
        }

        Rectangle {
            id: mixBar
            Layout.fillWidth: true; Layout.preferredHeight: 40
            color: "#14171A"

            Rectangle {
                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                width: Math.max(0, parent.width * (root.fx ? root.fx.wetDry1 : 0))
                color: root.effectOn ? root.activeColor : "#2E3439"
            }
            Text {
                anchors.centerIn: parent
                text: root.fx ? Math.round(root.fx.wetDry1 * 100) + " %" : "0 %"
                color: root.panelText; font.pixelSize: 15; font.family: "monospace"
            }
            MouseArea {
                anchors.fill: parent
                onPressed: (mouse) => root.setMix(mouse.x / width)
                onPositionChanged: (mouse) => root.setMix(mouse.x / width)
            }
        }

        Item { Layout.fillHeight: true; Layout.minimumHeight: 0 }

        // ── Routing and engage ──────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 30
            spacing: 1
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: root.routedA ? "#1F4A63" : root.rowColor
                Text {
                    anchors.centerIn: parent; text: "CH A"
                    color: root.routedA ? "#FFFFFF" : root.mutedText
                    font.pixelSize: 11; font.weight: Font.DemiBold
                }
                MouseArea { anchors.fill: parent; onClicked: if (root.fx) root.fx.setDeckAssignment(1, 1, !root.routedA) }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: root.routedB ? "#1F4A63" : root.rowColor
                Text {
                    anchors.centerIn: parent; text: "CH B"
                    color: root.routedB ? "#FFFFFF" : root.mutedText
                    font.pixelSize: 11; font.weight: Font.DemiBold
                }
                MouseArea { anchors.fill: parent; onClicked: if (root.fx) root.fx.setDeckAssignment(1, 2, !root.routedB) }
            }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 42
            color: root.effectOn ? root.accentColor : root.rowColor
            border.color: root.effectOn ? root.accentColor : root.lineColor
            border.width: 1
            Text {
                anchors.centerIn: parent
                text: "BEAT FX  " + (root.effectOn ? "ON" : "OFF")
                color: root.effectOn ? "#241708" : root.mutedText
                font.pixelSize: 13; font.weight: Font.DemiBold; font.letterSpacing: 0.8
            }
            MouseArea { anchors.fill: parent; onClicked: root.toggleEffect() }
        }
    }
}
