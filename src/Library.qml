import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: libraryRoot
    color: "#1e1e1e"

    // Aktiver Sidebar-Tab: "library" | "streaming" | "usb" | "files"
    property string activeTab: "files"

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
                            font.pixelSize: 12
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
                            font.pixelSize: 12
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
                        font.pixelSize: 11
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
                            font.pixelSize: 11
                            color: "#aaaaaa"
                        }
                        Text {
                            text: modelData
                            color: "#cccccc"
                            font.pixelSize: 12
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
        // SPALTE 2 PLACEHOLDER: sichtbar wenn Tab NICHT "files" ist
        // ----------------------------------------------------------------
        Rectangle {
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            color: "#222222"
            visible: libraryRoot.activeTab !== "files"

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
                font.pixelSize: 13
            }
        }

        // ----------------------------------------------------------------
        // SPALTE 3: TRACKLISTE
        // ----------------------------------------------------------------
        Rectangle {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            color: "#1e1e1e"

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
                        font.pixelSize: 11
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
                    // Visuelles Feedback: Delegate wird beim Drag leicht abgedunkelt,
                    // bewegt sich aber NICHT – es bleibt starr an seinem Platz.
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
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }

                    // Invisible drag proxy: only this item moves with the cursor.
                    // Drag.Automatic hands off the drag to the OS -> no delegate clipping.
                    Item {
                        id: dragPayload
                        anchors.fill: parent   // Startposition = Delegate-Position

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

                        // Ziel ist der unsichtbare Proxy, NICHT das sichtbare Delegate
                        drag.target:    dragPayload
                        drag.axis:      Drag.XAndYAxis
                        drag.threshold: 6

                        onReleased: {
                            dragPayload.Drag.drop()
                            // Reset proxy to origin position
                            dragPayload.x = 0
                            dragPayload.y = 0
                        }
                    }
                }

                // Leer-Zustand
                Text {
                    anchors.centerIn: parent
                    visible: trackList.count === 0
                    text: libraryRoot.activeTab === "files"
                          ? "Keine Audiodateien im gewählten Ordner"
                          : libraryRoot.activeTab.charAt(0).toUpperCase() + libraryRoot.activeTab.slice(1) + " – Platzhalter"
                    color: "#555"
                    font.pixelSize: 13
                }
            }
        }
    }
}
