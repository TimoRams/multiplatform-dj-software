import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Item {
    id: deck
    property string deckName: "A"
    property var engine: null

    Layout.fillWidth: true
    Layout.fillHeight: true

    property bool dropHovered: false

    // Local metadata properties: engine is a 'var', so QML cannot track its signals
    // directly. Values are mirrored here and updated explicitly via Connections.
    property bool   _hasTrack:      false
    property string _trackTitle:    "No Track Loaded"
    property string _trackArtist:   ""
    property string _trackAlbum:    ""
    property string _trackDuration: ""
    property string _trackKey:      ""
    property string _trackBpm:      ""
    property string _currentBpm:    ""   // live tempo-adjusted BPM

    function loopLabel() {
        if (!deck.engine || !deck.engine.loopActive)
            return "4 BEAT"
        var beats = deck.engine.loopLengthBeats
        if (Math.abs(beats - 1.5) < 0.08)
            return "1.5"
        if (Math.abs(beats - 0.75) < 0.05)
            return "3/4"
        if (Math.abs(beats - 0.5) < 0.04)
            return "1/2"
        if (Math.abs(beats - 0.25) < 0.03)
            return "1/4"
        if (beats >= 1.0)
            return Math.round(beats) + " BEAT"
        return beats.toFixed(2) + " BEAT"
    }

    function _syncMetadata() {
        if (!deck.engine) return
        _hasTrack      = deck.engine.hasTrack
        _trackTitle    = deck.engine.trackTitle
        _trackArtist   = deck.engine.trackArtist
        _trackAlbum    = deck.engine.trackAlbum
        _trackDuration = deck.engine.trackDuration
        _trackKey      = deck.engine.trackKey
        console.log("[Deck " + deck.deckName + "] title='" + _trackTitle + "' artist='" + _trackArtist + "' key='" + _trackKey + "' dur='" + _trackDuration + "'")
    }

    function _syncBpm() {
        if (!deck.engine || !deck.engine.trackData) return
        if (deck.engine.trackData.isBpmAnalyzed)
            _trackBpm = deck.engine.trackData.bpm.toFixed(2)
    }

    function _syncTempo() {
        if (!deck.engine) return
        var cb = deck.engine.currentBpm
        // For sub-beat loops (e.g. 3/4), show the perceived rhythmic BPM.
        if (cb > 0 && deck.engine.loopActive) {
            var beats = deck.engine.loopLengthBeats
            if (beats > 0.001 && beats < 1.0)
                cb = cb / beats
        }
        _currentBpm = cb > 0 ? cb.toFixed(2) : ""
    }

    function _showLiveBpmIndicator() {
        if (!deck.engine || deck._currentBpm === "")
            return false

        // Always show when tempo fader is moved.
        if (Math.abs(deck.engine.tempoPercent) > 0.01)
            return true

        // Also show when loop math changes perceived BPM without tempo fader move.
        var base = Number(deck._trackBpm)
        var live = Number(deck._currentBpm)
        if (!isNaN(base) && !isNaN(live))
            return Math.abs(live - base) > 0.01

        return false
    }

    Connections {
        target: deck.engine
        function onTrackMetadataChanged() { deck._syncMetadata() }
        function onTempoChanged() { deck._syncTempo() }
        function onLoopChanged() { deck._syncTempo() }
    }

    Connections {
        target: deck.engine ? deck.engine.trackData : null
        function onBpmAnalyzed() { deck._syncBpm(); deck._syncTempo() }
        function onBeatgridChanged() { deck._syncBpm(); deck._syncTempo() }
    }

    // Connect to ParameterStore for MIDI mapping
    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if (!deck.engine) return
            var expectedId = "deck" + deck.deckName + "_play"
            if (id === expectedId) {
                if (value > 0.5 && !deck.engine.isPlaying) {
                    deck.engine.play()
                } else if (value <= 0.5 && deck.engine.isPlaying) {
                    deck.engine.pause()
                }
            }
        }
    }

    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list", "text/plain"]

        onEntered: (drag) => {
            drag.accept(Qt.CopyAction)
            deck.dropHovered = true
        }
        onExited:  () => { deck.dropHovered = false }

        onDropped: (drop) => {
            deck.dropHovered = false
            var path = ""
            if (drop.hasUrls && drop.urls.length > 0) {
                path = drop.urls[0].toString()
            } else if (drop.hasText) {
                path = drop.text
            }
            // Strip file:// prefix (Linux/macOS)
            if (path.startsWith("file://"))
                path = path.substring(7)
            if (path !== "" && deck.engine) {
                console.log("Deck " + deck.deckName + " ← " + path)
                deck.engine.loadTrack(path)
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#161616"
        border.color: deck.dropHovered ? "#5599ff" : "#333"
        border.width: deck.dropHovered ? 2 : 1

        // Drag-hover overlay
        Rectangle {
            anchors.fill: parent
            color: "#5599ff"
            opacity: deck.dropHovered ? 0.07 : 0.0
            Behavior on opacity { NumberAnimation { duration: 80 } }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 12

            // Deck header: cover art, title, artist, BPM, key
            Rectangle {
                Layout.fillWidth: true
                height: 56
                color: "transparent"

                // Cover art square (left-aligned)
                Rectangle {
                    id: coverArt
                    anchors.left:   parent.left
                    anchors.top:    parent.top
                    anchors.bottom: parent.bottom
                    width:  height   // quadratisch
                    radius: 4
                    color:  "#1a1a1a"
                    border.color: deck._hasTrack
                                  ? (deck.deckName === "A" ? "#ff9900" : "#00ccff")
                                  : "#333"
                    border.width: 1

                    // Placeholder icon when no cover art is present
                    Text {
                        id: coverPlaceholder
                        anchors.centerIn: parent
                        text: deck._hasTrack ? "♪" : "♫"
                        color: deck._hasTrack
                               ? (deck.deckName === "A" ? "#ff9900" : "#00ccff")
                               : "#333"
                        font.pixelSize: deck._hasTrack ? window.sp(22) : window.sp(18)
                        opacity: deck._hasTrack ? 0.6 : 0.4
                        visible: coverImage.status !== Image.Ready
                    }

                    // Cover art via CoverArtProvider (image://coverart/)
                    Image {
                        id: coverImage
                        anchors.fill: parent
                        anchors.margins: 1
                        source: deck.engine && deck.engine.hasCoverArt
                                ? deck.engine.coverArtUrl : ""
                        fillMode: Image.PreserveAspectCrop
                        visible: status === Image.Ready
                    }
                }

                // Titelzeile + Artist
                Column {
                    anchors.left:   coverArt.right
                    anchors.leftMargin: 8
                    anchors.right:  metaBadges.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Text {
                        width: parent.width
                        text:  deck._hasTrack ? deck._trackTitle : "No Track Loaded"
                        color: deck._hasTrack ? "#f0f0f0" : "#555"
                        font.pixelSize: window.sp(14)
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text:  deck._hasTrack ? deck._trackArtist : ""
                        color: "#aaaaaa"
                        font.pixelSize: window.sp(11)
                        elide: Text.ElideRight
                        visible: deck._hasTrack && deck._trackArtist !== ""
                    }
                }

                // BPM + KEY badges (right-aligned)
                Row {
                    id: metaBadges
                    anchors.right:          parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5

                    // BPM badge (analyzed + live current)
                    Rectangle {
                        visible: deck._hasTrack
                        width:   bpmBadgeRow.implicitWidth + 10
                        height:  20
                        radius:  3
                        color:   "#1a2e1a"
                        border.color: "#4a8a4a"
                        border.width: 1

                        Row {
                            id: bpmBadgeRow
                            anchors.centerIn: parent
                            spacing: 4

                            Text {
                                id: bpmLabel
                                text:  deck._trackBpm !== "" ? deck._trackBpm : "BPM: \u2013"
                                color: deck._trackBpm !== "" ? "#80e080" : "#507050"
                                font.pixelSize: window.sp(10)
                                font.family: "monospace"
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            // Live BPM — shown on tempo shift OR loop-based perceived BPM shift
                            Text {
                                visible: deck._showLiveBpmIndicator()
                                text:  "→ " + deck._currentBpm
                                // Orange when sped up, light blue when slowed down
                                color: {
                                    if (!deck.engine) return "#aaaaaa"
                                    return deck.engine.tempoPercent > 0 ? "#ffaa00" : "#55ccff"
                                }
                                font.pixelSize: window.sp(10)
                                font.family: "monospace"
                                font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }

                    // KEY badge
                    Rectangle {
                        visible: deck._hasTrack
                        width:   keyLabel.implicitWidth + 10
                        height:  20
                        radius:  3
                        color:   "#1a1a2e"
                        border.color: "#4a4aaa"
                        border.width: 1
                        Text {
                            id: keyLabel
                            anchors.centerIn: parent
                            text:  deck._trackKey !== "" ? deck._trackKey : "KEY: \u2013"
                            color: deck._trackKey !== "" ? "#8080e0" : "#505070"
                            font.pixelSize: window.sp(10)
                            font.family: "monospace"
                        }
                    }

                    // Duration badge
                    Rectangle {
                        visible: deck._hasTrack
                        width:   durLabel.implicitWidth + 10
                        height:  20
                        radius:  3
                        color:   "#202020"
                        border.color: "#444"
                        border.width: 1
                        Text {
                            id: durLabel
                            anchors.centerIn: parent
                            text:  deck._trackDuration
                            color: "#aaaaaa"
                            font.pixelSize: window.sp(10)
                            font.family: "monospace"
                        }
                    }
                }
            }

            // Overview waveform + Buttons (left) | Tempo Fader (right)
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                // LEFT: Overview on top, buttons below
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    // Slim rectified overview waveform
                    OverallWaveform {
                        engine: deck.engine
                        Layout.fillWidth: true
                        Layout.preferredHeight: 44
                    }

                    SegmentBar {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 10
                        segments: deck.engine ? deck.engine.currentSegments : []
                        totalTrackDuration: deck.engine ? deck.engine.trackDurationSec : 0
                    }

                    // Controls (Play/Pause, Cue, Sync + Quantize, Keylock, Slip)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 3

                        Button {
                            text: "PLAY"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            palette.buttonText: "white"
                            font.pixelSize: window.sp(8)
                            background: Rectangle {
                                color: deck.engine && deck.engine.isPlaying ? "#44aa44" : "#444"
                                radius: 3
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideNone
                            }
                            onClicked: { if(deck.engine) deck.engine.togglePlay() }
                        }

                        Button {
                            text: "CUE"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle { color: "#444"; radius: 3 }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Button {
                            text: "REV"
                            checkable: true
                            checked: deck.engine ? deck.engine.isReverse : false
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: parent.checked ? "#883300" : "#444"
                                border.color: parent.checked ? "#ff6600" : "transparent"
                                border.width: 1
                                radius: 3
                            }
                            contentItem: Text {
                                text: parent.text
                                color: parent.checked ? "#ff6600" : "#aaa"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                if (deck.engine) deck.engine.setReverse(checked)
                            }
                        }

                        Button {
                            text: "SYNC"
                            checkable: true
                            checked: deck.engine ? deck.engine.syncEnabled : false
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: {
                                    if (!deck.engine || !parent.checked) return "#444"
                                    return deck.engine.syncMaster ? "#7a5a10" : "#3a8a3a"
                                }
                                border.color: {
                                    if (!deck.engine || !parent.checked) return "transparent"
                                    return deck.engine.syncMaster ? "#ffd24d" : "#5cfa5c"
                                }
                                border.width: (deck.engine && deck.engine.syncMaster) ? 2 : 1
                                radius: 3
                            }
                            contentItem: Text {
                                text: parent.text
                                color: {
                                    if (!parent.checked) return "#aaa"
                                    if (deck.engine && deck.engine.syncMaster) return "#ffe18a"
                                    return "white"
                                }
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                if (deck.engine)
                                    deck.engine.setSyncEnabled(checked)
                            }
                        }

                        Button {
                            text: "QUANTIZE"
                            checkable: true
                            checked: deck.engine ? deck.engine.quantizeEnabled : false
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: parent.checked ? (deck.deckName === "A" ? "#995c00" : "#007a99") : "#333"
                                border.color: parent.checked ? (deck.deckName === "A" ? "#ff9900" : "#00ccff") : "#555"
                                border.width: 1; radius: 3
                            }
                            contentItem: Text {
                                text: parent.text
                                color: parent.checked ? "white" : "#aaa"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                if (deck.engine) deck.engine.quantizeEnabled = checked
                            }
                        }

                        Button {
                            text: "KEYLOCK"
                            checkable: true
                            checked: deck.engine ? deck.engine.keylock : false
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: parent.checked ? (deck.deckName === "A" ? "#995c00" : "#007a99") : "#333"
                                border.color: parent.checked ? (deck.deckName === "A" ? "#ff9900" : "#00ccff") : "#555"
                                border.width: 1; radius: 3
                            }
                            contentItem: Text {
                                text: parent.text
                                color: parent.checked ? "white" : "#aaa"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                if (deck.engine) deck.engine.keylock = checked
                            }
                        }

                        Button {
                            text: "SLIP"
                            checkable: true
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: parent.checked ? (deck.deckName === "A" ? "#995c00" : "#007a99") : "#333"
                                border.color: parent.checked ? (deck.deckName === "A" ? "#ff9900" : "#00ccff") : "#555"
                                border.width: 1; radius: 3
                            }
                            contentItem: Text {
                                text: parent.text
                                color: parent.checked ? "white" : "#aaa"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    // Loop controls
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 3

                        Button {
                            text: "LOOP IN"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: "#333"
                                radius: 3
                                border.color: "#555"
                                border.width: 1
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#bbb"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: if (deck.engine) deck.engine.setLoopIn()
                        }

                        Button {
                            text: "LOOP OUT"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: "#333"
                                radius: 3
                                border.color: "#555"
                                border.width: 1
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#bbb"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: if (deck.engine) deck.engine.setLoopOut()
                        }

                        Button {
                            text: "<"
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: "#333"
                                radius: 3
                                border.color: "#555"
                                border.width: 1
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#bbb"
                                font.pixelSize: window.sp(9)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: if (deck.engine) deck.engine.halveLoopLength()
                        }

                        Button {
                            text: deck.loopLabel()
                            checkable: true
                            checked: deck.engine ? (deck.engine.loopActive && Math.abs(deck.engine.loopLengthBeats - 0.75) < 0.06) : false
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: parent.checked ? "#335533" : "#333"
                                radius: 3
                                border.color: parent.checked ? "#66dd66" : "#555"
                                border.width: 1
                            }
                            contentItem: Text {
                                text: parent.text
                                color: parent.checked ? "#d8ffd8" : "#bbb"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            onClicked: if (deck.engine) deck.engine.toggleLoop4Beats()
                        }

                        Button {
                            text: ">"
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: "#333"
                                radius: 3
                                border.color: "#555"
                                border.width: 1
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#bbb"
                                font.pixelSize: window.sp(9)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: if (deck.engine) deck.engine.doubleLoopLength()
                        }

                        Button {
                            text: "3/4"
                            checkable: true
                            checked: deck.engine ? (deck.engine.loopActive && Math.abs(deck.engine.loopLengthBeats - 0.75) < 0.06) : false
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 18
                            background: Rectangle {
                                color: parent.checked ? "#334455" : "#333"
                                radius: 3
                                border.color: parent.checked ? "#66bbff" : "#555"
                                border.width: 1
                            }
                            contentItem: Text {
                                text: parent.text
                                color: parent.checked ? "#d8ecff" : "#bbb"
                                font.pixelSize: window.sp(8)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: if (deck.engine) deck.engine.toggleLoopThreeQuarter()
                        }
                    }
                }

                // RIGHT: Tempo Fader spanning full height of this row
                Rectangle {
                    id: tempoPanel
                    Layout.preferredWidth: 50
                    Layout.fillHeight: true
                    color: "#1a1a1a"
                    border.color: "#333"
                    border.width: 1
                    radius: 4

                    property real tempoRange: 8

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 4
                        spacing: 2

                        // ── Clickable TEMPO ▾ header ──
                        Rectangle {
                            id: tempoHeader
                            Layout.fillWidth: true
                            height: 18
                            radius: 3
                            color: tempoRangePopup.visible ? "#2a2a2a" : "transparent"

                            Row {
                                anchors.centerIn: parent
                                spacing: 3
                                Text {
                                    text: "TEMPO"
                                    color: "#cccccc"
                                    font.pixelSize: window.sp(8)
                                    font.bold: true
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    text: "▾"
                                    color: deck.deckName === "A" ? "#ff9900" : "#00ccff"
                                    font.pixelSize: window.sp(9)
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: tempoRangePopup.visible = !tempoRangePopup.visible
                            }
                        }

                        Slider {
                            id: tempoSlider
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            orientation: Qt.Vertical
                            from: -tempoPanel.tempoRange
                            to:    tempoPanel.tempoRange
                            value: 0
                            stepSize: tempoPanel.tempoRange <= 8  ? 0.1
                                    : tempoPanel.tempoRange <= 16 ? 0.25
                                    : tempoPanel.tempoRange <= 32 ? 0.5
                                    : 1.0
                            TapHandler {
                                onDoubleTapped: {
                                    tempoSlider.enabled = false
                                    tempoSlider.value = 0
                                    tempoSlider.enabled = true
                                    if (deck.engine) deck.engine.setTempoPercent(0)
                                }
                            }
                            onValueChanged: { if (deck.engine) deck.engine.setTempoPercent(value) }
                        }

                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: (tempoSlider.value >= 0 ? "+" : "") + tempoSlider.value.toFixed(1) + "%"
                            color: tempoSlider.value > 0 ? "#ffaa00"
                                 : tempoSlider.value < 0 ? "#55ccff"
                                 : "#666666"
                            font.pixelSize: window.sp(9)
                            font.bold: true
                            font.family: "monospace"
                        }
                    }
                }
            }
            
            // Performance pads (hot cue, pad FX, beatjump, stems)
            PerformancePads {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 100
                engine: deck.engine
                accentColor: deck.deckName === "A" ? "#ff9900" : "#00ccff"
            }
        }

        // ── Tempo range popup — floats above everything, z:999 ──────────────
        // Lives directly inside the deck Rectangle (not in any layout) so it
        // is never clipped by the ColumnLayout / RowLayout / Slider stacking.
        Rectangle {
            id: tempoRangePopup
            visible: false
            z: 999

            // Position: align left edge with tempoPanel, just below the header
            x: tempoPanel.x + (tempoPanel.width - width) / 2
            y: tempoPanel.y + 26          // header height (18) + margin (4) + panel margin (4)

            width: 58
            height: rangeCol.implicitHeight + 10
            radius: 4
            color: "#1e1e1e"
            border.color: deck.deckName === "A" ? "#ff9900" : "#00ccff"
            border.width: 1

            // Close when clicking anywhere outside the popup
            MouseArea {
                anchors.fill: parent
                onClicked: mouse => mouse.accepted = true   // eat click, don't close
            }

            Column {
                id: rangeCol
                anchors { top: parent.top; left: parent.left; right: parent.right; margins: 5 }
                spacing: 1

                Repeater {
                    model: [
                        { label: "6%",   value: 6   },
                        { label: "8%",   value: 8   },
                        { label: "16%",  value: 16  },
                        { label: "32%",  value: 32  },
                        { label: "WIDE", value: 100 }
                    ]

                    delegate: Rectangle {
                        required property var modelData
                        width: rangeCol.width
                        height: 18
                        radius: 3
                        color: tempoPanel.tempoRange === modelData.value
                               ? (deck.deckName === "A" ? "#332200" : "#002233")
                               : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: tempoPanel.tempoRange === modelData.value
                                   ? (deck.deckName === "A" ? "#ff9900" : "#00ccff")
                                   : "#aaaaaa"
                            font.pixelSize: window.sp(9)
                            font.bold: tempoPanel.tempoRange === modelData.value
                            font.family: "monospace"
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                tempoPanel.tempoRange = modelData.value
                                var clamped = Math.max(-modelData.value,
                                              Math.min( modelData.value, tempoSlider.value))
                                tempoSlider.enabled = false
                                tempoSlider.value   = clamped
                                tempoSlider.enabled = true
                                if (deck.engine) deck.engine.setTempoPercent(clamped)
                                tempoRangePopup.visible = false
                            }
                        }
                    }
                }
            }
        }

        // Transparent full-deck overlay: dismiss popup when clicking outside it
        MouseArea {
            anchors.fill: parent
            z: 998
            visible: tempoRangePopup.visible
            onClicked: tempoRangePopup.visible = false
        }
    }
}
