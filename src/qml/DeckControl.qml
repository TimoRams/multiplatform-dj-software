import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Item {
    id: deck
    property string deckName: "A"
    property var engine: null
    property string _manualBpmInput: ""

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
        // For loops, show perceived rhythmic BPM and wrap to a realistic range.
        if (cb > 0 && deck.engine.loopActive) {
            var beats = deck.engine.loopLengthBeats
            // Only sub-beat loops (<1 beat) alter perceived BPM.
            // Loops >= 1 beat (e.g. 2 beats) should keep normal track BPM display.
            if (beats > 0.001 && beats < 1.0)
                cb = cb / beats

            if (beats > 0.001 && beats < 1.0) {
                var minBpm = 70.0
                var maxBpm = 150.0
                while (cb > maxBpm)
                    cb = cb / 2.0
                while (cb < minBpm)
                    cb = cb * 2.0
            }
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
                    deck.engine.togglePlay()
                } else if (value <= 0.5 && deck.engine.isPlaying) {
                    deck.engine.togglePlay()
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

            // Deck header: unified metadata fields (always visible)
            Rectangle {
                Layout.fillWidth: true
                height: 52
                color: "transparent"

                RowLayout {
                    anchors.fill: parent
                    spacing: 4

                    // Cover art square (left-aligned)
                    Rectangle {
                        id: coverArt
                        Layout.preferredWidth: 52
                        Layout.fillHeight: true
                        radius: 4
                        color: "#1a1a1a"
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
                            font.pixelSize: deck._hasTrack ? window.spViewport(22) : window.spViewport(18)
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

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 2

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 25
                            radius: 4
                            color: "#1b1b1b"
                            border.color: "#3a3a3a"
                            border.width: 1

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 4

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "TITLE"
                                    color: "#6f6f6f"
                                    font.pixelSize: window.spViewport(6)
                                    font.bold: true
                                    font.family: "monospace"
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 42
                                    text: deck._hasTrack ? deck._trackTitle : "No Track Loaded"
                                    color: deck._hasTrack ? "#f0f0f0" : "#777"
                                    font.pixelSize: window.spViewport(8)
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 25
                            radius: 4
                            color: "#1b1b1b"
                            border.color: "#3a3a3a"
                            border.width: 1

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 4

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "ARTIST"
                                    color: "#6f6f6f"
                                    font.pixelSize: window.spViewport(6)
                                    font.bold: true
                                    font.family: "monospace"
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 46
                                    text: deck._hasTrack
                                          ? (deck._trackArtist !== "" ? deck._trackArtist : "Unknown Artist")
                                          : "-"
                                    color: deck._hasTrack ? "#b8b8b8" : "#666"
                                    font.pixelSize: window.spViewport(8)
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    GridLayout {
                        id: metaBadges
                        columns: 2
                        rowSpacing: 2
                        columnSpacing: 2
                        Layout.preferredWidth: 164
                        Layout.alignment: Qt.AlignVCenter

                        // BASE BPM (analyzed/manual value)
                        Rectangle {
                            id: bpmBadge
                            Layout.preferredWidth: 81
                            Layout.preferredHeight: 25
                            radius: 4
                            color: "#1a2e1a"
                            border.color: "#4a8a4a"
                            border.width: 1

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "BPM"
                                    color: "#5f8f5f"
                                    font.pixelSize: window.spViewport(6)
                                    font.bold: true
                                    font.family: "monospace"
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: deck._trackBpm !== "" ? deck._trackBpm : "--"
                                    color: deck._trackBpm !== "" ? "#80e080" : "#5a705a"
                                    font.pixelSize: window.spViewport(8)
                                    font.bold: true
                                    font.family: "monospace"
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                cursorShape: Qt.PointingHandCursor
                                onClicked: (mouse) => {
                                    if (mouse.button !== Qt.RightButton || !deck.engine)
                                        return
                                    deck._manualBpmInput = deck._trackBpm !== ""
                                        ? deck._trackBpm
                                        : (deck._currentBpm !== "" ? deck._currentBpm : "120.00")
                                    manualBpmField.text = deck._manualBpmInput
                                    manualBpmPopup.visible = true
                                    manualBpmField.forceActiveFocus()
                                    manualBpmField.selectAll()
                                }
                            }
                        }

                        // LIVE BPM (tempo/loop adjusted value)
                        Rectangle {
                            Layout.preferredWidth: 81
                            Layout.preferredHeight: 25
                            radius: 4
                            color: "#20242f"
                            border.color: deck._showLiveBpmIndicator() ? "#4f7fcf" : "#3f475a"
                            border.width: 1

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "LIVE"
                                    color: "#6b82a5"
                                    font.pixelSize: window.spViewport(6)
                                    font.bold: true
                                    font.family: "monospace"
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: deck._currentBpm !== "" ? deck._currentBpm : "--"
                                    color: {
                                        if (!deck._showLiveBpmIndicator()) return "#6c7484"
                                        if (!deck.engine) return "#aab6cc"
                                        return deck.engine.tempoPercent > 0 ? "#ffaa00" : "#55ccff"
                                    }
                                    font.pixelSize: window.spViewport(8)
                                    font.bold: true
                                    font.family: "monospace"
                                }
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: 81
                            Layout.preferredHeight: 25
                            radius: 4
                            color: "#1a1a2e"
                            border.color: "#4a4aaa"
                            border.width: 1

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "KEY"
                                    color: "#5d6bad"
                                    font.pixelSize: window.spViewport(6)
                                    font.bold: true
                                    font.family: "monospace"
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: deck._trackKey !== "" ? deck._trackKey : "--"
                                    color: deck._trackKey !== "" ? "#8080e0" : "#59608a"
                                    font.pixelSize: window.spViewport(8)
                                    font.bold: true
                                    font.family: "monospace"
                                }
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: 81
                            Layout.preferredHeight: 25
                            radius: 4
                            color: "#202020"
                            border.color: "#4a4a4a"
                            border.width: 1

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "LEN"
                                    color: "#7f7f7f"
                                    font.pixelSize: window.spViewport(6)
                                    font.bold: true
                                    font.family: "monospace"
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: deck._trackDuration !== "" ? deck._trackDuration : "--:--"
                                    color: deck._trackDuration !== "" ? "#b0b0b0" : "#6f6f6f"
                                    font.pixelSize: window.spViewport(8)
                                    font.bold: true
                                    font.family: "monospace"
                                }
                            }
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
                            font.pixelSize: window.spViewport(8)
                            background: Rectangle {
                                color: deck.engine && deck.engine.isPlaying ? "#44aa44" : "#444"
                                radius: 3
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(9)
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
                                font.pixelSize: window.spViewport(8)
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
                                font.pixelSize: window.spViewport(9)
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
                                font.pixelSize: window.spViewport(8)
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
                                    font.pixelSize: window.spViewport(8)
                                    font.bold: true
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    text: "▾"
                                    color: deck.deckName === "A" ? "#ff9900" : "#00ccff"
                                    font.pixelSize: window.spViewport(9)
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
                            font.pixelSize: window.spViewport(9)
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
                            font.pixelSize: window.spViewport(9)
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

        // ── Manual BPM popup (right-click BPM badge) ────────────────────────
        Rectangle {
            id: manualBpmPopup
            visible: false
            z: 1000

            x: Math.max(6, Math.min(parent.width - width - 6,
                                    metaBadges.x + bpmBadge.x + bpmBadge.width - width))
            y: metaBadges.y + bpmBadge.height + 6

            width: 160
            height: 92
            radius: 4
            color: "#1e1e1e"
            border.color: deck.deckName === "A" ? "#4a8a4a" : "#4a8a4a"
            border.width: 1

            MouseArea {
                anchors.fill: parent
                onClicked: mouse => mouse.accepted = true
            }

            Column {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                Text {
                    text: "MANUAL BPM"
                    color: "#a5d6a7"
                    font.pixelSize: window.spViewport(9)
                    font.bold: true
                    font.family: "monospace"
                }

                TextField {
                    id: manualBpmField
                    width: parent.width
                    height: 30
                    placeholderText: "z.B. 124.50"
                    color: "#111"
                    placeholderTextColor: "#666"
                    selectionColor: "#4caf50"
                    selectedTextColor: "#fff"
                    font.pixelSize: window.spViewport(11)
                    font.bold: true
                    font.family: "monospace"
                    background: Rectangle {
                        color: "#f5f5f5"
                        border.color: "#8bc34a"
                        border.width: 2
                        radius: 3
                    }
                    onAccepted: applyManualBpm()

                    function applyManualBpm() {
                        var value = Number(text.replace(",", "."))
                        if (!deck.engine || isNaN(value) || value <= 0)
                            return
                        deck.engine.setManualBpm(value)
                        deck._syncBpm()
                        deck._syncTempo()
                        manualBpmPopup.visible = false
                    }
                }

                Text {
                    text: "Enter = Anwenden"
                    color: "#8e8e8e"
                    font.pixelSize: window.spViewport(8)
                    font.family: "monospace"
                }
            }
        }

        // Transparent full-deck overlay: dismiss popup when clicking outside it
        MouseArea {
            anchors.fill: parent
            z: 998
            visible: tempoRangePopup.visible || manualBpmPopup.visible
            onClicked: {
                tempoRangePopup.visible = false
                manualBpmPopup.visible = false
            }
        }
    }
}
