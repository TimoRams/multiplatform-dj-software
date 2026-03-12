import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: libraryRoot
    color: "#1e1e1e"

    // Aktiver Sidebar-Tab: "library" | "streaming" | "usb" | "files"
    property string activeTab: "library"
    // Aktiver Library-Unterpunkt
    property string librarySubTab: "allSongs"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ----------------------------------------------------------------
        // SPALTE 1: SIDEBAR
        // ----------------------------------------------------------------
        Rectangle {
            Layout.preferredWidth: 120
            Layout.fillHeight: true
            color: "#161616"

            // Rechte Trennlinie
            Rectangle {
                anchors.right:  parent.right
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: "#333"
            }

            Column {
                anchors.top:  parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 8
                spacing: 2

                Repeater {
                    model: [
                        { key: "library",   label: "Library"   },
                        { key: "streaming", label: "Streaming" },
                        { key: "usb",       label: "USB"        },
                        { key: "files",     label: "Dateien"   }
                    ]

                    delegate: Rectangle {
                        required property var modelData
                        width:  parent.width
                        height: 36
                        color:  libraryRoot.activeTab === modelData.key ? "#2a5298" : "transparent"

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left:           parent.left
                            anchors.leftMargin:     14
                            text:  modelData.label
                            color: libraryRoot.activeTab === modelData.key ? "#ffffff" : "#999999"
                            font.pixelSize: window.sp(12)
                            font.bold: libraryRoot.activeTab === modelData.key
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: libraryRoot.activeTab = modelData.key
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }
            }
        }

        // ----------------------------------------------------------------
        // SPALTE 2: ORDNERBAUM (nur sichtbar wenn Tab "files" aktiv)
        // ----------------------------------------------------------------
        Rectangle {
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            color: "#222222"
            visible: libraryRoot.activeTab === "files"

            // Rechte Trennlinie
            Rectangle {
                anchors.right:  parent.right
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: "#333"
            }

            // Spaltenheader
            Rectangle {
                id: folderHeader
                anchors.top:   parent.top
                anchors.left:  parent.left
                anchors.right: parent.right
                height: 24
                color: "#1a1a1a"

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left:  parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 6
                    spacing: 4

                    // Navigate-up button
                    Rectangle {
                        width: 20; height: 16
                        radius: 2
                        color: upMouse.containsMouse ? "#3a3a3a" : "transparent"
                        visible: libraryManager ? libraryManager.canNavigateUp : false

                        Text {
                            anchors.centerIn: parent
                            text: "↑"
                            color: "#aaaaaa"
                            font.pixelSize: window.sp(12)
                        }
                        MouseArea {
                            id: upMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (libraryManager) libraryManager.navigateUp()
                        }
                    }

                    Text {
                        verticalAlignment: Text.AlignVCenter
                        height: folderHeader.height
                        text: libraryManager
                              ? libraryManager.currentFolder.split("/").pop() || "/"
                              : "Ordner"
                        color: "#888"
                        font.pixelSize: window.sp(11)
                        elide: Text.ElideLeft
                        width: folderHeader.width - 36
                    }
                }
            }

            ListView {
                id: folderList
                anchors.top:    folderHeader.bottom
                anchors.left:   parent.left
                anchors.right:  parent.right
                anchors.bottom: parent.bottom
                clip: true

                model: libraryManager ? libraryManager.folders : []

                delegate: Rectangle {
                    required property string modelData
                    required property int    index

                    width:  ListView.view.width
                    height: 28
                    color:  index % 2 === 0 ? "transparent" : "#252525"

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        spacing: 6

                        Text {
                            text: "📁"
                            font.pixelSize: window.sp(11)
                            color: "#aaaaaa"
                        }
                        Text {
                            text: modelData
                            color: "#cccccc"
                            font.pixelSize: window.sp(12)
                            elide: Text.ElideRight
                            width: folderList.width - 40
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (libraryManager)
                                libraryManager.enterFolder(modelData)
                        }
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }

        // ----------------------------------------------------------------
        // SPALTE 2 LIBRARY: Unterpunkte (nur sichtbar wenn Tab "library")
        // ----------------------------------------------------------------
        Rectangle {
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            color: "#222222"
            visible: libraryRoot.activeTab === "library"

            Rectangle {
                anchors.right:  parent.right
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: "#333"
            }

            // Header
            Rectangle {
                id: libSubHeader
                anchors.top:   parent.top
                anchors.left:  parent.left
                anchors.right: parent.right
                height: 24
                color: "#1a1a1a"

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    text: "Sammlung"
                    color: "#888"
                    font.pixelSize: window.sp(11)
                    font.bold: true
                }
            }

            Column {
                anchors.top: libSubHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 4
                spacing: 1

                Rectangle {
                    width: parent.width
                    height: 30
                    color: libraryRoot.librarySubTab === "allSongs" ? "#2a5298" : "transparent"

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        spacing: 8

                        Text {
                            text: "♫"
                            color: libraryRoot.librarySubTab === "allSongs" ? "#ffffff" : "#888"
                            font.pixelSize: window.sp(13)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: "Alle Songs"
                            color: libraryRoot.librarySubTab === "allSongs" ? "#ffffff" : "#cccccc"
                            font.pixelSize: window.sp(12)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        // Track count badge
                        Rectangle {
                            visible: libraryModel ? libraryModel.count > 0 : false
                            width: countText.width + 10
                            height: 16
                            radius: 8
                            color: "#3a3a3a"
                            anchors.verticalCenter: parent.verticalCenter

                            Text {
                                id: countText
                                anchors.centerIn: parent
                                text: libraryModel ? libraryModel.count : "0"
                                color: "#aaa"
                                font.pixelSize: window.sp(10)
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: libraryRoot.librarySubTab = "allSongs"
                    }
                }
            }
        }

        // ----------------------------------------------------------------
        // SPALTE 2 PLACEHOLDER: sichtbar für streaming/usb Tabs
        // ----------------------------------------------------------------
        Rectangle {
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            color: "#222222"
            visible: libraryRoot.activeTab !== "files" && libraryRoot.activeTab !== "library"

            Rectangle {
                anchors.right:  parent.right
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: "#333"
            }

            Text {
                anchors.centerIn: parent
                text: libraryRoot.activeTab.charAt(0).toUpperCase() + libraryRoot.activeTab.slice(1)
                color: "#555"
                font.pixelSize: window.sp(13)
            }
        }

        // ----------------------------------------------------------------
        // SPALTE 3: CONTENT AREA
        // Switches between DB library view and file-browser track list.
        // ----------------------------------------------------------------
        Item {
            Layout.fillWidth:  true
            Layout.fillHeight: true

            // ============================================================
            // A) DATABASE LIBRARY VIEW (Tab "library")
            // ============================================================
            Rectangle {
                anchors.fill: parent
                color: "#1e1e1e"
                visible: libraryRoot.activeTab === "library"

                // Column headers
                Rectangle {
                    id: libHeader
                    anchors.top:   parent.top
                    anchors.left:  parent.left
                    anchors.right: parent.right
                    height: 24
                    color: "#1a1a1a"

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left:  parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 10
                        spacing: 0

                        // Status column
                        Text {
                            width: 36
                            text: "✓"
                            color: "#666"
                            font.pixelSize: window.sp(11)
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }
                        // Title
                        Text {
                            width: (libHeader.width - 36 - 140 - 70 - 60 - 10) * 0.55
                            text: "Titel"
                            color: "#666"
                            font.pixelSize: window.sp(11)
                            font.bold: true
                        }
                        // Artist
                        Text {
                            width: (libHeader.width - 36 - 140 - 70 - 60 - 10) * 0.45
                            text: "Künstler"
                            color: "#666"
                            font.pixelSize: window.sp(11)
                            font.bold: true
                        }
                        // BPM
                        Text {
                            width: 70
                            text: "BPM"
                            color: "#666"
                            font.pixelSize: window.sp(11)
                            font.bold: true
                            horizontalAlignment: Text.AlignRight
                        }
                        // Key
                        Text {
                            width: 60
                            text: "Key"
                            color: "#666"
                            font.pixelSize: window.sp(11)
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            leftPadding: 10
                        }
                    }
                }

                ListView {
                    id: libTrackList
                    anchors.top:    libHeader.bottom
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    anchors.bottom: parent.bottom
                    clip: true

                    model: libraryModel ? libraryModel : null

                    delegate: Rectangle {
                        id: libDelegate
                        required property int    index
                        required property string trackId
                        required property string title
                        required property string artist
                        required property real   bpm
                        required property string key
                        required property bool   isAnalyzed
                        required property string filePath

                        width:  ListView.view.width
                        height: 28
                        opacity: libDragArea.drag.active ? 0.45 : 1.0
                        color:   index % 2 === 0 ? "transparent" : "#232323"

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left:  parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 10
                            spacing: 0

                            // Status indicator
                            Text {
                                width: 36
                                text: libDelegate.isAnalyzed ? "●" : "○"
                                color: libDelegate.isAnalyzed ? "#4caf50" : "#555"
                                font.pixelSize: window.sp(11)
                                horizontalAlignment: Text.AlignHCenter
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            // Title
                            Text {
                                width: (libHeader.width - 36 - 140 - 70 - 60 - 10) * 0.55
                                text: libDelegate.title || "—"
                                color: "#dddddd"
                                font.pixelSize: window.sp(12)
                                elide: Text.ElideRight
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            // Artist
                            Text {
                                width: (libHeader.width - 36 - 140 - 70 - 60 - 10) * 0.45
                                text: libDelegate.artist || "—"
                                color: "#aaaaaa"
                                font.pixelSize: window.sp(12)
                                elide: Text.ElideRight
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            // BPM
                            Text {
                                width: 70
                                text: libDelegate.bpm > 0 ? libDelegate.bpm.toFixed(1) : "—"
                                color: libDelegate.bpm > 0 ? "#4caf50" : "#555"
                                font.pixelSize: window.sp(12)
                                horizontalAlignment: Text.AlignRight
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            // Key
                            Text {
                                width: 60
                                text: libDelegate.key || "—"
                                color: libDelegate.key ? "#42a5f5" : "#555"
                                font.pixelSize: window.sp(12)
                                horizontalAlignment: Text.AlignHCenter
                                leftPadding: 10
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        // Drag proxy for loading into decks
                        Item {
                            id: libDragPayload
                            anchors.fill: parent
                            Drag.active:           libDragArea.drag.active
                            Drag.dragType:         Drag.Automatic
                            Drag.supportedActions: Qt.CopyAction
                            Drag.hotSpot.x:        libDelegate.width  / 2
                            Drag.hotSpot.y:        libDelegate.height / 2
                            Drag.mimeData: ({
                                "text/uri-list": "file://" + libDelegate.filePath,
                                "text/plain":    libDelegate.filePath
                            })
                        }

                        MouseArea {
                            id: libDragArea
                            anchors.fill: parent
                            cursorShape:  drag.active ? Qt.DragMoveCursor : Qt.PointingHandCursor
                            drag.target:    libDragPayload
                            drag.axis:      Drag.XAndYAxis
                            drag.threshold: 6
                            onReleased: {
                                libDragPayload.Drag.drop()
                                libDragPayload.x = 0
                                libDragPayload.y = 0
                            }
                        }
                    }

                    // Empty state
                    Text {
                        anchors.centerIn: parent
                        visible: libTrackList.count === 0
                        text: "Keine Tracks in der Bibliothek.\nLade einen Track auf ein Deck, um ihn hinzuzufügen."
                        color: "#555"
                        font.pixelSize: window.sp(13)
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            // ============================================================
            // B) FILE-BROWSER TRACK LIST (Tab "files")
            // ============================================================
            Rectangle {
                anchors.fill: parent
                color: "#1e1e1e"
                visible: libraryRoot.activeTab === "files"

                // Spaltenheader
                Rectangle {
                    id: trackHeader
                    anchors.top:   parent.top
                    anchors.left:  parent.left
                    anchors.right: parent.right
                    height: 24
                    color: "#1a1a1a"

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        spacing: 0

                        Text {
                            text: "Track"
                            color: "#666"
                            font.pixelSize: window.sp(11)
                            font.bold: true
                        }
                    }
                }

                ListView {
                    id: trackList
                    anchors.top:    trackHeader.bottom
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    anchors.bottom: parent.bottom
                    clip: true

                    model: libraryManager ? libraryManager.tracks : []

                    delegate: Rectangle {
                        id: trackDelegate
                        required property string modelData
                        required property int    index

                        width:  ListView.view.width
                        height: 28
                        opacity: dragArea.drag.active ? 0.45 : 1.0
                        color:   trackDelegate.index % 2 === 0 ? "transparent" : "#232323"

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left:           parent.left
                            anchors.leftMargin:     12
                            anchors.right:          parent.right
                            anchors.rightMargin:    8
                            text:  trackDelegate.modelData
                            color: "#dddddd"
                            font.pixelSize: window.sp(12)
                            elide: Text.ElideRight
                        }

                        Item {
                            id: dragPayload
                            anchors.fill: parent

                            Drag.active:           dragArea.drag.active
                            Drag.dragType:         Drag.Automatic
                            Drag.supportedActions: Qt.CopyAction
                            Drag.hotSpot.x:        trackDelegate.width  / 2
                            Drag.hotSpot.y:        trackDelegate.height / 2
                            Drag.mimeData: ({
                                "text/uri-list": "file://" + (libraryManager ? libraryManager.currentFolder : "") + "/" + trackDelegate.modelData,
                                "text/plain":    (libraryManager ? libraryManager.currentFolder : "") + "/" + trackDelegate.modelData
                            })
                        }

                        MouseArea {
                            id: dragArea
                            anchors.fill: parent
                            cursorShape:  drag.active ? Qt.DragMoveCursor : Qt.PointingHandCursor
                            drag.target:    dragPayload
                            drag.axis:      Drag.XAndYAxis
                            drag.threshold: 6
                            onReleased: {
                                dragPayload.Drag.drop()
                                dragPayload.x = 0
                                dragPayload.y = 0
                            }
                        }
                    }

                    // Leer-Zustand
                    Text {
                        anchors.centerIn: parent
                        visible: trackList.count === 0
                        text: "Keine Audiodateien im gewählten Ordner"
                        color: "#555"
                        font.pixelSize: window.sp(13)
                    }
                }
            }

            // ============================================================
            // C) PLACEHOLDER for other tabs
            // ============================================================
            Rectangle {
                anchors.fill: parent
                color: "#1e1e1e"
                visible: libraryRoot.activeTab !== "files" && libraryRoot.activeTab !== "library"

                Text {
                    anchors.centerIn: parent
                    text: libraryRoot.activeTab.charAt(0).toUpperCase() + libraryRoot.activeTab.slice(1) + " – Platzhalter"
                    color: "#555"
                    font.pixelSize: window.sp(13)
                }
            }
        }
    }
}
