import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Window {
    id: mappingEditorWindow
    title: "Mapping Editor"
    width: 660
    height: 680
    minimumWidth: 520
    minimumHeight: 460
    visible: false
    color: "#1a1a17"
    flags: Qt.Dialog

    property string searchFilter: searchField.text.trim().toLowerCase()

    function rowVisible(lbl, pid) {
        if (!searchFilter) return true
        return lbl.toLowerCase().indexOf(searchFilter) >= 0
            || pid.toLowerCase().indexOf(searchFilter) >= 0
    }

    function catVisible(entries) {
        if (!searchFilter) return true
        for (var i = 0; i < entries.length; ++i)
            if (rowVisible(entries[i][0], entries[i][1])) return true
        return false
    }

    component MappingRow: Item {
        required property string labelStr
        required property string paramId
        height: 34

        property string currentMapping: midiManager ? midiManager.getMappingLabel(paramId) : ""
        property bool isLearning: false
        property bool inverted: midiManager ? midiManager.isMappingInverted(paramId) : false

        Connections {
            target: midiManager
            function onMappingUpdated() {
                isLearning = false
                currentMapping = midiManager ? midiManager.getMappingLabel(paramId) : ""
            }
            function onMappingInversionUpdated() {
                inverted = midiManager ? midiManager.isMappingInverted(paramId) : false
            }
            function onLearnStarted(pid) {
                if (pid !== paramId) isLearning = false
            }
        }

        // Label
        Text {
            id: rowLbl
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 134
            text: labelStr
            color: "#888"
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        // Assignment badge
        Rectangle {
            id: rowBadge
            anchors.left: rowLbl.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 80; height: 22
            color: currentMapping !== "" ? "#0c1e11" : "transparent"
            border.color: currentMapping !== "" ? "#194d27" : "#252525"
            radius: 4
            Text {
                anchors.centerIn: parent
                text: currentMapping
                color: "#40b85a"
                font.pixelSize: 10; font.bold: true
            }
        }

        // INV toggle
        Rectangle {
            id: invBtn
            anchors.left: rowBadge.right
            anchors.leftMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            width: 34; height: 22
            color: inverted ? "#1e1600" : (invH.hovered ? "#1c1c1c" : "#141414")
            border.color: inverted ? "#c07800" : (invH.hovered ? "#333" : "#222")
            radius: 4
            opacity: currentMapping !== "" ? 1.0 : 0.28
            Text {
                anchors.centerIn: parent
                text: "INV"
                color: inverted ? "#ff9900" : "#444"
                font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5
            }
            HoverHandler { id: invH; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                onTapped: {
                    if (midiManager && currentMapping !== "")
                        midiManager.setMappingInverted(paramId, !inverted)
                }
            }
        }

        // Clear button (only when mapped)
        Rectangle {
            id: clearBtn
            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            width: 26; height: 22
            visible: currentMapping !== ""
            color: xH.hovered ? "#330d0d" : "#1c0808"
            border.color: "#3a1414"
            radius: 4
            Text { anchors.centerIn: parent; text: "✕"; color: "#c04040"; font.pixelSize: 10 }
            HoverHandler { id: xH; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: { if (midiManager) midiManager.clearLearnedMapping(paramId) } }
        }

        // Learn button
        Rectangle {
            anchors.left: invBtn.right
            anchors.leftMargin: 5
            anchors.right: currentMapping !== "" ? clearBtn.left : parent.right
            anchors.rightMargin: currentMapping !== "" ? 4 : 2
            anchors.verticalCenter: parent.verticalCenter
            height: 22
            color: isLearning ? "#2a1600" : (lH.hovered ? "#222" : "#181818")
            border.color: isLearning ? "#d08000" : (lH.hovered ? "#353535" : "#222")
            radius: 4
            Text {
                anchors.centerIn: parent
                text: isLearning ? "⬤  Warte auf MIDI…" : "Learn"
                color: isLearning ? "#ff9900" : "#555"
                font.pixelSize: 11; font.bold: isLearning
            }
            HoverHandler { id: lH; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                onTapped: {
                    if (midiManager && !isLearning) {
                        isLearning = true
                        midiManager.startMidiLearn(paramId)
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Title bar ────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 44
            color: "#111111"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                spacing: 10

                Text {
                    text: "MAPPING EDITOR"
                    color: "#ff9900"
                    font.pixelSize: 12
                    font.bold: true
                    font.letterSpacing: 2
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    id: savedPill
                    width: savedLbl.implicitWidth + 16
                    height: 18
                    radius: 3
                    color: "#1a3a1a"
                    border.color: "#2a5a2a"
                    opacity: 0.0

                    Text {
                        id: savedLbl
                        anchors.centerIn: parent
                        text: "● Gespeichert"
                        color: "#4ac84a"
                        font.pixelSize: 10
                    }

                    Connections {
                        target: midiManager
                        function onMappingUpdated()          { savedPill.opacity = 1.0; saveFade.restart() }
                        function onMappingInversionUpdated() { savedPill.opacity = 1.0; saveFade.restart() }
                    }

                    NumberAnimation {
                        id: saveFade
                        target: savedPill; property: "opacity"
                        from: 1.0; to: 0.0; duration: 1800
                        easing.type: Easing.InQuad
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#282828" }

        // ── Search + MIDI monitor bar ─────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 42
            color: "#161614"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 10

                // Search input
                Rectangle {
                    Layout.fillWidth: true
                    height: 26
                    color: "#0e0e0e"
                    border.color: searchField.activeFocus ? "#444" : "#222"
                    radius: 4

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 6
                        spacing: 5

                        Text {
                            text: "⌕"
                            color: "#555"
                            font.pixelSize: 15
                            Layout.alignment: Qt.AlignVCenter
                        }

                        TextInput {
                            id: searchField
                            Layout.fillWidth: true
                            color: "#ccc"
                            font.pixelSize: 12
                            selectByMouse: true
                            clip: true
                            Layout.alignment: Qt.AlignVCenter

                            Text {
                                visible: !parent.text && !parent.activeFocus
                                text: "Funktion suchen…"
                                color: "#333"
                                font: parent.font
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        Rectangle {
                            width: 16; height: 16
                            radius: 8
                            color: clearH.hovered ? "#333" : "transparent"
                            visible: searchField.text.length > 0
                            Layout.alignment: Qt.AlignVCenter
                            Text { anchors.centerIn: parent; text: "✕"; color: "#777"; font.pixelSize: 10 }
                            HoverHandler { id: clearH; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: searchField.text = "" }
                        }
                    }
                }

                // Live MIDI monitor
                Rectangle {
                    Layout.preferredWidth: 196
                    height: 26
                    color: "#0e0e0e"
                    border.color: "#1c1c1c"
                    radius: 4

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 7
                        spacing: 5

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "MIDI IN:"
                            color: "#444"
                            font.pixelSize: 9; font.bold: true
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: midiManager ? (midiManager.lastMidiEvent || "–") : "–"
                            color: midiManager && midiManager.lastMidiEvent ? "#7cdb9a" : "#333"
                            font.pixelSize: 10
                            font.family: "monospace"
                            width: 140
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#282828" }

        // ── Column header labels ──────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 24
            color: "#131311"

            Row {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 0

                Text { width: 134; text: "Funktion"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
                Text { width: 86;  text: "Zugewiesen"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
                Text { width: 39;  text: "INV"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "Learn / Löschen"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#222220" }

        // ── Scrollable mapping list ───────────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Flickable {
                id: editorFlickable
                anchors.fill: parent
                contentWidth: width
                contentHeight: editorCol.implicitHeight + 20
                flickableDirection: Flickable.VerticalFlick
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    id: editorSb
                    width: 6
                    policy: ScrollBar.AsNeeded
                    contentItem: Rectangle { radius: 2; color: editorSb.active ? "#666" : "#3a3a3a" }
                    background: Rectangle { color: "transparent" }
                }

                Column {
                    id: editorCol
                    width: editorFlickable.width - 10
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    spacing: 1

                    // ── TRANSPORT ───────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Deck A – Play","deckA_play"],["Deck A – Cue","deckA_cue"],["Deck B – Play","deckB_play"],["Deck B – Cue","deckB_cue"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "TRANSPORT"; color: "#3d3d3d"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Deck A – Play","deckA_play"); labelStr: "Deck A – Play"; paramId: "deckA_play" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Deck A – Cue","deckA_cue");   labelStr: "Deck A – Cue";  paramId: "deckA_cue"  }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Deck B – Play","deckB_play"); labelStr: "Deck B – Play"; paramId: "deckB_play" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Deck B – Cue","deckB_cue");   labelStr: "Deck B – Cue";  paramId: "deckB_cue"  }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Deck A – Play","deckA_play"],["Deck A – Cue","deckA_cue"],["Deck B – Play","deckB_play"],["Deck B – Cue","deckB_cue"]]) }

                    // ── HEADPHONES ──────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Cue A","deckA_headphone_cue"],["Cue B","deckB_headphone_cue"],["Master Cue","master_cue"],["Cue Mix","headphone_mix"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "HEADPHONES"; color: "#3d3d3d"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Cue A","deckA_headphone_cue");    labelStr: "Cue A";      paramId: "deckA_headphone_cue" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Cue B","deckB_headphone_cue");    labelStr: "Cue B";      paramId: "deckB_headphone_cue" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Master Cue","master_cue");        labelStr: "Master Cue"; paramId: "master_cue" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Cue Mix","headphone_mix");        labelStr: "Cue Mix";    paramId: "headphone_mix" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Cue A","deckA_headphone_cue"],["Cue B","deckB_headphone_cue"],["Master Cue","master_cue"],["Cue Mix","headphone_mix"]]) }

                    // ── MIXER ───────────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Volume A","deckA_vol"],["Volume B","deckB_vol"],["Crossfader","crossfader"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "MIXER"; color: "#3d3d3d"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Volume A","deckA_vol");    labelStr: "Volume A";   paramId: "deckA_vol" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Volume B","deckB_vol");    labelStr: "Volume B";   paramId: "deckB_vol" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Crossfader","crossfader"); labelStr: "Crossfader"; paramId: "crossfader" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Volume A","deckA_vol"],["Volume B","deckB_vol"],["Crossfader","crossfader"]]) }

                    // ── DECK A — EQ & FILTER ────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Trim","deckA_gain"],["EQ High","deckA_eqHigh"],["EQ Mid","deckA_eqMid"],["EQ Low","deckA_eqLow"],["Filter","deckA_filter"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "DECK A — EQ & FILTER"; color: "#3d3d3d"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Trim","deckA_gain");      labelStr: "Trim";    paramId: "deckA_gain" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ High","deckA_eqHigh"); labelStr: "EQ High"; paramId: "deckA_eqHigh" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ Mid","deckA_eqMid");   labelStr: "EQ Mid";  paramId: "deckA_eqMid" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ Low","deckA_eqLow");   labelStr: "EQ Low";  paramId: "deckA_eqLow" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Filter","deckA_filter");  labelStr: "Filter";  paramId: "deckA_filter" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Trim","deckA_gain"],["EQ High","deckA_eqHigh"],["EQ Mid","deckA_eqMid"],["EQ Low","deckA_eqLow"],["Filter","deckA_filter"]]) }

                    // ── DECK B — EQ & FILTER ────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Trim","deckB_gain"],["EQ High","deckB_eqHigh"],["EQ Mid","deckB_eqMid"],["EQ Low","deckB_eqLow"],["Filter","deckB_filter"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "DECK B — EQ & FILTER"; color: "#3d3d3d"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Trim","deckB_gain");      labelStr: "Trim";    paramId: "deckB_gain" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ High","deckB_eqHigh"); labelStr: "EQ High"; paramId: "deckB_eqHigh" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ Mid","deckB_eqMid");   labelStr: "EQ Mid";  paramId: "deckB_eqMid" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ Low","deckB_eqLow");   labelStr: "EQ Low";  paramId: "deckB_eqLow" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Filter","deckB_filter");  labelStr: "Filter";  paramId: "deckB_filter" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Trim","deckB_gain"],["EQ High","deckB_eqHigh"],["EQ Mid","deckB_eqMid"],["EQ Low","deckB_eqLow"],["Filter","deckB_filter"]]) }

                    // ── TEMPO ───────────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Tempo A","deckA_tempo"],["Tempo B","deckB_tempo"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "TEMPO"; color: "#3d3d3d"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Tempo A","deckA_tempo"); labelStr: "Tempo A"; paramId: "deckA_tempo" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Tempo B","deckB_tempo"); labelStr: "Tempo B"; paramId: "deckB_tempo" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Tempo A","deckA_tempo"],["Tempo B","deckB_tempo"]]) }

                    // ── JOG WHEELS ──────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Touch A","deckA_jog_touch"],["Move A","deckA_jog_move"],["Touch B","deckB_jog_touch"],["Move B","deckB_jog_move"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "JOG WHEELS"; color: "#3d3d3d"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Touch A","deckA_jog_touch"); labelStr: "Touch A"; paramId: "deckA_jog_touch" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Move A","deckA_jog_move");   labelStr: "Move A";  paramId: "deckA_jog_move" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Touch B","deckB_jog_touch"); labelStr: "Touch B"; paramId: "deckB_jog_touch" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Move B","deckB_jog_move");   labelStr: "Move B";  paramId: "deckB_jog_move" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Touch A","deckA_jog_touch"],["Move A","deckA_jog_move"],["Touch B","deckB_jog_touch"],["Move B","deckB_jog_move"]]) }

                    // ── BROWSER ─────────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Browse","library_browse"],["Load to Deck A","library_load_deck_a"],["Load to Deck B","library_load_deck_b"],["Back","library_back"],["Expand","library_expand"],["Collapse","library_collapse"],["Next Playlist","library_playlist_next"],["Prev Playlist","library_playlist_prev"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "BROWSER"; color: "#3d3d3d"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Browse","library_browse");                    labelStr: "Browse";          paramId: "library_browse" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Load to Deck A","library_load_deck_a");       labelStr: "Load to Deck A";  paramId: "library_load_deck_a" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Load to Deck B","library_load_deck_b");       labelStr: "Load to Deck B";  paramId: "library_load_deck_b" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Back","library_back");                        labelStr: "Back";            paramId: "library_back" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Expand","library_expand");                    labelStr: "Expand";          paramId: "library_expand" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Collapse","library_collapse");                labelStr: "Collapse";        paramId: "library_collapse" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Next Playlist","library_playlist_next");      labelStr: "Next Playlist";   paramId: "library_playlist_next" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Prev Playlist","library_playlist_prev");      labelStr: "Prev Playlist";   paramId: "library_playlist_prev" }

                    Item { width: parent.width; height: 14 }
                }
            }
        }
    }
}
