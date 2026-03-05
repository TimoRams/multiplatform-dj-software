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

    Connections {
        target: deck.engine
        function onTrackMetadataChanged() { deck._syncMetadata() }
    }

    Connections {
        target: deck.engine ? deck.engine.trackData : null
        function onBpmAnalyzed() { deck._syncBpm() }
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

                    // BPM badge
                    Rectangle {
                        visible: deck._hasTrack
                        width:   bpmLabel.implicitWidth + 10
                        height:  20
                        radius:  3
                        color:   "#1a2e1a"
                        border.color: "#4a8a4a"
                        border.width: 1
                        Text {
                            id: bpmLabel
                            anchors.centerIn: parent
                            text:  deck._trackBpm !== "" ? deck._trackBpm + " BPM" : "BPM: \u2013"
                            color: deck._trackBpm !== "" ? "#80e080" : "#507050"
                            font.pixelSize: window.sp(10)
                            font.family: "monospace"
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

            // Overview waveform + Tempo Fader
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                spacing: 10

                // Waveform (left side)
                OverallWaveform {
                    engine: deck.engine
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }

                // Tempo Fader (right side, vertical)
                Rectangle {
                    Layout.preferredWidth: 50
                    Layout.fillHeight: true
                    color: "#1a1a1a"
                    border.color: "#333"
                    border.width: 1
                    radius: 4

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 5
                        spacing: 2

                        // Tempo label
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: "TEMPO"
                            color: "#aaaaaa"
                            font.pixelSize: window.sp(9)
                            font.bold: true
                        }

                        // Vertical slider for tempo (±8%)
                        Slider {
                            id: tempoSlider
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            orientation: Qt.Vertical
                            from: -8
                            to: 8
                            value: 0
                            stepSize: 0.5
                            TapHandler {
                                onDoubleTapped: {
                                    tempoSlider.enabled = false
                                    tempoSlider.value = 0
                                    tempoSlider.enabled = true
                                }
                            }
                            onValueChanged: {
                                if (deck.engine) {
                                    deck.engine.setTempoPercent(value)
                                }
                            }
                        }

                        // Percentage display
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: (tempoSlider.value >= 0 ? "+" : "") + tempoSlider.value.toFixed(1) + "%"
                            color: "#aaaaaa"
                            font.pixelSize: window.sp(10)
                            font.bold: true
                            font.family: "monospace"
                        }
                    }
                }
            }

            // Controls (Play/Pause, Cue, Sync + Quantize, Keylock, Slip)
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                
                Button {
                    text: "PLAY"
                    Layout.preferredWidth: 80
                    Layout.preferredHeight: 40
                    palette.buttonText: "white"
                    background: Rectangle {
                        color: deck.engine && deck.engine.isPlaying ? "lightgreen" : "#444"
                        radius: 4
                    }
                    onClicked: {
                        if(deck.engine) deck.engine.togglePlay()
                    }
                }
                
                Button {
                    text: "CUE"
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 40
                    palette.buttonText: "white"
                    background: Rectangle { color: "#444"; radius: 4 }
                }
                
                Button {
                    text: "SYNC"
                    checkable: true
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 40
                    palette.buttonText: checked ? "white" : "#aaa"
                    background: Rectangle { 
                        color: parent.checked ? "#3a8a3a" : "#444"
                        border.color: parent.checked ? "#5cfa5c" : "transparent"
                        border.width: parent.checked ? 1 : 0
                        radius: 4 
                    }
                }

                // Spacer to push the other buttons to the right (if space allows), or just visually separate
                Item { Layout.fillWidth: true }

                Button {
                    text: "QUANTIZE"
                    checkable: true
                    Layout.preferredWidth: 80
                    Layout.preferredHeight: 40
                    palette.buttonText: checked ? "white" : "#aaa"
                    background: Rectangle { 
                        color: parent.checked ? (deck.deckName === "A" ? "#995c00" : "#007a99") : "#333"
                        border.color: parent.checked ? (deck.deckName === "A" ? "#ff9900" : "#00ccff") : "#555"
                        border.width: 1
                        radius: 4 
                    }
                }

                Button {
                    text: "KEYLOCK"
                    checkable: true
                    Layout.preferredWidth: 80
                    Layout.preferredHeight: 40
                    palette.buttonText: checked ? "white" : "#aaa"
                    background: Rectangle { 
                        color: parent.checked ? (deck.deckName === "A" ? "#995c00" : "#007a99") : "#333"
                        border.color: parent.checked ? (deck.deckName === "A" ? "#ff9900" : "#00ccff") : "#555"
                        border.width: 1
                        radius: 4 
                    }
                }

                Button {
                    text: "SLIP"
                    checkable: true
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 40
                    palette.buttonText: checked ? "white" : "#aaa"
                    background: Rectangle { 
                        color: parent.checked ? (deck.deckName === "A" ? "#995c00" : "#007a99") : "#333"
                        border.color: parent.checked ? (deck.deckName === "A" ? "#ff9900" : "#00ccff") : "#555"
                        border.width: 1
                        radius: 4 
                    }
                }
            }
            
            // Performance pads (hot cue, pad FX, beatjump, stems)
            PerformancePads {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 100
                accentColor: deck.deckName === "A" ? "#ff9900" : "#00ccff"
            }
        }
    }
}
