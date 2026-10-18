import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

Window {
    id: mappingEditorWindow
    title: "Mapping Editor"
    width: 660
    height: 680
    minimumWidth: 520
    minimumHeight: 460
    visible: false
    color: UiTheme.surfaceInset
    flags: Qt.Dialog

    property string searchFilter: searchField.text.trim().toLowerCase()
    readonly property var crossfaderAssignActions: [
        ["Assign Deck A: A", "deckA_crossfader_assign_a"],
        ["Assign Deck A: THRU", "deckA_crossfader_assign_thru"],
        ["Assign Deck A: B", "deckA_crossfader_assign_b"],
        ["Assign Deck B: A", "deckB_crossfader_assign_a"],
        ["Assign Deck B: THRU", "deckB_crossfader_assign_thru"],
        ["Assign Deck B: B", "deckB_crossfader_assign_b"],
        ["Assign Deck C: A", "deckC_crossfader_assign_a"],
        ["Assign Deck C: THRU", "deckC_crossfader_assign_thru"],
        ["Assign Deck C: B", "deckC_crossfader_assign_b"],
        ["Assign Deck D: A", "deckD_crossfader_assign_a"],
        ["Assign Deck D: THRU", "deckD_crossfader_assign_thru"],
        ["Assign Deck D: B", "deckD_crossfader_assign_b"]
    ]
    readonly property var mixerActions: [["Volume A", "deckA_vol"],
        ["Volume B", "deckB_vol"], ["Crossfader", "crossfader"]].concat(crossfaderAssignActions)

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
            color: UiTheme.textSecondary
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
            color: currentMapping !== "" ? UiTheme.greenDim : UiTheme.panelInset
            border.color: UiTheme.borderSubtle
            radius: 0
            Text {
                anchors.centerIn: parent
                text: currentMapping
                color: UiTheme.green
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
            color: inverted ? UiTheme.orangeDim : (invH.hovered ? UiTheme.surfaceRaised : UiTheme.panelInset)
            border.color: inverted ? UiTheme.warning : (invH.hovered ? UiTheme.borderHover : UiTheme.borderSubtle)
            radius: 0
            opacity: currentMapping !== "" ? 1.0 : 0.28
            Text {
                anchors.centerIn: parent
                text: "INV"
                color: inverted ? UiTheme.warning : UiTheme.textLabel
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
            color: xH.hovered ? UiTheme.surfaceRaised : UiTheme.panelInset
            border.color: xH.hovered ? UiTheme.error : UiTheme.borderSubtle
            radius: 0
            Text { anchors.centerIn: parent; text: "✕"; color: UiTheme.error; font.pixelSize: 10 }
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
            color: isLearning ? UiTheme.orangeDim : (lH.hovered ? UiTheme.surfaceRaised : UiTheme.panelInset)
            border.color: isLearning ? UiTheme.warning : (lH.hovered ? UiTheme.borderHover : UiTheme.borderSubtle)
            radius: 0
            Text {
                anchors.centerIn: parent
                text: isLearning ? "⬤  Warte auf MIDI…" : "Learn"
                color: isLearning ? UiTheme.warning : UiTheme.textSecondary
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
            color: UiTheme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                spacing: 10

                Text {
                    text: "MAPPING EDITOR"
                    color: UiTheme.textPrimary
                    font.pixelSize: 12
                    font.bold: true
                    font.letterSpacing: 2
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    id: savedPill
                    width: savedLbl.implicitWidth + 16
                    height: 18
                    radius: 0
                    color: UiTheme.greenDim
                    border.color: UiTheme.borderSubtle
                    opacity: 0.0

                    Text {
                        id: savedLbl
                        anchors.centerIn: parent
                        text: "● Gespeichert"
                        color: UiTheme.green
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

        Rectangle { Layout.fillWidth: true; height: 1; color: UiTheme.separatorSubtle }

        // ── Search + MIDI monitor bar ─────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 42
            color: UiTheme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 10

                // Search input
                Rectangle {
                    Layout.fillWidth: true
                    height: 26
                    color: UiTheme.panelInset
                    border.color: searchField.activeFocus ? UiTheme.borderActive : UiTheme.borderSubtle
                    radius: 0

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 6
                        spacing: 5

                        Text {
                            text: "⌕"
                            color: UiTheme.textLabel
                            font.pixelSize: 15
                            Layout.alignment: Qt.AlignVCenter
                        }

                        TextInput {
                            id: searchField
                            Layout.fillWidth: true
                            color: UiTheme.textPrimary
                            font.pixelSize: 12
                            selectByMouse: true
                            clip: true
                            Layout.alignment: Qt.AlignVCenter

                            Text {
                                visible: !parent.text && !parent.activeFocus
                                text: "Funktion suchen…"
                                color: UiTheme.textDim
                                font: parent.font
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        Rectangle {
                            width: 16; height: 16
                            radius: 0
                            color: clearH.hovered ? UiTheme.surfaceRaised : "transparent"
                            visible: searchField.text.length > 0
                            Layout.alignment: Qt.AlignVCenter
                            Text { anchors.centerIn: parent; text: "✕"; color: UiTheme.textSecondary; font.pixelSize: 10 }
                            HoverHandler { id: clearH; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: searchField.text = "" }
                        }
                    }
                }

                // Live MIDI monitor
                Rectangle {
                    Layout.preferredWidth: 196
                    height: 26
                    color: UiTheme.panelInset
                    border.color: UiTheme.borderSubtle
                    radius: 0

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 7
                        spacing: 5

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "MIDI IN:"
                            color: UiTheme.textLabel
                            font.pixelSize: 9; font.bold: true
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: midiManager ? (midiManager.lastMidiEvent || "–") : "–"
                            color: midiManager && midiManager.lastMidiEvent ? UiTheme.green : UiTheme.textDim
                            font.pixelSize: 10
                            font.family: "monospace"
                            width: 140
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: UiTheme.separatorSubtle }

        // ── Column header labels ──────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 24
            color: UiTheme.panelInset

            Row {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 0

                Text { width: 134; text: "Funktion"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
                Text { width: 86;  text: "Zugewiesen"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
                Text { width: 39;  text: "INV"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "Learn / Löschen"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: UiTheme.separatorSubtle }

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
                    contentItem: Rectangle { radius: 0; color: editorSb.active ? UiTheme.borderHover : UiTheme.borderStrong }
                    background: Rectangle { color: "transparent" }
                }

                Column {
                    id: editorCol
                    width: editorFlickable.width - 28
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    spacing: 1

                    // ── TRANSPORT ───────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Deck A – Play","deckA_play"],["Deck A – Cue","deckA_cue"],["Deck B – Play","deckB_play"],["Deck B – Cue","deckB_cue"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "TRANSPORT"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
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
                        visible: mappingEditorWindow.catVisible([["Cue A","deckA_headphone_cue"],["Cue B","deckB_headphone_cue"],["Cue C","deckC_headphone_cue"],["Cue D","deckD_headphone_cue"],["Master Cue","master_cue"],["Cue Mix","headphone_mix"],["Headphone Level","headphone_level"],["Master Level","master_level"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "HEADPHONES"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Cue A","deckA_headphone_cue");    labelStr: "Cue A";      paramId: "deckA_headphone_cue" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Cue B","deckB_headphone_cue");    labelStr: "Cue B";      paramId: "deckB_headphone_cue" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Cue C","deckC_headphone_cue");    labelStr: "Cue C";      paramId: "deckC_headphone_cue" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Cue D","deckD_headphone_cue");    labelStr: "Cue D";      paramId: "deckD_headphone_cue" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Master Cue","master_cue");        labelStr: "Master Cue"; paramId: "master_cue" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Cue Mix","headphone_mix");        labelStr: "Cue Mix";    paramId: "headphone_mix" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Headphone Level","headphone_level"); labelStr: "Headphone Level"; paramId: "headphone_level" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Master Level","master_level");    labelStr: "Master Level"; paramId: "master_level" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Cue A","deckA_headphone_cue"],["Cue B","deckB_headphone_cue"],["Cue C","deckC_headphone_cue"],["Cue D","deckD_headphone_cue"],["Master Cue","master_cue"],["Cue Mix","headphone_mix"],["Headphone Level","headphone_level"],["Master Level","master_level"]]) }

                    // ── MIXER ───────────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible(mappingEditorWindow.mixerActions)
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "MIXER"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Volume A","deckA_vol");    labelStr: "Volume A";   paramId: "deckA_vol" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Volume B","deckB_vol");    labelStr: "Volume B";   paramId: "deckB_vol" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Crossfader","crossfader"); labelStr: "Crossfader"; paramId: "crossfader" }
                    Repeater {
                        model: mappingEditorWindow.crossfaderAssignActions
                        delegate: MappingRow {
                            required property var modelData
                            width: parent.width
                            labelStr: modelData[0]
                            paramId: modelData[1]
                            visible: mappingEditorWindow.rowVisible(labelStr, paramId)
                        }
                    }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible(mappingEditorWindow.mixerActions) }

                    // ── DECK A — EQ & FILTER ────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Trim","deckA_gain"],["EQ High","deckA_eqHigh"],["EQ Mid","deckA_eqMid"],["EQ Low","deckA_eqLow"],["Filter","deckA_filter"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "DECK A — EQ & FILTER"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
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
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "DECK B — EQ & FILTER"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Trim","deckB_gain");      labelStr: "Trim";    paramId: "deckB_gain" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ High","deckB_eqHigh"); labelStr: "EQ High"; paramId: "deckB_eqHigh" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ Mid","deckB_eqMid");   labelStr: "EQ Mid";  paramId: "deckB_eqMid" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("EQ Low","deckB_eqLow");   labelStr: "EQ Low";  paramId: "deckB_eqLow" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Filter","deckB_filter");  labelStr: "Filter";  paramId: "deckB_filter" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Trim","deckB_gain"],["EQ High","deckB_eqHigh"],["EQ Mid","deckB_eqMid"],["EQ Low","deckB_eqLow"],["Filter","deckB_filter"]]) }

                    // ── TEMPO ───────────────────────────────────────────────
                    Repeater {
                        model: ["A", "B", "C", "D"]
                        delegate: MappingRow {
                            required property string modelData
                            width: parent.width
                            labelStr: "Deck " + modelData + " - Reverse"
                            paramId: "deck" + modelData + "_reverse"
                            visible: mappingEditorWindow.rowVisible(labelStr, paramId)
                        }
                    }
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Tempo A","deckA_tempo"],["Tempo B","deckB_tempo"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "TEMPO"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Tempo A","deckA_tempo"); labelStr: "Tempo A"; paramId: "deckA_tempo" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Tempo B","deckB_tempo"); labelStr: "Tempo B"; paramId: "deckB_tempo" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Tempo A","deckA_tempo"],["Tempo B","deckB_tempo"]]) }

                    // ── JOG WHEELS ──────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Touch A","deckA_jog_touch"],["Nudge A","deckA_jog_nudge"],["Scratch A","deckA_jog_scratch"],["Touch B","deckB_jog_touch"],["Nudge B","deckB_jog_nudge"],["Scratch B","deckB_jog_scratch"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "JOG WHEELS"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
                    }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Touch A","deckA_jog_touch"); labelStr: "Touch A"; paramId: "deckA_jog_touch" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Nudge A","deckA_jog_nudge");   labelStr: "Nudge A";  paramId: "deckA_jog_nudge" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Scratch A","deckA_jog_scratch");   labelStr: "Scratch A";  paramId: "deckA_jog_scratch" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Touch B","deckB_jog_touch"); labelStr: "Touch B"; paramId: "deckB_jog_touch" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Nudge B","deckB_jog_nudge");   labelStr: "Nudge B";  paramId: "deckB_jog_nudge" }
                    MappingRow { width: parent.width; visible: mappingEditorWindow.rowVisible("Scratch B","deckB_jog_scratch");   labelStr: "Scratch B";  paramId: "deckB_jog_scratch" }

                    Item { width: parent.width; height: visible ? 8 : 0
                        visible: mappingEditorWindow.catVisible([["Touch A","deckA_jog_touch"],["Nudge A","deckA_jog_nudge"],["Scratch A","deckA_jog_scratch"],["Touch B","deckB_jog_touch"],["Nudge B","deckB_jog_nudge"],["Scratch B","deckB_jog_scratch"]]) }

                    // ── BROWSER ─────────────────────────────────────────────
                    Item {
                        width: parent.width; height: visible ? 30 : 0
                        visible: mappingEditorWindow.catVisible([["Browse","library_browse"],["Load to Deck A","library_load_deck_a"],["Load to Deck B","library_load_deck_b"],["Back","library_back"],["Expand","library_expand"],["Collapse","library_collapse"],["Next Playlist","library_playlist_next"],["Prev Playlist","library_playlist_prev"]])
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "BROWSER"; color: UiTheme.textLabel; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 }
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
