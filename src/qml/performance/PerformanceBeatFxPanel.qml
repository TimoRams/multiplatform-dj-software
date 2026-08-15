import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    property var fx: null
    signal closeRequested()

    readonly property int headerHeight: 36
    readonly property int rowHeight: 40
    readonly property int panelMargin: 7
    readonly property color panelText: "#E7EAEC"
    readonly property color mutedText: "#9DA5AA"
    readonly property color lineColor: "#343B40"
    readonly property color controlColor: "#20252A"
    readonly property var divisions: [
        { label: "1/16", value: 0.0625 }, { label: "1/8", value: 0.125 },
        { label: "1/4", value: 0.25 }, { label: "1/2", value: 0.5 },
        { label: "1", value: 1.0 }, { label: "2", value: 2.0 }, { label: "4", value: 4.0 }
    ]
    readonly property real currentDiv: fx ? fx.beatDiv1 : 0.25
    readonly property var nearbyDivisions: {
        var index = 0
        var nearest = Number.MAX_VALUE
        for (var i = 0; i < divisions.length; ++i) {
            var distance = Math.abs(currentDiv - divisions[i].value)
            if (distance < nearest) { nearest = distance; index = i }
        }
        return [divisions[Math.max(0, index - 1)], divisions[index], divisions[Math.min(divisions.length - 1, index + 1)]]
    }
    readonly property bool routedA: fx ? fx.deck1A : false
    readonly property bool routedB: fx ? fx.deck1B : false
    readonly property bool effectOn: fx && fx.effectType1 !== "---" && fx.wetDry1 > 0.001

    color: "#171A1D"
    border.color: lineColor
    border.width: 1
    radius: 0

    readonly property var effectOptions: ["Echo", "Reverb", "Flanger", "Roll", "Phaser", "---"]

    function selectNextEffect() {
        if (!fx) return
        var index = effectOptions.indexOf(fx.effectType1)
        fx.setEffectType(1, effectOptions[(index + 1) % effectOptions.length])
    }

    // ON needs both a selected effect and a non-zero wet amount. Raising the wet
    // amount alone leaves the slot on "---", which is silent — the button then
    // looks dead because it never reports ON back.
    function toggleEffect() {
        if (!fx) return
        if (effectOn) {
            fx.setWetDry(1, 0)
            return
        }
        if (fx.effectType1 === "---")
            fx.setEffectType(1, effectOptions[0])
        fx.setWetDry(1, 0.5)
    }

    function stepBeatDivision(direction) {
        if (!fx) return
        var index = 0
        var nearest = Number.MAX_VALUE
        for (var i = 0; i < divisions.length; ++i) {
            var distance = Math.abs(currentDiv - divisions[i].value)
            if (distance < nearest) { nearest = distance; index = i }
        }
        index = Math.max(0, Math.min(divisions.length - 1, index + direction))
        fx.setBeatDivision(1, divisions[index].value)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: root.headerHeight
            color: "#24292D"
            Text { anchors.centerIn: parent; text: "BEAT FX"; color: root.panelText; font.pixelSize: 12; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
            Text { anchors.right: parent.right; anchors.rightMargin: 9; anchors.verticalCenter: parent.verticalCenter; text: "×"; color: root.mutedText; font.pixelSize: 17 }
            MouseArea { anchors.fill: parent; onClicked: root.closeRequested() }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: root.rowHeight
            Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin
            Text { text: "BPM"; color: root.mutedText; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.5 }
            Item { Layout.fillWidth: true }
            Text {
                Layout.preferredWidth: 72; Layout.maximumWidth: 72
                text: root.fx && root.fx.displayBpm1 > 0 ? root.fx.displayBpm1.toFixed(1) : "---.-"
                color: root.panelText; font.pixelSize: 18; font.family: "monospace"
                elide: Text.ElideRight; horizontalAlignment: Text.AlignRight
            }
            Rectangle {
                Layout.preferredWidth: 54; Layout.preferredHeight: 30; color: root.fx && root.fx.syncEnabled1 ? "#E99128" : root.controlColor; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: root.fx && root.fx.syncEnabled1 ? "AUTO" : "MAN"; color: root.fx && root.fx.syncEnabled1 ? "#332411" : root.mutedText; font.pixelSize: 10; font.weight: Font.DemiBold }
                MouseArea { anchors.fill: parent; onClicked: if (root.fx) root.fx.setSyncEnabled(1, !root.fx.syncEnabled1) }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.lineColor }

        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 44; Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin
            color: root.controlColor; border.color: root.lineColor; border.width: 1
            Text { anchors.centerIn: parent; text: root.fx ? root.fx.effectType1.toUpperCase() : "---"; color: root.panelText; font.pixelSize: 16; font.weight: Font.DemiBold; font.letterSpacing: 0.4 }
            MouseArea { anchors.fill: parent; onClicked: root.selectNextEffect() }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: root.rowHeight
            Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin; spacing: 4
            Rectangle {
                Layout.preferredWidth: 34; Layout.fillHeight: true
                color: root.controlColor; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "−"; color: root.panelText; font.pixelSize: 15 }
                MouseArea { anchors.fill: parent; onClicked: root.stepBeatDivision(-1) }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: "#3B3022"; border.color: "#D88B2C"; border.width: 1
                Text { anchors.centerIn: parent; text: root.nearbyDivisions[1].label; color: root.panelText; font.pixelSize: 12; font.weight: Font.DemiBold }
            }
            Rectangle {
                Layout.preferredWidth: 34; Layout.fillHeight: true
                color: root.controlColor; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "+"; color: root.panelText; font.pixelSize: 15 }
                MouseArea { anchors.fill: parent; onClicked: root.stepBeatDivision(1) }
            }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: root.rowHeight
            Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin
            Text { text: "MIX"; color: root.mutedText; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.5 }
            Item { Layout.fillWidth: true }
            Text { text: root.fx ? Math.round(root.fx.wetDry1 * 100) + "%" : "0%"; color: root.panelText; font.pixelSize: 20; font.family: "monospace" }
        }

        Rectangle {
            id: xPad
            Layout.fillWidth: true; Layout.preferredHeight: 46; Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin
            color: "#181B1E"; border.color: root.lineColor; border.width: 1
            Rectangle { width: Math.max(3, parent.width * (root.fx ? root.fx.wetDry1 : 0)); anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; color: "#168FC4" }
            Text { anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top; anchors.topMargin: 6; text: "DRY                         WET"; color: root.mutedText; font.pixelSize: 9; font.family: "monospace" }
            MouseArea {
                anchors.fill: parent
                function setValue(mouse) { if (root.fx) root.fx.setWetDry(1, Math.max(0, Math.min(1, mouse.x / width))) }
                onPressed: (mouse) => setValue(mouse)
                onPositionChanged: (mouse) => setValue(mouse)
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: root.rowHeight
            Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin; Layout.bottomMargin: root.panelMargin; spacing: 4
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; color: root.effectOn ? "#5B351D" : root.controlColor; border.color: "#E99128"; border.width: 1
                Text { anchors.centerIn: parent; text: "FX " + (root.effectOn ? "ON" : "OFF"); color: root.panelText; font.pixelSize: 12; font.weight: Font.DemiBold }
                MouseArea { anchors.fill: parent; onClicked: root.toggleEffect() }
            }
            Rectangle {
                Layout.preferredWidth: 54; Layout.fillHeight: true; color: root.routedA ? "#244153" : root.controlColor; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "A"; color: root.panelText; font.pixelSize: 14; font.weight: Font.DemiBold }
                MouseArea { anchors.fill: parent; onClicked: if (root.fx) root.fx.setDeckAssignment(1, 1, !root.routedA) }
            }
            Rectangle {
                Layout.preferredWidth: 54; Layout.fillHeight: true; color: root.routedB ? "#244153" : root.controlColor; border.color: root.lineColor; border.width: 1
                Text { anchors.centerIn: parent; text: "B"; color: root.panelText; font.pixelSize: 14; font.weight: Font.DemiBold }
                MouseArea { anchors.fill: parent; onClicked: if (root.fx) root.fx.setDeckAssignment(1, 2, !root.routedB) }
            }
        }
    }
}
