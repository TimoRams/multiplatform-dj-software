import QtQuick
import QtQuick.Layouts

Item {
    id: root

    // --- Daten-Properties (werden von DeckControl gebunden) ---
    property string deckName:     "A"
    property string trackTitle:   "No Track Loaded"
    property string trackArtist:  ""
    property string trackAlbum:   ""
    property string trackBpm:     ""
    property string trackKey:     ""
    property string trackDuration: ""
    property bool   hasTrack:     false
    property url    coverArtUrl:  ""

    implicitHeight: 80
    implicitWidth: 300

    // Deck-Farben: A = Orange, B = Cyan
    readonly property color deckColor: deckName === "A" ? "#ff9900" : "#00ccff"

    RowLayout {
        anchors.fill: parent
        spacing: 10

        // ── DECK BADGE + COVER ───────────────────────────────────────
        Item {
            width: 64
            height: 64

            // Cover-Art Hintergrund
            Rectangle {
                id: coverBg
                anchors.fill: parent
                radius: 5
                color: "#222"
                border.color: root.deckColor
                border.width: 2

                Text {
                    anchors.centerIn: parent
                    text: "\u266a" // ♪
                    font.pixelSize: 26
                    color: "#444"
                    visible: coverImage.status !== Image.Ready
                }

                Image {
                    id: coverImage
                    anchors.fill: parent
                    anchors.margins: 2
                    source: root.coverArtUrl
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready
                }
            }

            // Deck badge (A / B) overlaid at bottom-right of cover art
            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: -2
                width: 20
                height: 20
                radius: 10
                color: root.deckColor
                z: 10

                Text {
                    anchors.centerIn: parent
                    text: root.deckName
                    color: "#111"
                    font.pixelSize: 11
                    font.bold: true
                }
            }
        }

        // ── TEXT + BADGES ────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 3

            // Titel
            Text {
                Layout.fillWidth: true
                text: root.hasTrack ? root.trackTitle : "No Track Loaded"
                color: root.hasTrack ? "#f0f0f0" : "#555"
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
                maximumLineCount: 1
            }

            // Artist + Album
            Text {
                Layout.fillWidth: true
                text: {
                    if (!root.hasTrack) return ""
                    if (root.trackAlbum) return root.trackArtist + "  —  " + root.trackAlbum
                    return root.trackArtist
                }
                color: "#999"
                font.pixelSize: 11
                elide: Text.ElideRight
                maximumLineCount: 1
                visible: root.hasTrack
            }

            // BPM / Key / Duration Badges
            RowLayout {
                spacing: 6
                Layout.topMargin: 1

                // BPM
                Rectangle {
                    visible: root.trackBpm !== ""
                    radius: 3
                    color: "#1a2e1a"
                    border.color: root.hasTrack ? "#4a8a4a" : "#333"
                    border.width: 1
                    implicitWidth: bpmText.implicitWidth + 12
                    height: 18

                    Text {
                        id: bpmText
                        anchors.centerIn: parent
                        text: root.trackBpm + " BPM"
                        color: root.hasTrack ? "#80e080" : "#555"
                        font.pixelSize: 10
                        font.family: "monospace"
                    }
                }

                // Key
                Rectangle {
                    visible: root.trackKey !== ""
                    radius: 3
                    color: "#1a1a2e"
                    border.color: root.hasTrack ? "#4a4aaa" : "#333"
                    border.width: 1
                    implicitWidth: keyText.implicitWidth + 12
                    height: 18

                    Text {
                        id: keyText
                        anchors.centerIn: parent
                        text: root.trackKey
                        color: root.hasTrack ? "#8080e0" : "#555"
                        font.pixelSize: 10
                        font.family: "monospace"
                    }
                }

                // Duration
                Rectangle {
                    visible: root.trackDuration !== ""
                    radius: 3
                    color: "#202020"
                    border.color: "#444"
                    border.width: 1
                    implicitWidth: durText.implicitWidth + 12
                    height: 18

                    Text {
                        id: durText
                        anchors.centerIn: parent
                        text: root.trackDuration
                        color: "#aaa"
                        font.pixelSize: 10
                        font.family: "monospace"
                    }
                }

                Item { Layout.fillWidth: true }
            }
        }
    }
}
