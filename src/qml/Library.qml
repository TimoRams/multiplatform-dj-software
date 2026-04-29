import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: libraryRoot
    color: "#141414"

    // ── External API (read by main.qml) ────────────────────────────────────
    property string activeTab:      "library"
    property string librarySubTab:  "allSongs"
    property string searchText:     ""

    // ── Theme ───────────────────────────────────────────────────────────────
    readonly property color bgBase:       "#141414"
    readonly property color bgSidebar:    "#0d0d0d"
    readonly property color bgToolbar:    "#191919"
    readonly property color bgHeader:     "#1c1c1c"
    readonly property color bgRowEven:    "#181818"
    readonly property color bgRowOdd:     "#1b1b1b"
    readonly property color bgRowHover:   "#242424"
    readonly property color borderSub:    "#1f1f1f"
    readonly property color borderMain:   "#272727"
    readonly property color textPrimary:  "#dcdcdc"
    readonly property color textSecond:   "#727272"
    readonly property color textDim:      "#383838"
    readonly property color accentBlue:   "#2d7dd2"
    readonly property color accentGreen:  "#4dd98a"
    readonly property color accentKey:    "#5bb6ff"
    readonly property color sidebarSel:   "#0d2e52"

    readonly property int rowH:       24
    readonly property int hdrH:       26
    readonly property int toolbarH:   36
    readonly property int sidebarW:   168

    // Fixed column widths (DB view)
    readonly property int colStatus:  26
    readonly property int colTime:    56
    readonly property int colBpm:     62
    readonly property int colKey:     50
    readonly property int colKbps:    50
    readonly property int colPad:     14   // right padding

    // ── Helpers ─────────────────────────────────────────────────────────────
    function _matchesSearch(value) {
        if (!searchText || searchText.length === 0) return true
        if (value === undefined || value === null) return false
        return value.toString().toLowerCase().indexOf(searchText.toLowerCase()) !== -1
    }

    readonly property var filteredFileTracks: {
        if (!libraryManager || !libraryManager.tracks) return []
        if (!searchText || searchText.length === 0) return libraryManager.tracks
        var q = searchText.toLowerCase()
        return libraryManager.tracks.filter(function(t) {
            return t.toLowerCase().indexOf(q) !== -1
        })
    }

    // ── Layout: Sidebar | Content ────────────────────────────────────────────
    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ════════════════════════════════════════════════════════════════════
        // SIDEBAR
        // ════════════════════════════════════════════════════════════════════
        Rectangle {
            Layout.preferredWidth: libraryRoot.sidebarW
            Layout.fillHeight: true
            color: libraryRoot.bgSidebar

            // Right border
            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: libraryRoot.borderMain
            }

            Column {
                anchors.fill: parent
                anchors.topMargin: 10

                // ── Section label: SAMMLUNG ──────────────────────────────────
                Item {
                    width: parent.width
                    height: 18
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        text: "SAMMLUNG"
                        color: libraryRoot.textDim
                        font.pixelSize: window.sp(9)
                        font.bold: true
                        font.letterSpacing: 1.0
                    }
                }

                // All Tracks
                Rectangle {
                    width: parent.width
                    height: 30
                    color: (libraryRoot.activeTab === "library")
                           ? libraryRoot.sidebarSel
                           : (allMouse.containsMouse ? "#161616" : "transparent")

                    // Active left accent
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        color: libraryRoot.accentBlue
                        visible: libraryRoot.activeTab === "library"
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        spacing: 9

                        Text {
                            text: "♫"
                            color: libraryRoot.activeTab === "library"
                                   ? libraryRoot.accentBlue : libraryRoot.textSecond
                            font.pixelSize: window.sp(13)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: "All Tracks"
                            color: libraryRoot.activeTab === "library"
                                   ? libraryRoot.textPrimary : "#999999"
                            font.pixelSize: window.sp(12)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Rectangle {
                        visible: libraryModel && libraryModel.count > 0
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        width: badgeText.implicitWidth + 10
                        height: 15
                        radius: 2
                        color: "#1e1e1e"

                        Text {
                            id: badgeText
                            anchors.centerIn: parent
                            text: libraryModel ? libraryModel.count : ""
                            color: libraryRoot.textSecond
                            font.pixelSize: window.sp(9)
                        }
                    }

                    MouseArea {
                        id: allMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            libraryRoot.activeTab = "library"
                            libraryRoot.librarySubTab = "allSongs"
                        }
                    }
                }

                // ── Spacer ───────────────────────────────────────────────────
                Item { width: parent.width; height: 12 }

                // ── Section label: QUELLEN ────────────────────────────────────
                Item {
                    width: parent.width
                    height: 18
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        text: "QUELLEN"
                        color: libraryRoot.textDim
                        font.pixelSize: window.sp(9)
                        font.bold: true
                        font.letterSpacing: 1.0
                    }
                }

                // Files
                Rectangle {
                    width: parent.width
                    height: 30
                    color: libraryRoot.activeTab === "files"
                           ? libraryRoot.sidebarSel
                           : (filesMouse.containsMouse ? "#161616" : "transparent")

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        color: libraryRoot.accentBlue
                        visible: libraryRoot.activeTab === "files"
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        spacing: 9
                        Text {
                            text: "📁"
                            font.pixelSize: window.sp(12)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: "Dateien"
                            color: libraryRoot.activeTab === "files"
                                   ? libraryRoot.textPrimary : "#999999"
                            font.pixelSize: window.sp(12)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        id: filesMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: libraryRoot.activeTab = "files"
                    }
                }

                // Streaming
                Rectangle {
                    width: parent.width
                    height: 30
                    color: libraryRoot.activeTab === "streaming"
                           ? libraryRoot.sidebarSel
                           : (streamMouse.containsMouse ? "#161616" : "transparent")

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        color: libraryRoot.accentBlue
                        visible: libraryRoot.activeTab === "streaming"
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        spacing: 9
                        Text {
                            text: "◉"
                            color: libraryRoot.activeTab === "streaming"
                                   ? libraryRoot.accentBlue : libraryRoot.textSecond
                            font.pixelSize: window.sp(13)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: "Streaming"
                            color: libraryRoot.activeTab === "streaming"
                                   ? libraryRoot.textPrimary : "#999999"
                            font.pixelSize: window.sp(12)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        id: streamMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: libraryRoot.activeTab = "streaming"
                    }
                }

                // USB
                Rectangle {
                    width: parent.width
                    height: 30
                    color: libraryRoot.activeTab === "usb"
                           ? libraryRoot.sidebarSel
                           : (usbMouse.containsMouse ? "#161616" : "transparent")

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        color: libraryRoot.accentBlue
                        visible: libraryRoot.activeTab === "usb"
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        spacing: 9
                        Text {
                            text: "⎘"
                            color: libraryRoot.activeTab === "usb"
                                   ? libraryRoot.accentBlue : libraryRoot.textSecond
                            font.pixelSize: window.sp(13)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: "USB"
                            color: libraryRoot.activeTab === "usb"
                                   ? libraryRoot.textPrimary : "#999999"
                            font.pixelSize: window.sp(12)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        id: usbMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: libraryRoot.activeTab = "usb"
                    }
                }
            }
        }

        // ════════════════════════════════════════════════════════════════════
        // MAIN CONTENT (Toolbar + View)
        // ════════════════════════════════════════════════════════════════════
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ── Toolbar ──────────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: libraryRoot.toolbarH
                color: libraryRoot.bgToolbar

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: libraryRoot.borderMain
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 10
                    spacing: 10

                    Text {
                        text: libraryRoot.activeTab === "library"   ? "Bibliothek"
                            : libraryRoot.activeTab === "files"     ? "Dateibrowser"
                            : libraryRoot.activeTab === "streaming" ? "Streaming"
                            : "USB"
                        color: libraryRoot.textSecond
                        font.pixelSize: window.sp(11)
                        font.bold: true
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Item { Layout.fillWidth: true }

                    // Search field
                    Rectangle {
                        Layout.preferredWidth: Math.min(280, Math.max(160, parent.width * 0.3))
                        Layout.preferredHeight: 23
                        Layout.alignment: Qt.AlignVCenter
                        color: "#111111"
                        radius: 3
                        border.color: searchField.activeFocus
                                      ? libraryRoot.accentBlue : "#2c2c2c"
                        border.width: 1

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 7
                            spacing: 5
                            visible: !searchField.activeFocus && searchField.text.length === 0

                            Text {
                                text: "🔍"
                                font.pixelSize: window.sp(9)
                                color: libraryRoot.textSecond
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: "Suche…"
                                color: libraryRoot.textSecond
                                font.pixelSize: window.sp(11)
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        TextField {
                            id: searchField
                            anchors.fill: parent
                            selectByMouse: true
                            color: libraryRoot.textPrimary
                            placeholderText: ""
                            font.pixelSize: window.sp(11)
                            leftPadding: 24
                            topPadding: 0
                            bottomPadding: 0
                            onTextEdited: libraryRoot.searchText = text
                            Component.onCompleted: text = libraryRoot.searchText
                            background: Item {}
                        }
                    }
                }
            }

            // ── View area ────────────────────────────────────────────────────
            Item {
                Layout.fillWidth:  true
                Layout.fillHeight: true

                // ════════════════════════════════════════════════════
                // A) DB LIBRARY
                // ════════════════════════════════════════════════════
                Rectangle {
                    id: libraryDbView
                    anchors.fill: parent
                    color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab === "library"

                    onVisibleChanged: {
                        if (libraryModel)
                            libraryModel.setFilterText(visible ? libraryRoot.searchText : "")
                    }
                    Connections {
                        target: libraryRoot
                        function onSearchTextChanged() {
                            if (libraryDbView.visible && libraryModel)
                                libraryModel.setFilterText(libraryRoot.searchText)
                        }
                    }

                    readonly property int colTitle:  Math.max(120,
                        Math.floor((width - libraryRoot.colStatus - libraryRoot.colTime
                                    - libraryRoot.colBpm - libraryRoot.colKey
                                    - libraryRoot.colKbps - libraryRoot.colPad) * 0.52))
                    readonly property int colArtist: Math.max(80,
                        width - libraryRoot.colStatus - libraryRoot.colTime
                        - libraryRoot.colBpm - libraryRoot.colKey
                        - libraryRoot.colKbps - libraryRoot.colPad - colTitle)

                    // ── Column headers ─────────────────────────────────────
                    Rectangle {
                        id: libHeader
                        anchors.top:   parent.top
                        anchors.left:  parent.left
                        anchors.right: parent.right
                        height: libraryRoot.hdrH
                        color: libraryRoot.bgHeader

                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            height: 1
                            color: libraryRoot.borderMain
                        }

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 6

                            // Status column
                            Item {
                                width: libraryRoot.colStatus
                                height: parent.height
                                Text {
                                    anchors.centerIn: parent
                                    text: "●"
                                    color: libraryRoot.textDim
                                    font.pixelSize: window.sp(8)
                                }
                            }

                            // Helper component: one sortable header cell
                            // (inline since QML doesn't allow dynamic component registration here)

                            // TITEL
                            Rectangle {
                                width: libraryDbView.colTitle
                                height: parent.height
                                color: "transparent"

                                Row {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    spacing: 4
                                    Text {
                                        text: "TITEL"
                                        color: libraryModel && libraryModel.sortField === "title"
                                               ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10)
                                        font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        visible: libraryModel && libraryModel.sortField === "title"
                                        text: (libraryModel && libraryModel.sortAscending) ? "▲" : "▼"
                                        color: libraryRoot.accentBlue
                                        font.pixelSize: window.sp(8)
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    width: parent.width - 2; height: 2
                                    color: libraryRoot.accentBlue
                                    visible: libraryModel && libraryModel.sortField === "title"
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: if (libraryModel) libraryModel.toggleSort("title")
                                }
                            }

                            // KÜNSTLER
                            Rectangle {
                                width: libraryDbView.colArtist
                                height: parent.height
                                color: "transparent"

                                Row {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    spacing: 4
                                    Text {
                                        text: "KÜNSTLER"
                                        color: libraryModel && libraryModel.sortField === "artist"
                                               ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10)
                                        font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        visible: libraryModel && libraryModel.sortField === "artist"
                                        text: (libraryModel && libraryModel.sortAscending) ? "▲" : "▼"
                                        color: libraryRoot.accentBlue
                                        font.pixelSize: window.sp(8)
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    width: parent.width - 2; height: 2
                                    color: libraryRoot.accentBlue
                                    visible: libraryModel && libraryModel.sortField === "artist"
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: if (libraryModel) libraryModel.toggleSort("artist")
                                }
                            }

                            // ZEIT
                            Rectangle {
                                width: libraryRoot.colTime; height: parent.height
                                color: "transparent"

                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text {
                                        text: "ZEIT"
                                        color: libraryModel && (libraryModel.sortField === "time" || libraryModel.sortField === "duration")
                                               ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        visible: libraryModel && (libraryModel.sortField === "time" || libraryModel.sortField === "duration")
                                        text: (libraryModel && libraryModel.sortAscending) ? "▲" : "▼"
                                        color: libraryRoot.accentBlue; font.pixelSize: window.sp(8)
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.left: parent.left
                                    width: parent.width; height: 2; color: libraryRoot.accentBlue
                                    visible: libraryModel && (libraryModel.sortField === "time" || libraryModel.sortField === "duration")
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: if (libraryModel) libraryModel.toggleSort("time")
                                }
                            }

                            // BPM
                            Rectangle {
                                width: libraryRoot.colBpm; height: parent.height
                                color: "transparent"

                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text {
                                        text: "BPM"
                                        color: libraryModel && libraryModel.sortField === "bpm"
                                               ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        visible: libraryModel && libraryModel.sortField === "bpm"
                                        text: (libraryModel && libraryModel.sortAscending) ? "▲" : "▼"
                                        color: libraryRoot.accentBlue; font.pixelSize: window.sp(8)
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.left: parent.left
                                    width: parent.width; height: 2; color: libraryRoot.accentBlue
                                    visible: libraryModel && libraryModel.sortField === "bpm"
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: if (libraryModel) libraryModel.toggleSort("bpm")
                                }
                            }

                            // KEY
                            Rectangle {
                                width: libraryRoot.colKey; height: parent.height
                                color: "transparent"

                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text {
                                        text: "KEY"
                                        color: libraryModel && libraryModel.sortField === "key"
                                               ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        visible: libraryModel && libraryModel.sortField === "key"
                                        text: (libraryModel && libraryModel.sortAscending) ? "▲" : "▼"
                                        color: libraryRoot.accentBlue; font.pixelSize: window.sp(8)
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.left: parent.left
                                    width: parent.width; height: 2; color: libraryRoot.accentBlue
                                    visible: libraryModel && libraryModel.sortField === "key"
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: if (libraryModel) libraryModel.toggleSort("key")
                                }
                            }

                            // KBPS
                            Rectangle {
                                width: libraryRoot.colKbps; height: parent.height
                                color: "transparent"

                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text {
                                        text: "KBPS"
                                        color: libraryModel && (libraryModel.sortField === "kbps" || libraryModel.sortField === "bitrate")
                                               ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        visible: libraryModel && (libraryModel.sortField === "kbps" || libraryModel.sortField === "bitrate")
                                        text: (libraryModel && libraryModel.sortAscending) ? "▲" : "▼"
                                        color: libraryRoot.accentBlue; font.pixelSize: window.sp(8)
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.left: parent.left
                                    width: parent.width; height: 2; color: libraryRoot.accentBlue
                                    visible: libraryModel && (libraryModel.sortField === "kbps" || libraryModel.sortField === "bitrate")
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: if (libraryModel) libraryModel.toggleSort("kbps")
                                }
                            }
                        }
                    }

                    // ── Track list ─────────────────────────────────────────
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
                            required property int    durationSec
                            required property real   bpm
                            required property string key
                            required property int    bitrateKbps
                            required property bool   isAnalyzed
                            required property string filePath

                            width:  ListView.view.width
                            height: libraryRoot.rowH
                            color:  libDragArea.containsMouse
                                    ? libraryRoot.bgRowHover
                                    : (index % 2 === 0 ? libraryRoot.bgRowEven : libraryRoot.bgRowOdd)
                            opacity: libDragPayload.dragging ? 0.4 : 1.0

                            // Hover left accent
                            Rectangle {
                                anchors.left:   parent.left
                                anchors.top:    parent.top
                                anchors.bottom: parent.bottom
                                width: 2
                                color: libraryRoot.accentBlue
                                visible: libDragArea.containsMouse
                            }

                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left:  parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 6

                                // Status
                                Text {
                                    width: libraryRoot.colStatus
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: libDelegate.isAnalyzed ? "●" : "○"
                                    color: libDelegate.isAnalyzed ? libraryRoot.accentGreen : "#2e2e2e"
                                    font.pixelSize: window.sp(8)
                                    horizontalAlignment: Text.AlignHCenter
                                }

                                // Title
                                Text {
                                    width: libraryDbView.colTitle
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: libDelegate.title || "—"
                                    color: libraryRoot.textPrimary
                                    font.pixelSize: window.sp(11)
                                    elide: Text.ElideRight
                                }

                                // Artist
                                Text {
                                    width: libraryDbView.colArtist
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: libDelegate.artist || "—"
                                    color: libraryRoot.textSecond
                                    font.pixelSize: window.sp(11)
                                    elide: Text.ElideRight
                                }

                                // Time
                                Text {
                                    width: libraryRoot.colTime
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: libDelegate.durationSec > 0
                                          ? (Math.floor(libDelegate.durationSec / 60) + ":"
                                             + ("0" + (libDelegate.durationSec % 60)).slice(-2))
                                          : "—"
                                    color: libDelegate.durationSec > 0 ? "#888888" : libraryRoot.textDim
                                    font.pixelSize: window.sp(11)
                                    font.family: "monospace"
                                    horizontalAlignment: Text.AlignHCenter
                                }

                                // BPM
                                Text {
                                    width: libraryRoot.colBpm
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: libDelegate.bpm > 0 ? libDelegate.bpm.toFixed(1) : "—"
                                    color: libDelegate.bpm > 0 ? libraryRoot.accentGreen : libraryRoot.textDim
                                    font.pixelSize: window.sp(11)
                                    font.family: "monospace"
                                    horizontalAlignment: Text.AlignHCenter
                                }

                                // Key
                                Text {
                                    width: libraryRoot.colKey
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: libDelegate.key || "—"
                                    color: libDelegate.key ? libraryRoot.accentKey : libraryRoot.textDim
                                    font.pixelSize: window.sp(11)
                                    font.family: "monospace"
                                    horizontalAlignment: Text.AlignHCenter
                                }

                                // kbps
                                Text {
                                    width: libraryRoot.colKbps
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: libDelegate.bitrateKbps > 0
                                          ? libDelegate.bitrateKbps.toString() : "—"
                                    color: libDelegate.bitrateKbps > 0 ? "#606060" : libraryRoot.textDim
                                    font.pixelSize: window.sp(10)
                                    font.family: "monospace"
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }

                            // Subtle row divider
                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left:   parent.left
                                anchors.right:  parent.right
                                height: 1
                                color: libraryRoot.borderSub
                            }

                            // Drag payload
                            Item {
                                id: libDragPayload
                                anchors.fill: parent
                                property bool dragging: false
                                Drag.active:           false
                                Drag.dragType:         Drag.Automatic
                                Drag.supportedActions: Qt.CopyAction
                                Drag.hotSpot.x: libDelegate.width  / 2
                                Drag.hotSpot.y: libDelegate.height / 2
                                Drag.mimeData: ({
                                    "text/uri-list": "file://" + libDelegate.filePath,
                                    "text/plain":    libDelegate.filePath
                                })
                            }

                            MouseArea {
                                id: libDragArea
                                anchors.fill: parent
                                hoverEnabled: true
                                property real pressX: 0
                                property real pressY: 0
                                cursorShape: libDragPayload.dragging
                                             ? Qt.DragMoveCursor : Qt.PointingHandCursor
                                onPressed: (mouse) => {
                                    pressX = mouse.x; pressY = mouse.y
                                    libDragPayload.dragging = false
                                }
                                onPositionChanged: (mouse) => {
                                    if (!pressed || libDragPayload.dragging) return
                                    if (Math.abs(mouse.x - pressX) + Math.abs(mouse.y - pressY) >= 8) {
                                        libDragPayload.dragging = true
                                        libDragPayload.Drag.active = true
                                    }
                                }
                                onReleased: {
                                    if (libDragPayload.dragging) libDragPayload.Drag.drop()
                                    libDragPayload.Drag.active = false
                                    libDragPayload.dragging = false
                                }
                                onCanceled: {
                                    libDragPayload.Drag.active = false
                                    libDragPayload.dragging = false
                                }
                            }
                        }

                        // Empty state
                        Column {
                            anchors.centerIn: parent
                            visible: libTrackList.count === 0
                            spacing: 10

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "♫"
                                color: "#252525"
                                font.pixelSize: 42
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Bibliothek ist leer"
                                color: "#333333"
                                font.pixelSize: window.sp(12)
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Lade einen Track auf ein Deck, um ihn hinzuzufügen"
                                color: "#282828"
                                font.pixelSize: window.sp(10)
                            }
                        }
                    }
                }

                // ════════════════════════════════════════════════════
                // B) FILE BROWSER
                // ════════════════════════════════════════════════════
                Rectangle {
                    anchors.fill: parent
                    color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab === "files"

                    RowLayout {
                        anchors.fill: parent
                        spacing: 0

                        // Folder pane (left)
                        Rectangle {
                            Layout.preferredWidth: 200
                            Layout.fillHeight: true
                            color: libraryRoot.bgSidebar

                            Rectangle {
                                anchors.right:  parent.right
                                anchors.top:    parent.top
                                anchors.bottom: parent.bottom
                                width: 1
                                color: libraryRoot.borderMain
                            }

                            Rectangle {
                                id: folderPaneHdr
                                anchors.top:   parent.top
                                anchors.left:  parent.left
                                anchors.right: parent.right
                                height: libraryRoot.hdrH
                                color: libraryRoot.bgHeader

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left:   parent.left
                                    anchors.right:  parent.right
                                    height: 1; color: libraryRoot.borderMain
                                }

                                Row {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 10
                                    spacing: 6

                                    Text {
                                        text: "ORDNER"
                                        color: libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                    }

                                    Rectangle {
                                        visible: libraryManager ? libraryManager.canNavigateUp : false
                                        width: 20; height: 16; radius: 2
                                        color: upMouse.containsMouse ? "#222222" : "transparent"
                                        anchors.verticalCenter: parent.verticalCenter

                                        Text {
                                            anchors.centerIn: parent
                                            text: "↑"; color: "#888"
                                            font.pixelSize: window.sp(11)
                                        }
                                        MouseArea {
                                            id: upMouse
                                            anchors.fill: parent
                                            hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: if (libraryManager) libraryManager.navigateUp()
                                        }
                                    }

                                    Text {
                                        text: libraryManager
                                              ? (libraryManager.currentFolder.split("/").pop() || "/")
                                              : ""
                                        color: "#4a4a4a"
                                        font.pixelSize: window.sp(9)
                                        elide: Text.ElideLeft
                                        width: 110
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                            }

                            ListView {
                                anchors.top:    folderPaneHdr.bottom
                                anchors.left:   parent.left
                                anchors.right:  parent.right
                                anchors.bottom: parent.bottom
                                clip: true
                                model: libraryManager ? libraryManager.folders : []

                                delegate: Rectangle {
                                    required property string modelData
                                    required property int index
                                    width:  ListView.view.width
                                    height: libraryRoot.rowH
                                    color:  folderMouse.containsMouse
                                            ? libraryRoot.bgRowHover : "transparent"

                                    Rectangle {
                                        anchors.left: parent.left
                                        anchors.top: parent.top; anchors.bottom: parent.bottom
                                        width: 2; color: libraryRoot.accentBlue
                                        visible: folderMouse.containsMouse
                                    }

                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left; anchors.leftMargin: 10
                                        spacing: 7
                                        Text {
                                            text: "📁"; font.pixelSize: window.sp(11)
                                            anchors.verticalCenter: parent.verticalCenter
                                        }
                                        Text {
                                            text: modelData; color: "#c0c0c0"
                                            font.pixelSize: window.sp(11)
                                            elide: Text.ElideRight
                                            width: 155
                                            anchors.verticalCenter: parent.verticalCenter
                                        }
                                    }
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        anchors.left: parent.left; anchors.right: parent.right
                                        height: 1; color: libraryRoot.borderSub
                                    }
                                    MouseArea {
                                        id: folderMouse
                                        anchors.fill: parent; hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: if (libraryManager) libraryManager.enterFolder(modelData)
                                    }
                                }
                            }
                        }

                        // Track list (right)
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 0

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: libraryRoot.hdrH
                                color: libraryRoot.bgHeader

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left; anchors.right: parent.right
                                    height: 1; color: libraryRoot.borderMain
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left; anchors.leftMargin: 10
                                    text: "TRACKS"
                                    color: libraryRoot.textSecond
                                    font.pixelSize: window.sp(10); font.bold: true
                                }
                            }

                            ListView {
                                id: trackList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: libraryRoot.filteredFileTracks

                                delegate: Rectangle {
                                    id: trackDelegate
                                    required property string modelData
                                    required property int index
                                    width:  ListView.view.width
                                    height: libraryRoot.rowH
                                    color:  dragArea.containsMouse
                                            ? libraryRoot.bgRowHover
                                            : (index % 2 === 0 ? libraryRoot.bgRowEven : libraryRoot.bgRowOdd)
                                    opacity: dragArea.drag.active ? 0.4 : 1.0

                                    Rectangle {
                                        anchors.left: parent.left
                                        anchors.top: parent.top; anchors.bottom: parent.bottom
                                        width: 2; color: libraryRoot.accentBlue
                                        visible: dragArea.containsMouse
                                    }

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left; anchors.leftMargin: 12
                                        anchors.right: parent.right; anchors.rightMargin: 8
                                        text:  trackDelegate.modelData
                                        color: libraryRoot.textPrimary
                                        font.pixelSize: window.sp(11)
                                        elide: Text.ElideRight
                                    }

                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        anchors.left: parent.left; anchors.right: parent.right
                                        height: 1; color: libraryRoot.borderSub
                                    }

                                    Item {
                                        id: dragPayload
                                        anchors.fill: parent
                                        Drag.active:           dragArea.drag.active
                                        Drag.dragType:         Drag.Automatic
                                        Drag.supportedActions: Qt.CopyAction
                                        Drag.hotSpot.x: trackDelegate.width  / 2
                                        Drag.hotSpot.y: trackDelegate.height / 2
                                        Drag.mimeData: ({
                                            "text/uri-list": "file://"
                                                + (libraryManager ? libraryManager.currentFolder : "")
                                                + "/" + trackDelegate.modelData,
                                            "text/plain": (libraryManager ? libraryManager.currentFolder : "")
                                                + "/" + trackDelegate.modelData
                                        })
                                    }

                                    MouseArea {
                                        id: dragArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape:  drag.active ? Qt.DragMoveCursor : Qt.PointingHandCursor
                                        drag.target:    dragPayload
                                        drag.axis:      Drag.XAndYAxis
                                        drag.threshold: 6
                                        onReleased: {
                                            dragPayload.Drag.drop()
                                            dragPayload.x = 0; dragPayload.y = 0
                                        }
                                    }
                                }

                                Text {
                                    anchors.centerIn: parent
                                    visible: trackList.count === 0
                                    text: "Keine Audiodateien im gewählten Ordner"
                                    color: "#2e2e2e"
                                    font.pixelSize: window.sp(12)
                                }
                            }
                        }
                    }
                }

                // ════════════════════════════════════════════════════
                // C) PLACEHOLDER (Streaming / USB)
                // ════════════════════════════════════════════════════
                Rectangle {
                    anchors.fill: parent
                    color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab !== "files" && libraryRoot.activeTab !== "library"

                    Column {
                        anchors.centerIn: parent
                        spacing: 12

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: libraryRoot.activeTab === "streaming" ? "◉" : "⎘"
                            color: "#252525"; font.pixelSize: 44
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: (libraryRoot.activeTab.charAt(0).toUpperCase()
                                   + libraryRoot.activeTab.slice(1)) + " — Demnächst verfügbar"
                            color: "#333333"; font.pixelSize: window.sp(12)
                        }
                    }
                }
            }
        }
    }
}
