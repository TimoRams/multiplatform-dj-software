import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: libraryRoot
    color: "#141414"

    // ── External API ───────────────────────────────────────────────────────
    property string activeTab:         "library"  // "library" | "playlist" | "files" | "streaming" | "usb"
    property string librarySubTab:     "allSongs"
    property string searchText:        ""
    property string currentPlaylistId: ""
    property string currentPlaylistName: ""

    // Playlist data (loaded from C++ on demand)
    property var allPlaylists:   []   // [{id, name, parentId, sortOrder, trackCount}, ...]
    property var playlistTracks: []   // [{trackId, title, artist, ...}, ...] for currentPlaylistId
    property var expandedPlaylists: ({})  // id → true if expanded

    // Context menu state (shared between library + playlist views)
    property string ctxTrackId:   ""
    property string ctxFilePath:  ""
    property string ctxTitle:     ""

    // ── Theme ───────────────────────────────────────────────────────────────
    readonly property color bgBase:      "#141414"
    readonly property color bgSidebar:   "#0d0d0d"
    readonly property color bgSidebarHv: "#121212"
    readonly property color bgToolbar:   "#191919"
    readonly property color bgHeader:    "#1c1c1c"
    readonly property color bgRowEven:   "#181818"
    readonly property color bgRowOdd:    "#1b1b1b"
    readonly property color bgRowHover:  "#242424"
    readonly property color bgRowActive: "#1d3a52"
    readonly property color borderSub:   "#1f1f1f"
    readonly property color borderMain:  "#272727"
    readonly property color borderHigh:  "#383838"
    readonly property color textPrimary: "#dcdcdc"
    readonly property color textSecond:  "#727272"
    readonly property color textDim:     "#383838"
    readonly property color accentBlue:  "#2d7dd2"
    readonly property color accentBlueLt:"#4a99e0"
    readonly property color accentGreen: "#4dd98a"
    readonly property color accentGreenLt:"#6fefab"
    readonly property color accentOrange:"#ff9d2d"
    readonly property color accentRed:   "#ef5350"
    readonly property color accentKey:   "#5bb6ff"
    readonly property color sidebarSel:  "#0d2e52"

    readonly property int rowH:      24
    readonly property int hdrH:      26
    readonly property int toolbarH:  36
    readonly property int sidebarW:  172

    // ── Fixed column widths ────────────────────────────────────────────────
    readonly property int colStatus: 26
    readonly property int colTime:   56
    readonly property int colBpm:    62
    readonly property int colKey:    50
    readonly property int colKbps:   50
    readonly property int colPad:    14

    // ── Resizable title/artist split ───────────────────────────────────────
    // titleFraction is dragged via the header resize handle.
    property real titleFraction: 0.52
    function flexWidth(viewWidth) {
        return Math.max(0, viewWidth - colStatus - colTime - colBpm - colKey - colKbps - colPad)
    }
    function colTitle(viewWidth) {
        return Math.max(80, Math.round(flexWidth(viewWidth) * titleFraction))
    }
    function colArtist(viewWidth) {
        return Math.max(60, flexWidth(viewWidth) - colTitle(viewWidth))
    }

    // ── Helpers ────────────────────────────────────────────────────────────
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

    function loadPlaylists() {
        allPlaylists = libraryDb ? libraryDb.getAllPlaylists() : []
    }

    function loadPlaylistTracks() {
        playlistTracks = (libraryDb && currentPlaylistId)
            ? libraryDb.getPlaylistTracks(currentPlaylistId)
            : []
        loadPlaylistSort(currentPlaylistId)
    }

    function toggleExpanded(id) {
        var e = Object.assign({}, expandedPlaylists)
        if (e[id]) delete e[id]; else e[id] = true
        expandedPlaylists = e
    }

    // Sidebar playlist tree: flat visible list (pre-order DFS), arbitrary depth.
    readonly property var visiblePlaylists: {
        expandedPlaylists  // reactive dependency
        allPlaylists       // reactive dependency
        var result = []
        // Seed with root-level items sorted by sortOrder
        var roots = allPlaylists.filter(function(p) { return (p.parentId || "") === "" })
                                .sort(function(a, b) { return a.sortOrder - b.sortOrder })
        var queue = roots.map(function(p) { return { item: p, depth: 0 } })
        while (queue.length > 0) {
            var entry = queue.shift()
            var p = entry.item
            var depth = entry.depth
            var hasChildren = allPlaylists.some(function(c) { return (c.parentId || "") === p.id })
            result.push({ id: p.id, name: p.name, trackCount: p.trackCount,
                          parentId: p.parentId || "", sortOrder: p.sortOrder,
                          depth: depth, hasChildren: hasChildren })
            if (hasChildren && expandedPlaylists[p.id]) {
                var kids = allPlaylists.filter(function(c) { return (c.parentId || "") === p.id })
                                       .sort(function(a, b) { return a.sortOrder - b.sortOrder })
                var childEntries = kids.map(function(c) { return { item: c, depth: depth + 1 } })
                queue = childEntries.concat(queue)
            }
        }
        return result
    }

    // ── Per-playlist / all-tracks sort state ──────────────────────────────
    property string playlistSortField: "title"
    property bool   playlistSortAscending: true

    function loadPlaylistSort(playlistId) {
        if (!libraryDb || !playlistId) return
        playlistSortField     = libraryDb.getSetting("pl_" + playlistId + "_sf",  "title")
        playlistSortAscending = libraryDb.getSetting("pl_" + playlistId + "_sa",  "1") === "1"
    }
    function savePlaylistSort(playlistId) {
        if (!libraryDb || !playlistId) return
        libraryDb.setSetting("pl_" + playlistId + "_sf", playlistSortField)
        libraryDb.setSetting("pl_" + playlistId + "_sa", playlistSortAscending ? "1" : "0")
    }

    // Sorted playlist tracks (QML-side, reactive to sort state changes).
    readonly property var sortedPlaylistTracks: {
        playlistTracks; playlistSortField; playlistSortAscending
        var arr = playlistTracks.slice()
        var f = playlistSortField
        var asc = playlistSortAscending
        arr.sort(function(a, b) {
            var va = a[f] !== undefined ? a[f] : ""
            var vb = b[f] !== undefined ? b[f] : ""
            if (f === "durationSec" || f === "bpm" || f === "bitrateKbps") {
                va = Number(va) || 0; vb = Number(vb) || 0
            } else {
                va = String(va).toLowerCase(); vb = String(vb).toLowerCase()
            }
            if (va < vb) return asc ? -1 : 1
            if (va > vb) return asc ? 1 : -1
            return 0
        })
        return arr
    }

    function togglePlaylistSort(field) {
        if (playlistSortField === field)
            playlistSortAscending = !playlistSortAscending
        else {
            playlistSortField = field
            playlistSortAscending = true
        }
        savePlaylistSort(currentPlaylistId)
    }

    Component.onCompleted: {
        loadPlaylists()
        // Restore All Tracks sort (default: title A→Z)
        if (libraryDb && libraryModel) {
            var sf = libraryDb.getSetting("allTracks_sf", "title")
            var sa = libraryDb.getSetting("allTracks_sa", "1") === "1"
            libraryModel.setSort(sf, sa)
        }
    }

    Connections {
        target: libraryDb
        function onPlaylistsChanged() {
            libraryRoot.loadPlaylists()
            if (libraryRoot.activeTab === "playlist")
                libraryRoot.loadPlaylistTracks()
        }
    }

    Connections {
        target: libraryModel
        function onSortChanged() {
            if (libraryDb && libraryModel) {
                libraryDb.setSetting("allTracks_sf", libraryModel.sortField)
                libraryDb.setSetting("allTracks_sa", libraryModel.sortAscending ? "1" : "0")
            }
        }
    }

    // ── Inline component: sortable column header cell ──────────────────────
    component SortHeader: Rectangle {
        id: sh
        required property string field
        required property string label
        property bool centerAlign: false
        property bool isLast: false

        height: parent.height
        color: "transparent"

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.horizontalCenter: sh.centerAlign ? parent.horizontalCenter : undefined
            anchors.left: sh.centerAlign ? undefined : parent.left
            spacing: 4
            Text {
                text: sh.label
                color: libraryModel && libraryModel.sortField === sh.field
                       ? libraryRoot.textPrimary : libraryRoot.textSecond
                font.pixelSize: window.sp(10); font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                visible: libraryModel && libraryModel.sortField === sh.field
                text: (libraryModel && libraryModel.sortAscending) ? "▲" : "▼"
                color: libraryRoot.accentBlue
                font.pixelSize: window.sp(8)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left
            width: sh.isLast ? parent.width : parent.width - 2
            height: 2; color: libraryRoot.accentBlue
            visible: libraryModel && libraryModel.sortField === sh.field
        }
        MouseArea {
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
            onClicked: if (libraryModel) libraryModel.toggleSort(sh.field)
        }
    }

    // ── Inline component: track row (shared between library + playlist) ────
    component TrackRow: Rectangle {
        id: tr
        property int    rowIndex: 0
        property string rowTrackId: ""
        property string rowTitle: ""
        property string rowArtist: ""
        property int    rowDurationSec: 0
        property real   rowBpm: 0
        property string rowKey: ""
        property int    rowBitrateKbps: 0
        property bool   rowIsAnalyzed: false
        property string rowFilePath: ""
        property real viewWidth: parent ? parent.width : 0
        property bool isPlaylistTrack: false  // Set to true when in playlist view
        property string playlistId: ""  // Set when in playlist view

        height: libraryRoot.rowH
        color: trMouse.containsMouse
               ? libraryRoot.bgRowHover
             : (rowIndex % 2 === 0 ? libraryRoot.bgRowEven : libraryRoot.bgRowOdd)
        opacity: trDragPayload.dragging ? 0.4 : 1.0

        Rectangle {
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: 2; color: libraryRoot.accentBlue
            visible: trMouse.containsMouse
        }
        
        // Drop zone indicators for playlist track reordering
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: parent.top
            height: 2; color: libraryRoot.accentGreen
            visible: tr.isPlaylistTrack && trDropArea.containsDrag && trDropArea.dropPosition === 0
        }
        
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2; color: libraryRoot.accentGreen
            visible: tr.isPlaylistTrack && trDropArea.containsDrag && trDropArea.dropPosition === 1
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 6

            Text {
                width: libraryRoot.colStatus
                anchors.verticalCenter: parent.verticalCenter
                text: tr.rowIsAnalyzed ? "●" : "○"
                color: tr.rowIsAnalyzed ? libraryRoot.accentGreen : "#2e2e2e"
                font.pixelSize: window.sp(8)
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                width: libraryRoot.colTitle(tr.viewWidth)
                anchors.verticalCenter: parent.verticalCenter
                text: tr.rowTitle || "—"
                color: libraryRoot.textPrimary
                font.pixelSize: window.sp(11)
                elide: Text.ElideRight
            }
            Text {
                width: libraryRoot.colArtist(tr.viewWidth)
                anchors.verticalCenter: parent.verticalCenter
                text: tr.rowArtist || "—"
                color: libraryRoot.textSecond
                font.pixelSize: window.sp(11)
                elide: Text.ElideRight
            }
            Text {
                width: libraryRoot.colTime
                anchors.verticalCenter: parent.verticalCenter
                text: tr.rowDurationSec > 0
                      ? (Math.floor(tr.rowDurationSec / 60) + ":" + ("0" + (tr.rowDurationSec % 60)).slice(-2))
                      : "—"
                color: tr.rowDurationSec > 0 ? "#888888" : libraryRoot.textDim
                font.pixelSize: window.sp(11); font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                width: libraryRoot.colBpm
                anchors.verticalCenter: parent.verticalCenter
                text: tr.rowBpm > 0 ? tr.rowBpm.toFixed(1) : "—"
                color: tr.rowBpm > 0 ? libraryRoot.accentGreen : libraryRoot.textDim
                font.pixelSize: window.sp(11); font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                width: libraryRoot.colKey
                anchors.verticalCenter: parent.verticalCenter
                text: tr.rowKey || "—"
                color: tr.rowKey ? libraryRoot.accentKey : libraryRoot.textDim
                font.pixelSize: window.sp(11); font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                width: libraryRoot.colKbps
                anchors.verticalCenter: parent.verticalCenter
                text: tr.rowBitrateKbps > 0 ? tr.rowBitrateKbps.toString() : "—"
                color: tr.rowBitrateKbps > 0 ? "#606060" : libraryRoot.textDim
                font.pixelSize: window.sp(10); font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            height: 1; color: libraryRoot.borderSub
        }

        Item {
            id: trDragPayload
            anchors.fill: parent
            property bool dragging: false
            Drag.active:           false
            Drag.dragType:         Drag.Automatic
            Drag.supportedActions: Qt.CopyAction | (tr.isPlaylistTrack ? Qt.MoveAction : Qt.IgnoredAction)
            Drag.keys: tr.isPlaylistTrack ? ["playlist-track-reorder", "text/uri-list"] : ["text/uri-list"]
            Drag.hotSpot.x: tr.width / 2
            Drag.hotSpot.y: tr.height / 2
            Drag.mimeData: tr.isPlaylistTrack
                ? ({
                    "playlist-track-id": tr.rowTrackId,
                    "playlist-id": tr.playlistId,
                    "playlist-track-index": String(tr.rowIndex),
                    "text/uri-list": "file://" + tr.rowFilePath,
                    "text/plain": tr.rowFilePath
                })
                : ({
                    "text/uri-list": "file://" + tr.rowFilePath,
                    "text/plain": tr.rowFilePath
                })
        }
        
        // Drop area for playlist track reordering
        DropArea {
            id: trDropArea
            anchors.fill: parent
            keys: ["playlist-track-reorder"]
            enabled: tr.isPlaylistTrack
            property int dropPosition: 0
            
            onEntered: {
                if (tr.isPlaylistTrack) {
                    var dragIndex = parseInt(drag.getDataAsString("playlist-track-index") || "-1")
                        if (dragIndex >= 0 && dragIndex !== tr.rowIndex) {
                        trDropArea.dropPosition = mouse.y < height / 2 ? 0 : 1
                    }
                }
            }
            
            onDropped: {
                if (tr.isPlaylistTrack && libraryDb) {
                    var dragIndex = parseInt(drop.getDataAsString("playlist-track-index") || "-1")
                    var dragTrackId = drop.getDataAsString("playlist-track-id")
                    var playlistId = tr.playlistId
                    
                    if (dragIndex >= 0 && dragIndex !== tr.rowIndex && dragTrackId && playlistId) {
                        var newPosition = trDropArea.dropPosition === 0 ? tr.rowIndex : tr.rowIndex + 1
                        libraryDb.setPlaylistTrackPosition(playlistId, dragTrackId, newPosition)
                        libraryRoot.loadPlaylistTracks()
                    }
                }
            }
        }

        MouseArea {
            id: trMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: trDragPayload.dragging ? Qt.DragMoveCursor : Qt.PointingHandCursor
            property real pressX: 0; property real pressY: 0

            onPressed: (mouse) => {
                pressX = mouse.x; pressY = mouse.y
                trDragPayload.dragging = false
            }
            onPositionChanged: (mouse) => {
                if (!pressed || trDragPayload.dragging) return
                if (Math.abs(mouse.x - pressX) + Math.abs(mouse.y - pressY) >= 8) {
                    trDragPayload.dragging = true
                    trDragPayload.Drag.active = true
                }
            }
            onReleased: {
                if (trDragPayload.dragging) trDragPayload.Drag.drop()
                trDragPayload.Drag.active = false
                trDragPayload.dragging = false
            }
            onCanceled: {
                trDragPayload.Drag.active = false
                trDragPayload.dragging = false
            }
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton) {
                    libraryRoot.ctxTrackId  = tr.rowTrackId
                    libraryRoot.ctxFilePath = tr.rowFilePath
                    libraryRoot.ctxTitle    = tr.rowTitle
                    trackContextMenu.popup()
                }
            }
        }
    }

    // ── Layout ─────────────────────────────────────────────────────────────
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
            clip: true

            Rectangle {
                anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
                width: 1; color: libraryRoot.borderMain; z: 2
            }

            Flickable {
                anchors.fill: parent
                contentHeight: sidebarColumn.implicitHeight
                clip: true
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                Column {
                    id: sidebarColumn
                    width: parent.width
                    topPadding: 10

                    // ── SAMMLUNG ─────────────────────────────────────────────
                    Item {
                        width: parent.width; height: 18
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 14
                            text: "SAMMLUNG"
                            color: libraryRoot.textDim
                            font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.0
                        }
                    }

                    // All Tracks
                    Rectangle {
                        width: parent.width; height: 30
                        color: libraryRoot.activeTab === "library"
                               ? libraryRoot.bgRowActive
                               : (allMouse.containsMouse ? libraryRoot.bgSidebarHv : "transparent")
                        
                        Behavior on color { ColorAnimation { duration: 150 } }

                        Rectangle {
                            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: 2; color: libraryRoot.accentBlue
                            visible: libraryRoot.activeTab === "library"
                        }
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 16
                            spacing: 9
                            Text {
                                text: "♫"
                                color: libraryRoot.activeTab === "library"
                                       ? libraryRoot.accentBlueLt : libraryRoot.textSecond
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
                            anchors.right: parent.right; anchors.rightMargin: 12
                            width: allBadge.implicitWidth + 10; height: 15; radius: 2
                            color: libraryRoot.activeTab === "library" ? "#1a3a52" : "#1e1e1e"
                            Text {
                                id: allBadge
                                anchors.centerIn: parent
                                text: libraryModel ? libraryModel.count : ""
                                color: libraryRoot.activeTab === "library" ? libraryRoot.accentBlueLt : libraryRoot.textSecond; font.pixelSize: window.sp(9)
                            }
                        }
                        MouseArea {
                            id: allMouse; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                libraryRoot.activeTab = "library"
                                libraryRoot.librarySubTab = "allSongs"
                            }
                        }
                    }

                    // ── PLAYLISTS header ─────────────────────────────────────
                    Item { width: parent.width; height: 14 }

                    Rectangle {
                        width: parent.width; height: 22
                        color: "transparent"

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 14
                            text: "PLAYLISTS"
                            color: libraryRoot.textDim
                            font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.0
                        }

                        // "+" create playlist button
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right; anchors.rightMargin: 10
                            width: 18; height: 18; radius: 2
                            color: addPlaylistHover.containsMouse ? "#2a2a2a" : "transparent"

                            Text {
                                anchors.centerIn: parent; text: "+"
                                color: addPlaylistHover.containsMouse ? libraryRoot.textPrimary : libraryRoot.textSecond
                                font.pixelSize: window.sp(13)
                            }
                            MouseArea {
                                id: addPlaylistHover; anchors.fill: parent
                                hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    createPlaylistDialog.parentId = ""
                                    createPlaylistDialog.open()
                                }
                            }
                        }
                    }

                    // ── Playlist tree ────────────────────────────────────────
                    Repeater {
                        model: libraryRoot.visiblePlaylists

                        Rectangle {
                            id: plRow
                            required property var modelData
                            required property int index

                            // "before" = green line at top, "into" = tinted bg, "after" = green line at bottom
                            property string dropZone: ""

                            readonly property bool isSelected:
                                libraryRoot.activeTab === "playlist" &&
                                libraryRoot.currentPlaylistId === modelData.id

                            width: parent.width; height: 28
                            color: isSelected        ? libraryRoot.bgRowActive
                                 : dropZone === "into" ? "#0d2a18"
                                 : plMouse.containsMouse ? libraryRoot.bgSidebarHv
                                 : "transparent"

                            Behavior on color { ColorAnimation { duration: 100 } }
                            opacity: plMouse.drag.active ? 0.45 : 1.0

                            Rectangle {
                                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                width: 2; color: libraryRoot.accentBlue; visible: isSelected
                            }
                            Rectangle {
                                anchors { left: parent.left; right: parent.right; top: parent.top }
                                height: 2; color: libraryRoot.accentGreen
                                visible: plRow.dropZone === "before"; z: 5
                            }
                            Rectangle {
                                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                                height: 2; color: libraryRoot.accentGreen
                                visible: plRow.dropZone === "after"; z: 5
                            }

                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 14 + modelData.depth * 14
                                spacing: 5
                                Text {
                                    visible: modelData.hasChildren
                                    text: libraryRoot.expandedPlaylists[modelData.id] ? "▾" : "▸"
                                    color: libraryRoot.textSecond; font.pixelSize: window.sp(9)
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Item {
                                    visible: !modelData.hasChildren && modelData.depth > 0
                                    width: 11; height: 1
                                }
                                Text {
                                    text: "☰"
                                    color: isSelected ? libraryRoot.accentBlue : libraryRoot.textDim
                                    font.pixelSize: window.sp(10)
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    text: modelData.name
                                    color: isSelected ? libraryRoot.textPrimary : "#999999"
                                    font.pixelSize: window.sp(12); elide: Text.ElideRight
                                    width: libraryRoot.sidebarW - 90 - modelData.depth * 14
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            Rectangle {
                                visible: modelData.trackCount > 0
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.right: parent.right; anchors.rightMargin: 10
                                width: plBadge.implicitWidth + 8; height: 14; radius: 2
                                color: "#1a1a1a"
                                Text {
                                    id: plBadge; anchors.centerIn: parent
                                    text: modelData.trackCount
                                    color: libraryRoot.textSecond; font.pixelSize: window.sp(8)
                                }
                            }

                            // ── Drag source ───────────────────────────────────
                            // Uses Drag.Internal (no OS drag layer) so no "forbidden"
                            // cursor and no drag.accept() needed on DropAreas.
                            // Data is read from these properties in onDropped via drop.source.
                            Item {
                                id: plDrag
                                x: 0; y: 0
                                width: parent.width; height: parent.height
                                property string dragPlaylistId:       modelData.id
                                property string dragPlaylistParentId: modelData.parentId || ""

                                Drag.active:    plMouse.drag.active
                                Drag.dragType:  Drag.Internal
                                Drag.keys:      ["playlist-reorder"]
                                Drag.hotSpot.x: width  / 2
                                Drag.hotSpot.y: height / 2
                            }

                            // ── Mouse / drag handler ──────────────────────────
                            MouseArea {
                                id: plMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: drag.active ? Qt.DragMoveCursor : Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                drag.target:    plDrag
                                drag.threshold: 8
                                drag.axis:      Drag.XAndYAxis

                                onReleased: {
                                    if (plDrag.Drag.active) plDrag.Drag.drop()
                                    plDrag.x = 0; plDrag.y = 0
                                }
                                onCanceled: { plDrag.x = 0; plDrag.y = 0 }

                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.RightButton) {
                                        playlistContextMenu.targetId       = modelData.id
                                        playlistContextMenu.targetName     = modelData.name
                                        playlistContextMenu.targetParentId = modelData.parentId || ""
                                        playlistContextMenu.popup()
                                        return
                                    }
                                    // Arrow area: toggle expand only, don't select
                                    var arrowX = 14 + modelData.depth * 14
                                    if (modelData.hasChildren && mouse.x >= arrowX && mouse.x < arrowX + 12) {
                                        libraryRoot.toggleExpanded(modelData.id)
                                        return
                                    }
                                    if (modelData.hasChildren && !libraryRoot.expandedPlaylists[modelData.id])
                                        libraryRoot.toggleExpanded(modelData.id)
                                    libraryRoot.currentPlaylistId   = modelData.id
                                    libraryRoot.currentPlaylistName = modelData.name
                                    libraryRoot.activeTab           = "playlist"
                                    libraryRoot.loadPlaylistTracks()
                                }
                            }

                            // ── Drop target ───────────────────────────────────
                            DropArea {
                                id: plDropArea
                                anchors.fill: parent
                                keys: ["playlist-reorder"]

                                function zoneFor(dy) {
                                    return dy < height * 0.3 ? "before"
                                         : dy > height * 0.7 ? "after"
                                         : "into"
                                }

                                onEntered: (drag) => {
                                    if (!drag.source || drag.source.dragPlaylistId === modelData.id) return
                                    plRow.dropZone = zoneFor(drag.y)
                                }
                                onPositionChanged: (drag) => {
                                    if (!drag.source || drag.source.dragPlaylistId === modelData.id) return
                                    plRow.dropZone = zoneFor(drag.y)
                                }
                                onExited: { plRow.dropZone = "" }

                                onDropped: (drop) => {
                                    // Capture everything we need NOW, before any DB call.
                                    // setPlaylistParent emits playlistsChanged → loadPlaylists
                                    // → Repeater rebuilds → THIS delegate is destroyed.
                                    // After that, modelData / libraryRoot / libraryDb are gone
                                    // from this scope. Captured JS vars (db, root, …) stay valid.
                                    var zone      = plRow.dropZone
                                    var targetId  = modelData.id
                                    var tParentId = modelData.parentId || ""
                                    plRow.dropZone = ""
                                    if (!drop.source || !libraryDb) return
                                    var dragId = drop.source.dragPlaylistId
                                    if (!dragId || dragId === targetId) return

                                    var db   = libraryDb
                                    var root = libraryRoot

                                    Qt.callLater(function() {
                                        if (!db || !root) return
                                        if (zone === "into") {
                                            db.setPlaylistParent(dragId, targetId)
                                            var ex = Object.assign({}, root.expandedPlaylists)
                                            ex[targetId] = true
                                            root.expandedPlaylists = ex
                                        } else {
                                            db.setPlaylistParent(dragId, tParentId)
                                            // allPlaylists refreshed synchronously by playlistsChanged
                                            var siblings = root.allPlaylists
                                                .filter(function(p) { return (p.parentId || "") === tParentId })
                                                .sort(function(a, b) { return a.sortOrder - b.sortOrder })
                                            var reordered = siblings.slice()
                                            for (var ri = reordered.length - 1; ri >= 0; ri--) {
                                                if (reordered[ri].id === dragId) { reordered.splice(ri, 1); break }
                                            }
                                            var tPos = -1
                                            for (var ti = 0; ti < reordered.length; ti++) {
                                                if (reordered[ti].id === targetId) { tPos = ti; break }
                                            }
                                            if (tPos < 0) return
                                            reordered.splice(zone === "before" ? tPos : tPos + 1, 0, { id: dragId })
                                            for (var si = 0; si < reordered.length; si++)
                                                db.setPlaylistSortOrder(reordered[si].id, si)
                                        }
                                    })
                                }
                            }
                        }
                    }

                    // ── QUELLEN ──────────────────────────────────────────────
                    Item { width: parent.width; height: 14 }

                    Item {
                        width: parent.width; height: 18
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 14
                            text: "QUELLEN"
                            color: libraryRoot.textDim
                            font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.0
                        }
                    }

                    // Files
                    Rectangle {
                        width: parent.width; height: 30
                        color: libraryRoot.activeTab === "files"
                               ? libraryRoot.sidebarSel
                               : (filesMouse.containsMouse ? "#161616" : "transparent")

                        Rectangle {
                            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: 2; color: libraryRoot.accentBlue
                            visible: libraryRoot.activeTab === "files"
                        }
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 16; spacing: 9
                            Text { text: "📁"; font.pixelSize: window.sp(12); anchors.verticalCenter: parent.verticalCenter }
                            Text {
                                text: "Dateien"
                                color: libraryRoot.activeTab === "files" ? libraryRoot.textPrimary : "#999999"
                                font.pixelSize: window.sp(12); anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        MouseArea {
                            id: filesMouse; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: libraryRoot.activeTab = "files"
                        }
                    }

                    // Streaming
                    Rectangle {
                        width: parent.width; height: 30
                           color: libraryRoot.activeTab === "streaming"
                               ? libraryRoot.bgRowActive
                               : (streamMouse.containsMouse ? libraryRoot.bgSidebarHv : "transparent")
                        
                           Behavior on color { ColorAnimation { duration: 120 } }

                        Rectangle {
                            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: 2; color: libraryRoot.accentBlue
                            visible: libraryRoot.activeTab === "streaming"
                        }
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 16; spacing: 9
                            Text {
                                text: "◉"
                                color: libraryRoot.activeTab === "streaming" ? libraryRoot.accentBlue : libraryRoot.textSecond
                                font.pixelSize: window.sp(13); anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: "Streaming"
                                color: libraryRoot.activeTab === "streaming" ? libraryRoot.textPrimary : "#999999"
                                font.pixelSize: window.sp(12); anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        MouseArea {
                            id: streamMouse; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: libraryRoot.activeTab = "streaming"
                        }
                    }

                    // USB
                    Rectangle {
                        width: parent.width; height: 30
                           color: libraryRoot.activeTab === "usb"
                               ? libraryRoot.bgRowActive
                               : (usbMouse.containsMouse ? libraryRoot.bgSidebarHv : "transparent")
                        
                           Behavior on color { ColorAnimation { duration: 120 } }

                        Rectangle {
                            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: 2; color: libraryRoot.accentBlue
                            visible: libraryRoot.activeTab === "usb"
                        }
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 16; spacing: 9
                            Text {
                                text: "⎘"
                                color: libraryRoot.activeTab === "usb" ? libraryRoot.accentBlueLt : libraryRoot.textSecond
                                font.pixelSize: window.sp(13); anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: "USB"
                                color: libraryRoot.activeTab === "usb" ? libraryRoot.textPrimary : "#999999"
                                font.pixelSize: window.sp(12); anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        MouseArea {
                            id: usbMouse; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: libraryRoot.activeTab = "usb"
                        }
                    }

                    Item { width: parent.width; height: 10 }
                }
            }
        }

        // ════════════════════════════════════════════════════════════════════
        // MAIN CONTENT
        // ════════════════════════════════════════════════════════════════════
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ── Toolbar ────────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: libraryRoot.toolbarH
                color: libraryRoot.bgToolbar

                Rectangle {
                    anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                    height: 1; color: libraryRoot.borderMain
                }

                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 10
                    spacing: 10

                    Text {
                        text: libraryRoot.activeTab === "library"   ? "Bibliothek"
                            : libraryRoot.activeTab === "playlist"  ? libraryRoot.currentPlaylistName
                            : libraryRoot.activeTab === "files"     ? "Dateibrowser"
                            : libraryRoot.activeTab === "streaming" ? "Streaming"
                            : "USB"
                        color: libraryRoot.textSecond
                        font.pixelSize: window.sp(11); font.bold: true
                        Layout.alignment: Qt.AlignVCenter
                    }

                    // Playlist crumb trail
                    Text {
                        visible: libraryRoot.activeTab === "playlist"
                        text: "▸ Playlist"
                        color: libraryRoot.textDim; font.pixelSize: window.sp(10)
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Item { Layout.fillWidth: true }

                    // Search field
                    Rectangle {
                        Layout.preferredWidth: Math.min(280, Math.max(160, parent.width * 0.3))
                        Layout.preferredHeight: 23
                        Layout.alignment: Qt.AlignVCenter
                        color: "#111111"; radius: 3
                        border.color: searchField.activeFocus ? libraryRoot.accentBlue : "#2c2c2c"
                        border.width: 1

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 7
                            spacing: 5
                            visible: !searchField.activeFocus && searchField.text.length === 0
                            Text { text: "🔍"; font.pixelSize: window.sp(9); color: libraryRoot.textSecond; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: "Suche…"; color: libraryRoot.textSecond; font.pixelSize: window.sp(11); anchors.verticalCenter: parent.verticalCenter }
                        }

                        TextField {
                            id: searchField
                            anchors.fill: parent
                            selectByMouse: true; color: libraryRoot.textPrimary
                            placeholderText: ""; font.pixelSize: window.sp(11)
                            leftPadding: 24; topPadding: 0; bottomPadding: 0
                            onTextEdited: libraryRoot.searchText = text
                            Component.onCompleted: text = libraryRoot.searchText
                            background: Item {}
                        }
                    }
                }
            }

            // ── View area ──────────────────────────────────────────────────
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // ════════════════════════════════════════════════
                // A) DB LIBRARY VIEW
                // ════════════════════════════════════════════════
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

                    // ── Column headers ─────────────────────────────────────
                    Rectangle {
                        id: libHeader
                        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                        height: libraryRoot.hdrH; color: libraryRoot.bgHeader

                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 1; color: libraryRoot.borderMain
                        }

                        Row {
                            anchors.fill: parent; anchors.leftMargin: 6

                            Item {
                                width: libraryRoot.colStatus; height: parent.height
                                Text { anchors.centerIn: parent; text: "●"; color: libraryRoot.textDim; font.pixelSize: window.sp(8) }
                            }

                            // TITEL — resizable
                            Item {
                                id: titleHeaderCell
                                width: libraryRoot.colTitle(libraryDbView.width)
                                height: parent.height

                                SortHeader { width: parent.width; height: parent.height; field: "title"; label: "TITEL" }

                                // Resize handle on right edge
                                Rectangle {
                                    id: titleResizeHandle
                                    anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
                                    width: 6; color: "transparent"

                                    Rectangle {
                                        anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
                                        width: 1
                                        color: resizeMa.containsMouse || resizeMa.pressed ? "#44ffffff" : "#1a1a1a"
                                    }

                                    MouseArea {
                                        id: resizeMa; anchors.fill: parent
                                        hoverEnabled: true; cursorShape: Qt.SizeHorCursor
                                        property real startX: 0
                                        property real startFrac: 0
                                        onPressed: (mouse) => {
                                            startX = mapToItem(libraryDbView, mouse.x, 0).x
                                            startFrac = libraryRoot.titleFraction
                                        }
                                        onPositionChanged: (mouse) => {
                                            if (!pressed) return
                                            var dx = mapToItem(libraryDbView, mouse.x, 0).x - startX
                                            var fw = libraryRoot.flexWidth(libraryDbView.width)
                                            if (fw <= 0) return
                                            var newFrac = startFrac + dx / fw
                                            libraryRoot.titleFraction = Math.max(0.2, Math.min(0.8, newFrac))
                                        }
                                    }
                                }
                            }

                            SortHeader { width: libraryRoot.colArtist(libraryDbView.width); height: parent.height; field: "artist"; label: "KÜNSTLER" }
                            SortHeader { width: libraryRoot.colTime;  height: parent.height; field: "time";    label: "ZEIT";    centerAlign: true }
                            SortHeader { width: libraryRoot.colBpm;   height: parent.height; field: "bpm";     label: "BPM";     centerAlign: true }
                            SortHeader { width: libraryRoot.colKey;   height: parent.height; field: "key";     label: "KEY";     centerAlign: true }
                            SortHeader { width: libraryRoot.colKbps;  height: parent.height; field: "kbps";    label: "KBPS";    centerAlign: true; isLast: true }
                        }
                    }

                    // ── Track list ─────────────────────────────────────────
                    ListView {
                        id: libTrackList
                        anchors.top: libHeader.bottom; anchors.left: parent.left
                        anchors.right: parent.right; anchors.bottom: parent.bottom
                        clip: true
                        model: libraryModel ? libraryModel : null
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: TrackRow {
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
                            rowIndex: index
                            rowTrackId: trackId
                            rowTitle: title
                            rowArtist: artist
                            rowDurationSec: durationSec
                            rowBpm: bpm
                            rowKey: key
                            rowBitrateKbps: bitrateKbps
                            rowIsAnalyzed: isAnalyzed
                            rowFilePath: filePath
                            width: ListView.view.width
                            viewWidth: ListView.view.width
                        }

                        // Empty state
                        Column {
                            anchors.centerIn: parent
                            visible: libTrackList.count === 0
                            spacing: 10
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "♫"; color: "#252525"; font.pixelSize: 42 }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Bibliothek ist leer"; color: "#333333"; font.pixelSize: window.sp(12) }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Lade einen Track auf ein Deck, um ihn hinzuzufügen"
                                color: "#282828"; font.pixelSize: window.sp(10)
                            }
                        }
                    }
                }

                // ════════════════════════════════════════════════
                // B) PLAYLIST VIEW
                // ════════════════════════════════════════════════
                Rectangle {
                    id: playlistView
                    anchors.fill: parent
                    color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab === "playlist"

                    // ── Column headers ────────────────────────────────────
                    Rectangle {
                        id: plHeader
                        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                        height: libraryRoot.hdrH; color: libraryRoot.bgHeader

                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 1; color: libraryRoot.borderMain
                        }

                        Row {
                            anchors.fill: parent; anchors.leftMargin: 6
                            spacing: 0

                            Item {
                                width: libraryRoot.colStatus; height: parent.height
                                Text { anchors.centerIn: parent; text: "●"; color: libraryRoot.textDim; font.pixelSize: window.sp(8) }
                            }

                            // TITEL — resizable + sortable
                            Item {
                                width: libraryRoot.colTitle(playlistView.width); height: parent.height
                                Row {
                                    anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; spacing: 4
                                    Text {
                                        text: "TITEL"
                                        color: libraryRoot.playlistSortField === "title" ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        visible: libraryRoot.playlistSortField === "title"
                                        text: libraryRoot.playlistSortAscending ? "▲" : "▼"
                                        color: libraryRoot.accentBlue; font.pixelSize: window.sp(8); anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.left: parent.left
                                    width: parent.width - 2; height: 2; color: libraryRoot.accentBlue
                                    visible: libraryRoot.playlistSortField === "title"
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: libraryRoot.togglePlaylistSort("title")
                                }
                                // Resize handle
                                MouseArea {
                                    anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
                                    width: 6; cursorShape: Qt.SizeHorCursor; hoverEnabled: true
                                    property real startX: 0; property real startFrac: 0
                                    onPressed: (mouse) => { startX = mapToItem(playlistView, mouse.x, 0).x; startFrac = libraryRoot.titleFraction }
                                    onPositionChanged: (mouse) => {
                                        if (!pressed) return
                                        var dx = mapToItem(playlistView, mouse.x, 0).x - startX
                                        var fw = libraryRoot.flexWidth(playlistView.width)
                                        if (fw > 0) libraryRoot.titleFraction = Math.max(0.2, Math.min(0.8, startFrac + dx / fw))
                                    }
                                }
                            }

                            // KÜNSTLER — sortable
                            Item {
                                width: libraryRoot.colArtist(playlistView.width); height: parent.height
                                Row {
                                    anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; spacing: 4
                                    Text {
                                        text: "KÜNSTLER"
                                        color: libraryRoot.playlistSortField === "artist" ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        visible: libraryRoot.playlistSortField === "artist"
                                        text: libraryRoot.playlistSortAscending ? "▲" : "▼"
                                        color: libraryRoot.accentBlue; font.pixelSize: window.sp(8); anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; width: parent.width - 2; height: 2; color: libraryRoot.accentBlue; visible: libraryRoot.playlistSortField === "artist" }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: libraryRoot.togglePlaylistSort("artist") }
                            }

                            // ZEIT — sortable
                            Item {
                                width: libraryRoot.colTime; height: parent.height
                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text {
                                        text: "ZEIT"
                                        color: libraryRoot.playlistSortField === "durationSec" ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text { visible: libraryRoot.playlistSortField === "durationSec"; text: libraryRoot.playlistSortAscending ? "▲" : "▼"; color: libraryRoot.accentBlue; font.pixelSize: window.sp(8); anchors.verticalCenter: parent.verticalCenter }
                                }
                                Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; width: parent.width; height: 2; color: libraryRoot.accentBlue; visible: libraryRoot.playlistSortField === "durationSec" }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: libraryRoot.togglePlaylistSort("durationSec") }
                            }

                            // BPM — sortable
                            Item {
                                width: libraryRoot.colBpm; height: parent.height
                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text {
                                        text: "BPM"
                                        color: libraryRoot.playlistSortField === "bpm" ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text { visible: libraryRoot.playlistSortField === "bpm"; text: libraryRoot.playlistSortAscending ? "▲" : "▼"; color: libraryRoot.accentBlue; font.pixelSize: window.sp(8); anchors.verticalCenter: parent.verticalCenter }
                                }
                                Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; width: parent.width; height: 2; color: libraryRoot.accentBlue; visible: libraryRoot.playlistSortField === "bpm" }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: libraryRoot.togglePlaylistSort("bpm") }
                            }

                            // KEY — sortable
                            Item {
                                width: libraryRoot.colKey; height: parent.height
                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text {
                                        text: "KEY"
                                        color: libraryRoot.playlistSortField === "key" ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text { visible: libraryRoot.playlistSortField === "key"; text: libraryRoot.playlistSortAscending ? "▲" : "▼"; color: libraryRoot.accentBlue; font.pixelSize: window.sp(8); anchors.verticalCenter: parent.verticalCenter }
                                }
                                Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; width: parent.width; height: 2; color: libraryRoot.accentBlue; visible: libraryRoot.playlistSortField === "key" }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: libraryRoot.togglePlaylistSort("key") }
                            }

                            // KBPS — sortable
                            Item {
                                width: libraryRoot.colKbps; height: parent.height
                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text {
                                        text: "KBPS"
                                        color: libraryRoot.playlistSortField === "bitrateKbps" ? libraryRoot.textPrimary : libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text { visible: libraryRoot.playlistSortField === "bitrateKbps"; text: libraryRoot.playlistSortAscending ? "▲" : "▼"; color: libraryRoot.accentBlue; font.pixelSize: window.sp(8); anchors.verticalCenter: parent.verticalCenter }
                                }
                                Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; width: parent.width; height: 2; color: libraryRoot.accentBlue; visible: libraryRoot.playlistSortField === "bitrateKbps" }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: libraryRoot.togglePlaylistSort("bitrateKbps") }
                            }
                        }
                    }

                    ListView {
                        id: plTrackList
                        anchors.top: plHeader.bottom; anchors.left: parent.left
                        anchors.right: parent.right; anchors.bottom: parent.bottom
                        clip: true
                        model: libraryRoot.sortedPlaylistTracks
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: TrackRow {
                            required property var    modelData
                            required property int    index
                            readonly property string trackId:    modelData.trackId    ?? ""
                            readonly property string title:      modelData.title      ?? ""
                            readonly property string artist:     modelData.artist     ?? ""
                            readonly property int    durationSec: modelData.durationSec ?? 0
                            readonly property real   bpm:        modelData.bpm        ?? 0
                            readonly property string key:        modelData.key        ?? ""
                            readonly property int    bitrateKbps: modelData.bitrateKbps ?? 0
                            readonly property bool   isAnalyzed: modelData.isAnalyzed ?? false
                            readonly property string filePath:   modelData.filePath   ?? ""
                            rowIndex: index
                            rowTrackId: trackId
                            rowTitle: title
                            rowArtist: artist
                            rowDurationSec: durationSec
                            rowBpm: bpm
                            rowKey: key
                            rowBitrateKbps: bitrateKbps
                            rowIsAnalyzed: isAnalyzed
                            rowFilePath: filePath
                            width: ListView.view.width
                            viewWidth: ListView.view.width
                            isPlaylistTrack: true
                            playlistId: libraryRoot.currentPlaylistId
                        }

                        // Empty state
                        Column {
                            anchors.centerIn: parent
                            visible: plTrackList.count === 0
                            spacing: 10
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "☰"; color: "#252525"; font.pixelSize: 42 }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Playlist ist leer"; color: "#333333"; font.pixelSize: window.sp(12) }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: 'Rechtsklick auf einen Track → "Zu Playlist hinzufügen"'
                                color: "#282828"; font.pixelSize: window.sp(10)
                            }
                        }
                    }
                }

                // ════════════════════════════════════════════════
                // C) FILE BROWSER
                // ════════════════════════════════════════════════
                Rectangle {
                    anchors.fill: parent; color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab === "files"

                    RowLayout {
                        anchors.fill: parent; spacing: 0

                        Rectangle {
                            Layout.preferredWidth: 200; Layout.fillHeight: true
                            color: libraryRoot.bgSidebar

                            Rectangle {
                                anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
                                width: 1; color: libraryRoot.borderMain
                            }

                            Rectangle {
                                id: folderPaneHdr
                                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                                height: libraryRoot.hdrH; color: libraryRoot.bgHeader

                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                                    height: 1; color: libraryRoot.borderMain
                                }
                                Row {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left; anchors.leftMargin: 10; spacing: 6

                                    Text { text: "ORDNER"; color: libraryRoot.textSecond; font.pixelSize: window.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter }

                                    Rectangle {
                                        visible: libraryManager ? libraryManager.canNavigateUp : false
                                        width: 20; height: 16; radius: 2
                                        color: upMouse.containsMouse ? "#222222" : "transparent"
                                        anchors.verticalCenter: parent.verticalCenter
                                        Text { anchors.centerIn: parent; text: "↑"; color: "#888"; font.pixelSize: window.sp(11) }
                                        MouseArea {
                                            id: upMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: if (libraryManager) libraryManager.navigateUp()
                                        }
                                    }
                                    Text {
                                        text: libraryManager ? (libraryManager.currentFolder.split("/").pop() || "/") : ""
                                        color: "#4a4a4a"; font.pixelSize: window.sp(9)
                                        elide: Text.ElideLeft; width: 110; anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                            }

                            ListView {
                                anchors.top: folderPaneHdr.bottom; anchors.left: parent.left
                                anchors.right: parent.right; anchors.bottom: parent.bottom
                                clip: true; model: libraryManager ? libraryManager.folders : []
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                                delegate: Rectangle {
                                    required property string modelData; required property int index
                                    width: ListView.view.width; height: libraryRoot.rowH
                                    color: folderMouse.containsMouse ? libraryRoot.bgRowHover : "transparent"
                                    Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 2; color: libraryRoot.accentBlue; visible: folderMouse.containsMouse }
                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left; anchors.leftMargin: 10; spacing: 7
                                        Text { text: "📁"; font.pixelSize: window.sp(11); anchors.verticalCenter: parent.verticalCenter }
                                        Text { text: modelData; color: "#c0c0c0"; font.pixelSize: window.sp(11); elide: Text.ElideRight; width: 155; anchors.verticalCenter: parent.verticalCenter }
                                    }
                                    Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; height: 1; color: libraryRoot.borderSub }
                                    MouseArea {
                                        id: folderMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: if (libraryManager) libraryManager.enterFolder(modelData)
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0

                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: libraryRoot.hdrH
                                color: libraryRoot.bgHeader
                                Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; height: 1; color: libraryRoot.borderMain }
                                Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 10; text: "TRACKS"; color: libraryRoot.textSecond; font.pixelSize: window.sp(10); font.bold: true }
                            }

                            ListView {
                                id: trackList
                                Layout.fillWidth: true; Layout.fillHeight: true
                                clip: true; model: libraryRoot.filteredFileTracks
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                                delegate: Rectangle {
                                    id: trackDelegate
                                    required property string modelData; required property int index
                                    width: ListView.view.width; height: libraryRoot.rowH
                                    color: dragArea.containsMouse ? libraryRoot.bgRowHover
                                           : (index % 2 === 0 ? libraryRoot.bgRowEven : libraryRoot.bgRowOdd)
                                    opacity: dragArea.drag.active ? 0.4 : 1.0

                                    Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 2; color: libraryRoot.accentBlue; visible: dragArea.containsMouse }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left; anchors.leftMargin: 12
                                        anchors.right: parent.right; anchors.rightMargin: 8
                                        text: trackDelegate.modelData; color: libraryRoot.textPrimary; font.pixelSize: window.sp(11); elide: Text.ElideRight
                                    }
                                    Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; height: 1; color: libraryRoot.borderSub }

                                    Item {
                                        id: dragPayload; anchors.fill: parent
                                        Drag.active: dragArea.drag.active
                                        Drag.dragType: Drag.Automatic; Drag.supportedActions: Qt.CopyAction
                                        Drag.hotSpot.x: trackDelegate.width / 2; Drag.hotSpot.y: trackDelegate.height / 2
                                        Drag.mimeData: ({
                                            "text/uri-list": "file://" + (libraryManager ? libraryManager.currentFolder : "") + "/" + trackDelegate.modelData,
                                            "text/plain": (libraryManager ? libraryManager.currentFolder : "") + "/" + trackDelegate.modelData
                                        })
                                    }
                                    MouseArea {
                                        id: dragArea; anchors.fill: parent; hoverEnabled: true
                                        cursorShape: drag.active ? Qt.DragMoveCursor : Qt.PointingHandCursor
                                        drag.target: dragPayload; drag.axis: Drag.XAndYAxis; drag.threshold: 6
                                        onReleased: { dragPayload.Drag.drop(); dragPayload.x = 0; dragPayload.y = 0 }
                                    }
                                }

                                Text {
                                    anchors.centerIn: parent; visible: trackList.count === 0
                                    text: "Keine Audiodateien im gewählten Ordner"; color: "#2e2e2e"; font.pixelSize: window.sp(12)
                                }
                            }
                        }
                    }
                }

                // ════════════════════════════════════════════════
                // D) PLACEHOLDER (Streaming / USB)
                // ════════════════════════════════════════════════
                Rectangle {
                    anchors.fill: parent; color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab !== "files" && libraryRoot.activeTab !== "library"
                             && libraryRoot.activeTab !== "playlist"

                    Column {
                        anchors.centerIn: parent; spacing: 12
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: libraryRoot.activeTab === "streaming" ? "◉" : "⎘"; color: "#252525"; font.pixelSize: 44 }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: (libraryRoot.activeTab.charAt(0).toUpperCase() + libraryRoot.activeTab.slice(1)) + " — Demnächst verfügbar"
                            color: "#333333"; font.pixelSize: window.sp(12)
                        }
                    }
                }
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // CONTEXT MENUS
    // ════════════════════════════════════════════════════════════════════════

    // Track right-click menu
    Menu {
        id: trackContextMenu
        background: Rectangle { color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }

        MenuItem {
            text: "Load to Deck A"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: { if (deckA) deckA.loadTrack(libraryRoot.ctxFilePath) }
        }
        MenuItem {
            text: "Load to Deck B"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: { if (deckB) deckB.loadTrack(libraryRoot.ctxFilePath) }
        }
        MenuSeparator {
            contentItem: Rectangle { height: 1; color: "#2a2a2a" }
        }
        Menu {
            id: addToPlaylistMenu
            title: "Zu Playlist hinzufügen"
            background: Rectangle { color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }

            // Show ALL playlists (top-level and sub-crates) with depth indent.
            Repeater {
                model: libraryRoot.visiblePlaylists   // uses existing tree-order list
                MenuItem {
                    required property var modelData
                    text: (modelData.depth > 0 ? "  └ " : "") + modelData.name
                    contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
                    background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
                    onTriggered: {
                        if (libraryDb)
                            libraryDb.addTrackToPlaylist(modelData.id, libraryRoot.ctxTrackId)
                    }
                }
            }

            MenuSeparator {
                contentItem: Rectangle { height: 1; color: "#2a2a2a" }
                visible: libraryRoot.allPlaylists.length > 0
            }

            MenuItem {
                text: "Neue Playlist…"
                contentItem: Text { text: parent.text; color: libraryRoot.accentBlue; font.pixelSize: window.sp(11); leftPadding: 12 }
                background: Rectangle { color: parent.highlighted ? "#1a3a5c" : "transparent" }
                onTriggered: {
                    createPlaylistDialog.parentId = ""
                    createPlaylistDialog.pendingTrackId = libraryRoot.ctxTrackId
                    createPlaylistDialog.open()
                }
            }
        }
        MenuItem {
            text: "Aus Playlist entfernen"
            visible: libraryRoot.activeTab === "playlist" && libraryRoot.currentPlaylistId !== ""
            contentItem: Text { text: parent.text; color: "#e06060"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#3a1a1a" : "transparent" }
            onTriggered: {
                if (libraryDb && libraryRoot.ctxTrackId)
                    libraryDb.removeTrackFromPlaylist(libraryRoot.currentPlaylistId, libraryRoot.ctxTrackId)
            }
        }
        MenuSeparator { contentItem: Rectangle { height: 1; color: "#2a2a2a" } }
        MenuItem {
            text: "Aus Bibliothek entfernen"
            contentItem: Text { text: parent.text; color: "#e06060"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#3a1a1a" : "transparent" }
            onTriggered: {
                removeFromLibraryPopup.trackId    = libraryRoot.ctxTrackId
                removeFromLibraryPopup.trackTitle = libraryRoot.ctxTitle
                removeFromLibraryPopup.open()
            }
        }
    }

    // Playlist sidebar right-click menu
    Menu {
        id: playlistContextMenu
        property string targetId:       ""
        property string targetName:     ""
        property string targetParentId: ""

        background: Rectangle { color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }

        MenuItem {
            text: "Umbenennen"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: {
                renamePlaylistDialog.playlistId = playlistContextMenu.targetId
                renamePlaylistDialog.currentName = playlistContextMenu.targetName
                renamePlaylistDialog.open()
            }
        }
        MenuItem {
            text: "Sub-Crate erstellen"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: {
                createPlaylistDialog.parentId = playlistContextMenu.targetId
                createPlaylistDialog.pendingTrackId = ""
                createPlaylistDialog.open()
                libraryRoot.expandedPlaylists = Object.assign({}, libraryRoot.expandedPlaylists,
                    { [playlistContextMenu.targetId]: true })
            }
        }
        MenuSeparator { contentItem: Rectangle { height: 1; color: "#2a2a2a" } }
        MenuItem {
            text: "Löschen"
            contentItem: Text { text: parent.text; color: "#e06060"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#3a1a1a" : "transparent" }
            onTriggered: {
                deleteConfirmPopup.playlistId   = playlistContextMenu.targetId
                deleteConfirmPopup.playlistName = playlistContextMenu.targetName
                deleteConfirmPopup.open()
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // DIALOGS
    // ════════════════════════════════════════════════════════════════════════

    // Create playlist / sub-crate dialog
    Popup {
        id: createPlaylistDialog
        property string parentId:       ""
        property string pendingTrackId: ""

        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: 270; height: 104
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2

        background: Rectangle { color: "#1e1e1e"; border.color: "#383838"; border.width: 1; radius: 3 }

        Column {
            anchors.fill: parent; anchors.margins: 14; spacing: 8

            Text {
                text: createPlaylistDialog.parentId ? "Sub-Crate erstellen" : "Neue Playlist"
                color: libraryRoot.textPrimary; font.pixelSize: window.sp(12); font.bold: true
            }

            Rectangle {
                width: parent.width; height: 26; radius: 2
                color: "#141414"; border.color: newPlaylistField.activeFocus ? libraryRoot.accentBlue : "#2c2c2c"; border.width: 1

                TextField {
                    id: newPlaylistField
                    anchors.fill: parent
                    color: libraryRoot.textPrimary
                    font.pixelSize: window.sp(11)
                    background: Item {}
                    leftPadding: 8
                    topPadding: 0
                    bottomPadding: 0
                    Keys.onReturnPressed: { createPlaylistDialog.doCreatePlaylist(); createPlaylistDialog.close() }
                    Keys.onEscapePressed: createPlaylistDialog.close()
                }
            }

            Row {
                anchors.right: parent.right; spacing: 6

                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: cancelCreate.containsMouse ? "#2e2e2e" : "#242424"
                    border.color: "#333"; border.width: 1
                    Text { anchors.centerIn: parent; text: "Abbrechen"; color: "#888"; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: cancelCreate; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: createPlaylistDialog.close()
                    }
                }
                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: okCreate.containsMouse ? libraryRoot.accentBlue : "#1d5d9e"
                    Text { anchors.centerIn: parent; text: "Erstellen"; color: "#fff"; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: okCreate; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { createPlaylistDialog.doCreatePlaylist(); createPlaylistDialog.close() }
                    }
                }
            }
        }

        function doCreatePlaylist() {
            var n = newPlaylistField.text.trim()
            if (!n || !libraryDb) return
            var id = libraryDb.createPlaylist(n, createPlaylistDialog.parentId)
            if (id && createPlaylistDialog.pendingTrackId)
                libraryDb.addTrackToPlaylist(id, createPlaylistDialog.pendingTrackId)
        }

        onOpened: { newPlaylistField.text = ""; newPlaylistField.forceActiveFocus() }
    }

    // Rename playlist dialog
    Popup {
        id: renamePlaylistDialog
        property string playlistId:   ""
        property string currentName:  ""

        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: 270; height: 104
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2

        background: Rectangle { color: "#1e1e1e"; border.color: "#383838"; border.width: 1; radius: 3 }

        Column {
            anchors.fill: parent; anchors.margins: 14; spacing: 8

            Text { text: "Playlist umbenennen"; color: libraryRoot.textPrimary; font.pixelSize: window.sp(12); font.bold: true }

            Rectangle {
                width: parent.width; height: 26; radius: 2
                color: "#141414"; border.color: renameField.activeFocus ? libraryRoot.accentBlue : "#2c2c2c"; border.width: 1

                TextField {
                    id: renameField
                    anchors.fill: parent
                    color: libraryRoot.textPrimary
                    font.pixelSize: window.sp(11)
                    background: Item {}
                    leftPadding: 8
                    topPadding: 0
                    bottomPadding: 0
                    Keys.onReturnPressed: { doRename(); renamePlaylistDialog.close() }
                    Keys.onEscapePressed: renamePlaylistDialog.close()
                }
            }

            Row {
                anchors.right: parent.right; spacing: 6

                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: cancelRename.containsMouse ? "#2e2e2e" : "#242424"
                    border.color: "#333"; border.width: 1
                    Text { anchors.centerIn: parent; text: "Abbrechen"; color: "#888"; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: cancelRename; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: renamePlaylistDialog.close()
                    }
                }
                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: okRename.containsMouse ? libraryRoot.accentBlue : "#1d5d9e"
                    Text { anchors.centerIn: parent; text: "Speichern"; color: "#fff"; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: okRename; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { doRename(); renamePlaylistDialog.close() }
                    }
                }
            }
        }

        function doRename() {
            var n = renameField.text.trim()
            if (!n || !libraryDb || !renamePlaylistDialog.playlistId) return
            libraryDb.renamePlaylist(renamePlaylistDialog.playlistId, n)
            if (libraryRoot.currentPlaylistId === renamePlaylistDialog.playlistId)
                libraryRoot.currentPlaylistName = n
        }

        onOpened: { renameField.text = currentName; renameField.forceActiveFocus(); renameField.selectAll() }
    }

    // Remove from Library confirmation popup
    Popup {
        id: removeFromLibraryPopup
        property string trackId:    ""
        property string trackTitle: ""

        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: 300; height: 120
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2

        background: Rectangle { color: "#1e1e1e"; border.color: "#383838"; border.width: 1; radius: 3 }

        Column {
            anchors.fill: parent; anchors.margins: 14; spacing: 10

            Text { text: "Aus Bibliothek entfernen?"; color: libraryRoot.textPrimary; font.pixelSize: window.sp(12); font.bold: true }
            Text {
                width: parent.width
                text: '"' + removeFromLibraryPopup.trackTitle + '" wird aus der Bibliothek entfernt. Die Datei wird nicht gelöscht.'
                color: libraryRoot.textSecond; font.pixelSize: window.sp(10); wrapMode: Text.Wrap
            }

            Row {
                anchors.right: parent.right; spacing: 6

                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: cancelRmLib.containsMouse ? "#2e2e2e" : "#242424"
                    border.color: "#333"; border.width: 1
                    Text { anchors.centerIn: parent; text: "Abbrechen"; color: "#888"; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: cancelRmLib; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: removeFromLibraryPopup.close()
                    }
                }
                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: okRmLib.containsMouse ? "#c0392b" : "#8b2020"
                    Text { anchors.centerIn: parent; text: "Entfernen"; color: "#fff"; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: okRmLib; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (libraryDb) libraryDb.removeTrackFromLibrary(removeFromLibraryPopup.trackId)
                            if (libraryRoot.activeTab === "playlist")
                                libraryRoot.loadPlaylistTracks()
                            removeFromLibraryPopup.close()
                        }
                    }
                }
            }
        }
    }

    // Delete confirmation popup
    Popup {
        id: deleteConfirmPopup
        property string playlistId:   ""
        property string playlistName: ""

        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: 270; height: 110
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2

        background: Rectangle { color: "#1e1e1e"; border.color: "#383838"; border.width: 1; radius: 3 }

        Column {
            anchors.fill: parent; anchors.margins: 14; spacing: 10

            Text { text: "Playlist löschen?"; color: libraryRoot.textPrimary; font.pixelSize: window.sp(12); font.bold: true }
            Text {
                width: parent.width
                text: '"' + deleteConfirmPopup.playlistName + '" wird dauerhaft gelöscht.'
                color: libraryRoot.textSecond; font.pixelSize: window.sp(10); wrapMode: Text.Wrap
            }

            Row {
                anchors.right: parent.right; spacing: 6

                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: cancelDel.containsMouse ? "#2e2e2e" : "#242424"
                    border.color: "#333"; border.width: 1
                    Text { anchors.centerIn: parent; text: "Abbrechen"; color: "#888"; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: cancelDel; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: deleteConfirmPopup.close()
                    }
                }
                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: okDel.containsMouse ? "#c0392b" : "#8b2020"
                    Text { anchors.centerIn: parent; text: "Löschen"; color: "#fff"; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: okDel; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (libraryDb) libraryDb.deletePlaylist(deleteConfirmPopup.playlistId)
                            if (libraryRoot.currentPlaylistId === deleteConfirmPopup.playlistId) {
                                libraryRoot.currentPlaylistId = ""
                                libraryRoot.currentPlaylistName = ""
                                libraryRoot.activeTab = "library"
                            }
                            deleteConfirmPopup.close()
                        }
                    }
                }
            }
        }
    }
}
