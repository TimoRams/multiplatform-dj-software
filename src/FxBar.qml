import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// ─────────────────────────────────────────────────────────────────────────────
// FxBar – horizontal FX strip that sits between the Deck/Mixer section and the
//         Library.  Two FxUnits share the full width 50 / 50 with a thin
//         divider in the exact centre.
// ─────────────────────────────────────────────────────────────────────────────
Rectangle {
    id: root

    color: "#141414"
    height: 40

    // Thin border lines to visually separate from the rows above and below
    Rectangle {
        anchors.top:   parent.top
        anchors.left:  parent.left
        anchors.right: parent.right
        height: 1
        color: "#2a2a2a"
    }
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        height: 1
        color: "#2a2a2a"
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── FX Unit 1 (Deck A side) ──────────────────────────────────────────
        FxUnit {
            id: fxUnit1
            unitId: 1
            accentColor: "#1e90ff"   // blue – matches deck A
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        // ── Centre divider ───────────────────────────────────────────────────
        Rectangle {
            width:  1
            Layout.fillHeight: true
            color: "#2e2e2e"
        }

        // ── FX Unit 2 (Deck B side) ──────────────────────────────────────────
        FxUnit {
            id: fxUnit2
            unitId: 2
            accentColor: "#ff6a00"   // orange – matches deck B
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
