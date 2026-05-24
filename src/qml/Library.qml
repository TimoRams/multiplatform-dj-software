import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: libraryRoot
    color: "#141414"
    focus: true

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

    // ── Browser cursor + focus system ─────────────────────────────────────
    // focusedPanel: which panel receives keyboard input
    //   "tracks"  — main track list (default)
    //   "sidebar" — playlist tree
    //   "search"  — search TextField has active focus
    property string focusedPanel: "tracks"

    property int  browserCursorIndex:  -1
    property bool browserCursorActive: false

    property int  sidebarCursorIndex:  -1
    property bool sidebarCursorActive: false

    // ── Active track selection (single source of truth) ─────────────────
    property string activeTrackId:       ""
    property string activeTrackFilePath: ""
    property int    activeTrackIndex:    -1
    property string activeTrackSource:   ""   // "library" | "playlist"

    // ── Sidebar keyboard navigation map ─────────────────────────────────
    readonly property int sidebarTopNavCount: 1   // All Tracks
    readonly property int sidebarBottomNavCount: 3 // Files / Streaming / USB
    readonly property int sidebarPlaylistStartIndex: sidebarTopNavCount

    onActiveTabChanged: {
        syncSidebarCursorToSelection()
        ensureActiveTrackForCurrentTab()
    }

    onVisiblePlaylistsChanged: {
        syncSidebarCursorToSelection()
    }

    onCurrentPlaylistIdChanged: {
        syncSidebarCursorToSelection()
    }

    onPlaylistTracksChanged: {
        if (activeTab === "playlist") ensureActiveTrackForCurrentTab()
    }

    onPlaylistSortFieldChanged: {
        if (activeTab === "playlist") ensureActiveTrackForCurrentTab()
    }

    onPlaylistSortAscendingChanged: {
        if (activeTab === "playlist") ensureActiveTrackForCurrentTab()
    }

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
    readonly property color textMeta:    "#888888"
    readonly property color textNav:     "#aaaaaa"

    readonly property int rowH:       24
    readonly property int rowHNormal: 56
    readonly property int hdrH:       26
    readonly property int toolbarH:   36
    readonly property int sidebarW:   200
    property string viewMode: "compact"

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

    function _isContextClick(mouse) {
        return mouse.button === Qt.RightButton
            || (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ControlModifier))
    }

    function _popupMenuAt(menu) {
        Qt.callLater(function() { menu.popup() })
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

    function startAnalyzeCurrentView() {
        if (!libraryAnalyzer) return
        if (activeTab === "playlist" && currentPlaylistId)
            libraryAnalyzer.analyzePlaylist(currentPlaylistId, true)
        else
            libraryAnalyzer.analyzeAll(true)
    }

    // ── Cursor navigation ──────────────────────────────────────────────────

    function sidebarTotalCount() {
        return sidebarTopNavCount + visiblePlaylists.length + sidebarBottomNavCount
    }

    function sidebarIndexForTab(tab) {
        if (tab === "library") return 0
        var bottomStart = sidebarTopNavCount + visiblePlaylists.length
        if (tab === "files") return bottomStart
        if (tab === "streaming") return bottomStart + 1
        if (tab === "usb") return bottomStart + 2
        return -1
    }

    function _playlistIndexForId(playlistId) {
        if (!playlistId) return -1
        for (var i = 0; i < visiblePlaylists.length; i++) {
            if (visiblePlaylists[i].id === playlistId) return i
        }
        return -1
    }

    function sidebarEntryAt(index) {
        if (index < 0) return null

        var bottomStart = sidebarTopNavCount + visiblePlaylists.length
        if (index === 0) {
            return { type: "nav", tab: "library", item: navAllTracks }
        }

        var plIndex = index - sidebarTopNavCount
        if (plIndex >= 0 && plIndex < visiblePlaylists.length) {
            return {
                type: "playlist",
                data: visiblePlaylists[plIndex],
                item: sidebarPlaylistRepeater.itemAt(plIndex)
            }
        }

        var bottomIndex = index - bottomStart
        if (bottomIndex === 0) return { type: "nav", tab: "files", item: navFiles }
        if (bottomIndex === 1) return { type: "nav", tab: "streaming", item: navStreaming }
        if (bottomIndex === 2) return { type: "nav", tab: "usb", item: navUsb }

        return null
    }

    function _applyActiveTrack(index, trackId, filePath, source) {
        activeTrackIndex = index
        activeTrackId = trackId || ""
        activeTrackFilePath = filePath || ""
        activeTrackSource = source || ""
    }

    function setActiveTrackFromRow(index, trackId, filePath, source) {
        browserCursorIndex = index
        browserCursorActive = true
        _applyActiveTrack(index, trackId, filePath, source || activeTab)
    }

    function _playlistIndexForTrackId(trackId) {
        if (!trackId) return -1
        for (var i = 0; i < sortedPlaylistTracks.length; i++) {
            if (sortedPlaylistTracks[i].trackId === trackId) return i
        }
        return -1
    }

    function syncActiveTrackFromCursor() {
        if (!browserCursorActive || browserCursorIndex < 0) {
            _applyActiveTrack(-1, "", "", "")
            return
        }
        if (activeTab === "playlist") {
            var t = sortedPlaylistTracks[browserCursorIndex]
            _applyActiveTrack(browserCursorIndex, t ? t.trackId : "", t ? t.filePath : "", "playlist")
            return
        }
        if (activeTab === "library") {
            var id = libraryModel ? libraryModel.trackIdAtRow(browserCursorIndex) : ""
            var fp = libraryModel ? libraryModel.filePathAtRow(browserCursorIndex) : ""
            _applyActiveTrack(browserCursorIndex, id, fp, "library")
        }
    }

    function ensureActiveTrackForCurrentTab() {
        if (activeTab !== "library" && activeTab !== "playlist") {
            browserCursorActive = false
            _applyActiveTrack(-1, "", "", "")
            return
        }

        var list = (activeTab === "playlist") ? plTrackList : libTrackList
        var count = list ? list.count : 0
        if (count === 0) {
            browserCursorActive = false
            _applyActiveTrack(-1, "", "", "")
            return
        }

        var idx = -1
        if (activeTrackId) {
            if (activeTab === "playlist") {
                idx = _playlistIndexForTrackId(activeTrackId)
            } else if (libraryModel) {
                idx = libraryModel.indexOfTrackId(activeTrackId)
            }
        }
        if (idx < 0) idx = 0

        browserCursorIndex = idx
        browserCursorActive = true
        syncActiveTrackFromCursor()
        if (list) list.positionViewAtIndex(idx, ListView.Contain)
    }

    function syncSidebarCursorToSelection() {
        var idx = -1
        if (activeTab === "playlist" && currentPlaylistId) {
            var plIdx = _playlistIndexForId(currentPlaylistId)
            if (plIdx >= 0) idx = sidebarPlaylistStartIndex + plIdx
        }
        if (idx < 0) idx = sidebarIndexForTab(activeTab)
        if (idx < 0 && activeTab === "playlist") idx = 0
        if (idx >= 0) {
            sidebarCursorIndex = idx
            sidebarCursorActive = true
            _ensureSidebarVisible(idx)
        }
    }

    function moveCursor(rawDelta) {
        if (activeTab !== "library" && activeTab !== "playlist") return
        var list = (activeTab === "playlist") ? plTrackList : libTrackList
        var count = list.count
        if (count === 0) { browserCursorActive = false; return }

        var absD = Math.abs(rawDelta)
        var steps = absD <= 1 ? 1 : (absD <= 3 ? absD * 2 : absD * 3)
        steps = Math.min(steps, 24)
        var delta = rawDelta >= 0 ? steps : -steps

        var newIdx
        if (!browserCursorActive) {
            newIdx = delta >= 0 ? 0 : count - 1
        } else {
            newIdx = Math.max(0, Math.min(count - 1, browserCursorIndex + delta))
        }
        browserCursorIndex = newIdx
        browserCursorActive = true
        syncActiveTrackFromCursor()
        list.positionViewAtIndex(newIdx, ListView.Contain)
    }

    function getCursorFilePath() {
        if (!browserCursorActive || browserCursorIndex < 0) return ""
        if (activeTrackIndex !== browserCursorIndex) syncActiveTrackFromCursor()
        return activeTrackFilePath || ""
    }

    function selectNextPlaylist(direction) {
        if (visiblePlaylists.length === 0) return
        var curIdx = -1
        for (var i = 0; i < visiblePlaylists.length; i++) {
            if (visiblePlaylists[i].id === currentPlaylistId) { curIdx = i; break }
        }
        var newIdx = curIdx + direction
        if (newIdx < 0) newIdx = visiblePlaylists.length - 1
        if (newIdx >= visiblePlaylists.length) newIdx = 0
        var pl = visiblePlaylists[newIdx]
        currentPlaylistId   = pl.id
        currentPlaylistName = pl.name
        activeTab           = "playlist"
        loadPlaylistTracks()
        syncSidebarCursorToSelection()
    }

    // ── Sidebar cursor ─────────────────────────────────────────────────────

    function moveSidebarCursor(delta) {
        var count = sidebarTotalCount()
        if (count === 0) { sidebarCursorActive = false; return }
        var newIdx
        if (!sidebarCursorActive) {
            newIdx = delta >= 0 ? 0 : count - 1
        } else {
            newIdx = Math.max(0, Math.min(count - 1, sidebarCursorIndex + delta))
        }
        sidebarCursorIndex = newIdx
        sidebarCursorActive = true
        _ensureSidebarVisible(newIdx)
    }

    function _ensureSidebarVisible(index) {
        var entry = sidebarEntryAt(index)
        if (!entry || !entry.item) return
        var item = entry.item
        var itemY  = item.y
        var itemH  = item.height
        var viewY  = sidebarFlickable.contentY
        var viewH  = sidebarFlickable.height
        if (itemY < viewY)
            sidebarFlickable.contentY = Math.max(0, itemY - 4)
        else if (itemY + itemH > viewY + viewH)
            sidebarFlickable.contentY = itemY + itemH - viewH + 4
    }

    function selectSidebarItem() {
        if (!sidebarCursorActive || sidebarCursorIndex < 0) return
        var entry = sidebarEntryAt(sidebarCursorIndex)
        if (!entry) return
        if (entry.type === "nav") {
            activeTab = entry.tab
            if (entry.tab === "library")
                librarySubTab = "allSongs"
            focusedPanel = "sidebar"
            forceActiveFocus()
            return
        }
        var pl = entry.data
        if (pl && pl.hasChildren) {
            toggleExpanded(pl.id)
        } else if (pl) {
            currentPlaylistId   = pl.id
            currentPlaylistName = pl.name
            activeTab           = "playlist"
            loadPlaylistTracks()
        }
        focusedPanel = "sidebar"
        forceActiveFocus()
    }

    function expandSidebarItem() {
        if (!sidebarCursorActive || sidebarCursorIndex < 0) return
        var entry = sidebarEntryAt(sidebarCursorIndex)
        if (!entry || entry.type !== "playlist") return
        var pl = entry.data
        if (pl && pl.hasChildren && !expandedPlaylists[pl.id])
            toggleExpanded(pl.id)
    }

    function collapseSidebarItem() {
        if (!sidebarCursorActive || sidebarCursorIndex < 0) return
        var entry = sidebarEntryAt(sidebarCursorIndex)
        if (!entry || entry.type !== "playlist") return
        var pl = entry.data
        if (!pl) return
        if (expandedPlaylists[pl.id]) {
            toggleExpanded(pl.id)
        } else if (pl.parentId) {
            var parentIdx = _playlistIndexForId(pl.parentId)
            if (parentIdx >= 0) {
                sidebarCursorIndex = sidebarPlaylistStartIndex + parentIdx
                sidebarCursorActive = true
                _ensureSidebarVisible(sidebarCursorIndex)
            }
        }
    }

    // ── Panel focus management ─────────────────────────────────────────────

    function cyclePanel(direction) {
        var panels = ["tracks", "sidebar", "search"]
        var idx = panels.indexOf(focusedPanel)
        if (idx < 0) idx = 0
        idx = (idx + direction + panels.length) % panels.length
        setFocusedPanel(panels[idx])
    }

    function setFocusedPanel(panel) {
        focusedPanel = panel
        if (panel === "search") {
            searchField.forceActiveFocus()
        } else {
            forceActiveFocus()
            if (panel === "sidebar") {
                syncSidebarCursorToSelection()
            } else if (panel === "tracks") {
                ensureActiveTrackForCurrentTab()
            }
        }
    }

    Component.onCompleted: {
        loadPlaylists()
        if (libraryDb) {
            var vm = libraryDb.getSetting("library_view_mode", viewMode)
            if (vm === "compact" || vm === "normal") viewMode = vm
        }
        // Restore All Tracks sort (default: title A→Z)
        if (libraryDb && libraryModel) {
            var sf = libraryDb.getSetting("allTracks_sf", "title")
            var sa = libraryDb.getSetting("allTracks_sa", "1") === "1"
            libraryModel.setSort(sf, sa)
        }
        syncSidebarCursorToSelection()
        ensureActiveTrackForCurrentTab()
    }

    onViewModeChanged: {
        if (libraryDb && (viewMode === "compact" || viewMode === "normal"))
            libraryDb.setSetting("library_view_mode", viewMode)
    }

    Connections {
        target: libraryDb
        function onPlaylistsChanged() {
            libraryRoot.loadPlaylists()
            if (libraryRoot.activeTab === "playlist")
                libraryRoot.loadPlaylistTracks()
        }
        function onAnalysisUpdated(trackId) {
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
        function onCountChanged() {
            if (libraryRoot.activeTab === "library")
                libraryRoot.ensureActiveTrackForCurrentTab()
        }
    }

    // ── MIDI browser navigation ────────────────────────────────────────────
    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if (id === "library_browse") {
                if (value !== 0) libraryRoot.moveCursor(value)
            } else if (id === "library_load_deck_a") {
                if (value > 0) {
                    var fpA = libraryRoot.getCursorFilePath()
                    if (fpA && deckA) deckA.loadTrack(fpA)
                }
            } else if (id === "library_load_deck_b") {
                if (value > 0) {
                    var fpB = libraryRoot.getCursorFilePath()
                    if (fpB && deckB) deckB.loadTrack(fpB)
                }
            } else if (id === "library_playlist_next") {
                if (value > 0) libraryRoot.selectNextPlaylist(1)
            } else if (id === "library_playlist_prev") {
                if (value > 0) libraryRoot.selectNextPlaylist(-1)
            } else if (id === "library_back") {
                if (value > 0) libraryRoot.activeTab = "library"
            } else if (id === "library_expand") {
                if (value > 0 && libraryRoot.currentPlaylistId)
                    libraryRoot.toggleExpanded(libraryRoot.currentPlaylistId)
            } else if (id === "library_collapse") {
                if (value > 0 && libraryRoot.currentPlaylistId) {
                    var ex = Object.assign({}, libraryRoot.expandedPlaylists)
                    delete ex[libraryRoot.currentPlaylistId]
                    libraryRoot.expandedPlaylists = ex
                }
            }
        }
    }

    // ── Keyboard navigation ────────────────────────────────────────────────
    Keys.onPressed: (event) => {
        // Tab / Shift+Tab cycle focus between panels
        if (event.key === Qt.Key_Tab) {
            cyclePanel(1); event.accepted = true; return
        }
        if (event.key === Qt.Key_Backtab) {
            cyclePanel(-1); event.accepted = true; return
        }

        if (focusedPanel === "tracks") {
            if (event.key === Qt.Key_Up) {
                moveCursor(-1); event.accepted = true
            } else if (event.key === Qt.Key_Down) {
                moveCursor(1); event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                var fp = getCursorFilePath()
                if (fp && deckA) deckA.loadTrack(fp)
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                browserCursorActive = false; event.accepted = true
            }
        } else if (focusedPanel === "sidebar") {
            if (event.key === Qt.Key_Up) {
                moveSidebarCursor(-1); event.accepted = true
            } else if (event.key === Qt.Key_Down) {
                moveSidebarCursor(1); event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                selectSidebarItem(); event.accepted = true
            } else if (event.key === Qt.Key_Right) {
                expandSidebarItem(); event.accepted = true
            } else if (event.key === Qt.Key_Left) {
                collapseSidebarItem(); event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                sidebarCursorActive = false
                focusedPanel = "tracks"
                event.accepted = true
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

    // ── Inline component: sortable playlist column header cell ───────────────
    component PlSortHeader: Rectangle {
        id: psh
        required property string field
        required property string label
        property bool centerAlign: false
        property bool isLast: false

        height: parent.height
        color: "transparent"

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.horizontalCenter: psh.centerAlign ? parent.horizontalCenter : undefined
            anchors.left: psh.centerAlign ? undefined : parent.left
            spacing: 4
            Text {
                text: psh.label
                color: libraryRoot.playlistSortField === psh.field
                       ? libraryRoot.textPrimary : libraryRoot.textSecond
                font.pixelSize: window.sp(10); font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                visible: libraryRoot.playlistSortField === psh.field
                text: libraryRoot.playlistSortAscending ? "▲" : "▼"
                color: libraryRoot.accentBlue
                font.pixelSize: window.sp(8)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left
            width: psh.isLast ? parent.width : parent.width - 2
            height: 2; color: libraryRoot.accentBlue
            visible: libraryRoot.playlistSortField === psh.field
        }
        MouseArea {
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
            onClicked: libraryRoot.togglePlaylistSort(psh.field)
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

        readonly property bool isCursorRow: libraryRoot.browserCursorActive
                                           && libraryRoot.browserCursorIndex === rowIndex
                                           && (isPlaylistTrack
                                               ? libraryRoot.activeTab === "playlist"
                                               : libraryRoot.activeTab === "library")

        readonly property color artBgColor: {
            var s = rowTitle || rowArtist || ""
            if (s.length === 0) return "#1a2535"
            var h = 0
            for (var i = 0; i < s.length && i < 20; i++) h = (h * 31 + s.charCodeAt(i)) & 0xFFFF
            return Qt.hsla((h % 360) / 360.0, 0.42, 0.20, 1.0)
        }

        height: libraryRoot.viewMode === "normal" ? libraryRoot.rowHNormal : libraryRoot.rowH
        color: trMouse.containsMouse
               ? libraryRoot.bgRowHover
             : (isCursorRow
                ? "#163328"
                : (rowIndex % 2 === 0 ? libraryRoot.bgRowEven : libraryRoot.bgRowOdd))
        opacity: trDragPayload.dragging ? 0.4 : 1.0

        Rectangle {
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: 2
            color: tr.isCursorRow ? libraryRoot.accentGreen : libraryRoot.accentBlue
            visible: trMouse.containsMouse || tr.isCursorRow
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

        // ── Compact layout ────────────────────────────────────────────────────
        Row {
            visible: libraryRoot.viewMode === "compact"
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
                color: tr.rowDurationSec > 0 ? libraryRoot.textMeta : libraryRoot.textDim
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
                color: tr.rowBitrateKbps > 0 ? libraryRoot.textSecond : libraryRoot.textDim
                font.pixelSize: window.sp(10); font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // ── Normal layout ─────────────────────────────────────────────────────
        Item {
            visible: libraryRoot.viewMode === "normal"
            anchors.fill: parent

            // Art placeholder
            Rectangle {
                id: trArtBox
                anchors.left: parent.left; anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 44; height: 44; radius: 5
                color: tr.artBgColor

                Rectangle {
                    anchors.top: parent.top; anchors.right: parent.right
                    anchors.topMargin: 3; anchors.rightMargin: 3
                    width: 8; height: 8; radius: 4
                    color: tr.rowIsAnalyzed ? libraryRoot.accentGreen : "#252525"
                    border.color: Qt.rgba(0, 0, 0, 0.4); border.width: 1
                }

                Text {
                    anchors.centerIn: parent
                    text: (tr.rowTitle || tr.rowArtist || "?").charAt(0).toUpperCase()
                    color: Qt.rgba(1, 1, 1, 0.60)
                    font.pixelSize: window.sp(17); font.bold: true
                }
            }

            // Title + Artist column
            Column {
                anchors.left: trArtBox.right; anchors.leftMargin: 12
                anchors.right: trInfoCol.left; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 5

                Text {
                    width: parent.width
                    text: tr.rowTitle || "—"
                    color: libraryRoot.textPrimary
                    font.pixelSize: window.sp(12); font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: tr.rowArtist || "—"
                    color: libraryRoot.textSecond
                    font.pixelSize: window.sp(10)
                    elide: Text.ElideRight
                }
            }

            // Right info column
            Column {
                id: trInfoCol
                anchors.right: parent.right; anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Text {
                    anchors.right: parent.right
                    text: tr.rowDurationSec > 0
                          ? (Math.floor(tr.rowDurationSec / 60) + ":" + ("0" + (tr.rowDurationSec % 60)).slice(-2))
                          : "—"
                    color: tr.rowDurationSec > 0 ? libraryRoot.textMeta : libraryRoot.textDim
                    font.pixelSize: window.sp(11); font.family: "monospace"
                }

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    Text {
                        text: tr.rowBpm > 0 ? tr.rowBpm.toFixed(1) : "—"
                        color: tr.rowBpm > 0 ? libraryRoot.accentGreen : libraryRoot.textDim
                        font.pixelSize: window.sp(10); font.family: "monospace"
                    }
                    Text {
                        text: tr.rowKey || "—"
                        color: tr.rowKey ? libraryRoot.accentKey : libraryRoot.textDim
                        font.pixelSize: window.sp(10); font.family: "monospace"
                    }
                }
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
            property real pressX: 0
            property real pressY: 0
            property bool rightDragging: false

            onPressed: (mouse) => {
                libraryRoot.setActiveTrackFromRow(
                    tr.rowIndex,
                    tr.rowTrackId,
                    tr.rowFilePath,
                    tr.isPlaylistTrack ? "playlist" : "library")
                libraryRoot.focusedPanel = "tracks"
                libraryRoot.forceActiveFocus()

                pressX = mouse.x
                pressY = mouse.y
                if (mouse.button === Qt.RightButton) {
                    rightDragging = false
                    trDragPayload.dragging = false
                } else if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ControlModifier)) {
                    libraryRoot.ctxTrackId  = tr.rowTrackId
                    libraryRoot.ctxFilePath = tr.rowFilePath
                    libraryRoot.ctxTitle    = tr.rowTitle
                    libraryRoot._popupMenuAt(trackContextMenu)
                } else {
                    trDragPayload.dragging = false
                }
            }
            onPositionChanged: (mouse) => {
                if (!pressed) return
                var moved = Math.abs(mouse.x - pressX) + Math.abs(mouse.y - pressY) >= 8
                if (!trDragPayload.dragging && moved) {
                    trDragPayload.dragging = true
                    trDragPayload.Drag.active = true
                    if (mouse.buttons & Qt.RightButton) rightDragging = true
                }
            }
            onReleased: (mouse) => {
                if (trDragPayload.dragging) {
                    trDragPayload.Drag.drop()
                } else if (mouse.button === Qt.RightButton && !rightDragging) {
                    libraryRoot.ctxTrackId  = tr.rowTrackId
                    libraryRoot.ctxFilePath = tr.rowFilePath
                    libraryRoot.ctxTitle    = tr.rowTitle
                    libraryRoot._popupMenuAt(trackContextMenu)
                }
                trDragPayload.Drag.active = false
                trDragPayload.dragging = false
                rightDragging = false
            }
            onCanceled: {
                trDragPayload.Drag.active = false
                trDragPayload.dragging = false
                rightDragging = false
            }
            onClicked: (mouse) => {
                if (mouse.button === Qt.LeftButton && !(mouse.modifiers & Qt.ControlModifier)) {
                    libraryRoot.focusedPanel = "tracks"
                    libraryRoot.forceActiveFocus()
                }
            }
        }
    }

    // ── Inline component: sidebar nav button ──────────────────────────────
    component NavButton: Rectangle {
        id: navBtn
        required property string tabKey
        required property string btnIcon
        required property string btnLabel
        property int badgeCount: -1
        property var customAction: null
         property int cursorIndex: -1

         readonly property bool isCursor: libraryRoot.focusedPanel === "sidebar"
                              && libraryRoot.sidebarCursorActive
                              && libraryRoot.sidebarCursorIndex === cursorIndex

        width: parent ? parent.width : 0; height: 42
         color: libraryRoot.activeTab === tabKey
             ? libraryRoot.sidebarSel
             : (isCursor ? "#0f2816" : (navMa.containsMouse ? libraryRoot.bgSidebarHv : "transparent"))
        Behavior on color { ColorAnimation { duration: 120 } }

        Rectangle {
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: 3; color: libraryRoot.accentBlue
            visible: libraryRoot.activeTab === navBtn.tabKey
        }

        Rectangle {
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: 3; color: libraryRoot.accentGreen
            visible: navBtn.isCursor && libraryRoot.activeTab !== navBtn.tabKey
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 14
            spacing: 10

            Rectangle {
                width: 26; height: 26; radius: 5
                color: libraryRoot.activeTab === navBtn.tabKey ? "#132840" : "#181818"
                border.color: libraryRoot.activeTab === navBtn.tabKey ? "#1e4070" : "#222222"
                border.width: 1
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    anchors.centerIn: parent
                    text: navBtn.btnIcon
                    color: libraryRoot.activeTab === navBtn.tabKey
                           ? libraryRoot.accentBlueLt : libraryRoot.textSecond
                    font.pixelSize: window.sp(11)
                }
            }

            Text {
                text: navBtn.btnLabel
                color: libraryRoot.activeTab === navBtn.tabKey
                       ? libraryRoot.textPrimary : libraryRoot.textNav
                font.pixelSize: window.sp(12)
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Rectangle {
            id: navBadgeRect
            visible: navBtn.badgeCount > 0
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right; anchors.rightMargin: 10
            width: Math.max(24, navBadgeLabel.implicitWidth + 10); height: 16; radius: 8
            color: libraryRoot.activeTab === navBtn.tabKey ? "#1a3a52" : "#202020"
            border.color: libraryRoot.activeTab === navBtn.tabKey ? "#2a5070" : "#282828"
            border.width: 1

            Text {
                id: navBadgeLabel
                anchors.centerIn: parent
                text: navBtn.badgeCount > 0 ? navBtn.badgeCount : ""
                color: libraryRoot.activeTab === navBtn.tabKey
                       ? libraryRoot.accentBlueLt : "#666666"
                font.pixelSize: window.sp(9)
            }
        }

        MouseArea {
            id: navMa; anchors.fill: parent; hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                libraryRoot.activeTab = navBtn.tabKey
                if (navBtn.customAction) navBtn.customAction()
                libraryRoot.sidebarCursorIndex = navBtn.cursorIndex
                libraryRoot.sidebarCursorActive = true
                libraryRoot.focusedPanel = "sidebar"
                libraryRoot.forceActiveFocus()
            }
        }
    }

    // ── Inline component: sort pill button (normal view sort bar) ────────────
    component SortPill: Rectangle {
        id: pill
        required property string label
        required property bool   isActive
        required property bool   ascending
        signal tapped()

        height: 20; radius: 10
        width: pillLabel.implicitWidth + (isActive ? pillArrow.implicitWidth + 28 : 20)
        color: isActive ? "#1a3a52" : "#181818"
        border.color: isActive ? libraryRoot.accentBlue : "#262626"; border.width: 1
        Behavior on color        { ColorAnimation { duration: 110 } }
        Behavior on border.color { ColorAnimation { duration: 110 } }
        Behavior on width        { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }

        Row {
            anchors.centerIn: parent; spacing: 4
            Text {
                id: pillLabel
                text: pill.label
                color: pill.isActive ? libraryRoot.textPrimary : libraryRoot.textSecond
                font.pixelSize: window.sp(10)
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: pillArrow
                visible: pill.isActive
                text: pill.ascending ? "▲" : "▼"
                color: libraryRoot.accentBlueLt; font.pixelSize: window.sp(8)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: pill.tapped() }
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

            // Focus indicator — top accent line when sidebar is focused
            Rectangle {
                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                height: 2; color: libraryRoot.accentGreen
                visible: libraryRoot.focusedPanel === "sidebar"
                z: 10
            }

            Flickable {
                id: sidebarFlickable
                anchors.fill: parent
                contentHeight: sidebarColumn.implicitHeight
                clip: true
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                Column {
                    id: sidebarColumn
                    width: parent.width

                    Item { width: parent.width; height: 8 }

                    // ── SAMMLUNG ─────────────────────────────────────────────
                    Rectangle {
                        width: parent.width; height: 30
                        color: "transparent"
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            height: 1; color: libraryRoot.borderSub
                        }
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 14
                            anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                            text: "SAMMLUNG"
                            color: "#484848"
                            font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.3
                        }
                    }

                    NavButton {
                        id: navAllTracks
                        tabKey: "library"
                        btnIcon: "♫"
                        btnLabel: "All Tracks"
                        badgeCount: libraryModel ? libraryModel.count : 0
                        customAction: function() { libraryRoot.librarySubTab = "allSongs" }
                        cursorIndex: 0
                    }

                    Item { width: parent.width; height: 8 }

                    // ── PLAYLISTS ─────────────────────────────────────────────
                    Rectangle {
                        width: parent.width; height: 32
                        color: "transparent"

                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            height: 1; color: libraryRoot.borderSub
                        }
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 14
                            anchors.bottom: parent.bottom; anchors.bottomMargin: 5
                            text: "PLAYLISTS"
                            color: "#484848"
                            font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.3
                        }

                        Rectangle {
                            anchors.right: parent.right; anchors.rightMargin: 10
                            anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                            width: 22; height: 22; radius: 4
                            color: addPlaylistHover.containsMouse ? "#2a2a2a" : "#1c1c1c"
                            border.color: addPlaylistHover.containsMouse ? "#3a3a3a" : "#272727"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent; text: "+"
                                color: addPlaylistHover.containsMouse ? libraryRoot.textPrimary : libraryRoot.textSecond
                                font.pixelSize: window.sp(14)
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
                        id: sidebarPlaylistRepeater
                        model: libraryRoot.visiblePlaylists

                        Rectangle {
                            id: plRow
                            required property var modelData
                            required property int index

                            property string dropZone: ""

                            readonly property int sidebarIndex: libraryRoot.sidebarPlaylistStartIndex + index

                            readonly property bool isSelected:
                                libraryRoot.activeTab === "playlist" &&
                                libraryRoot.currentPlaylistId === modelData.id

                            readonly property bool isSidebarCursor:
                                libraryRoot.focusedPanel === "sidebar" &&
                                libraryRoot.sidebarCursorActive &&
                                libraryRoot.sidebarCursorIndex === sidebarIndex

                            width: parent.width; height: 36
                            color: isSelected          ? libraryRoot.sidebarSel
                                 : dropZone === "into" ? "#0d2a18"
                                 : isSidebarCursor     ? "#0f2816"
                                 : plMouse.containsMouse ? libraryRoot.bgSidebarHv
                                 : "transparent"

                            Behavior on color { ColorAnimation { duration: 100 } }
                            opacity: plMouse.drag.active ? 0.45 : 1.0

                            // Selection accent (blue)
                            Rectangle {
                                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                width: 3; color: libraryRoot.accentBlue; visible: isSelected
                            }
                            // Keyboard cursor accent (green)
                            Rectangle {
                                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                width: 3; color: libraryRoot.accentGreen
                                visible: plRow.isSidebarCursor && !isSelected
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
                                    color: isSelected ? libraryRoot.textPrimary : libraryRoot.textNav
                                    font.pixelSize: window.sp(12); elide: Text.ElideRight
                                    width: libraryRoot.sidebarW - 88 - modelData.depth * 14
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            Rectangle {
                                visible: modelData.trackCount > 0
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.right: parent.right; anchors.rightMargin: 10
                                width: Math.max(22, plBadge.implicitWidth + 10); height: 16; radius: 8
                                color: "#1a1a1a"
                                border.color: "#252525"; border.width: 1
                                Text {
                                    id: plBadge; anchors.centerIn: parent
                                    text: modelData.trackCount
                                    color: "#666666"; font.pixelSize: window.sp(8)
                                }
                            }

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

                                onPressed: (mouse) => {
                                    if (libraryRoot._isContextClick(mouse)) {
                                        playlistContextMenu.targetId       = modelData.id
                                        playlistContextMenu.targetName     = modelData.name
                                        playlistContextMenu.targetParentId = modelData.parentId || ""
                                        libraryRoot._popupMenuAt(playlistContextMenu)
                                        return
                                    }
                                }
                                onClicked: (mouse) => {
                                    var arrowX = 14 + modelData.depth * 14
                                    if (modelData.hasChildren && mouse.x >= arrowX && mouse.x < arrowX + 12) {
                                        libraryRoot.toggleExpanded(modelData.id)
                                        // Sync sidebar cursor to this item
                                        libraryRoot.sidebarCursorIndex = plRow.sidebarIndex
                                        libraryRoot.sidebarCursorActive = true
                                        libraryRoot.focusedPanel = "sidebar"
                                        libraryRoot.forceActiveFocus()
                                        return
                                    }
                                    if (modelData.hasChildren && !libraryRoot.expandedPlaylists[modelData.id])
                                        libraryRoot.toggleExpanded(modelData.id)
                                    libraryRoot.currentPlaylistId   = modelData.id
                                    libraryRoot.currentPlaylistName = modelData.name
                                    libraryRoot.activeTab           = "playlist"
                                    libraryRoot.loadPlaylistTracks()
                                    // Clicking a playlist: keep sidebar focus for keyboard navigation
                                    libraryRoot.sidebarCursorIndex = plRow.sidebarIndex
                                    libraryRoot.sidebarCursorActive = true
                                    libraryRoot.focusedPanel = "sidebar"
                                    libraryRoot.forceActiveFocus()
                                }
                            }

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

                    Item { width: parent.width; height: 8 }

                    // ── QUELLEN ──────────────────────────────────────────────
                    Item {
                        width: parent.width; height: 30
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            height: 1; color: libraryRoot.borderSub
                        }
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 14
                            anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                            text: "QUELLEN"
                            color: "#484848"
                            font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.3
                        }
                    }

                    NavButton {
                        id: navFiles
                        tabKey: "files"
                        btnIcon: "≡"
                        btnLabel: "Files"
                        cursorIndex: libraryRoot.sidebarTopNavCount + libraryRoot.visiblePlaylists.length
                    }

                    NavButton {
                        id: navStreaming
                        tabKey: "streaming"
                        btnIcon: "◎"
                        btnLabel: "Streaming"
                        cursorIndex: libraryRoot.sidebarTopNavCount + libraryRoot.visiblePlaylists.length + 1
                    }

                    NavButton {
                        id: navUsb
                        tabKey: "usb"
                        btnIcon: "⊕"
                        btnLabel: "USB"
                        cursorIndex: libraryRoot.sidebarTopNavCount + libraryRoot.visiblePlaylists.length + 2
                    }

                    Item { width: parent.width; height: 12 }
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
                    anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 12
                    spacing: 8

                    // ── Tab icon + title ───────────────────────────────────
                    Row {
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 7

                        Rectangle {
                            width: 22; height: 22; radius: 4
                            color: "#132840"
                            border.color: "#1e4070"; border.width: 1
                            anchors.verticalCenter: parent.verticalCenter

                            Text {
                                anchors.centerIn: parent
                                text: libraryRoot.activeTab === "library"   ? "♫"
                                    : libraryRoot.activeTab === "playlist"  ? "☰"
                                    : libraryRoot.activeTab === "files"     ? "≡"
                                    : libraryRoot.activeTab === "streaming" ? "◎"
                                    : "⊕"
                                color: libraryRoot.accentBlueLt
                                font.pixelSize: window.sp(10)
                            }
                        }

                        Text {
                            text: libraryRoot.activeTab === "library"   ? "Library"
                                : libraryRoot.activeTab === "playlist"  ? libraryRoot.currentPlaylistName
                                : libraryRoot.activeTab === "files"     ? "File Browser"
                                : libraryRoot.activeTab === "streaming" ? "Streaming"
                                : "USB"
                            color: libraryRoot.textPrimary
                            font.pixelSize: window.sp(12); font.weight: Font.Medium
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    // ── Divider ────────────────────────────────────────────
                    Rectangle {
                        width: 1; height: 16; color: libraryRoot.borderMain
                        Layout.alignment: Qt.AlignVCenter
                        visible: libraryRoot.activeTab === "library" || libraryRoot.activeTab === "playlist"
                    }

                    // ── Analyse button (combined with progress fill) ────────
                    Rectangle {
                        id: analyzeBtn
                        Layout.preferredWidth: 148
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4; clip: true
                        color: analyzeHover.containsMouse ? "#1a1a1a" : "#111111"
                        border.color: libraryAnalyzer && libraryAnalyzer.running
                                      ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        visible: libraryRoot.activeTab === "library" || libraryRoot.activeTab === "playlist"

                        Behavior on border.color { ColorAnimation { duration: 200 } }

                        // Progress fill
                        Rectangle {
                            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: parent.width * (libraryAnalyzer ? libraryAnalyzer.progress : 0)
                            color: "#0d2840"
                            visible: libraryAnalyzer && libraryAnalyzer.running
                            Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
                        }

                        Row {
                            anchors.centerIn: parent
                            spacing: 6

                            Text {
                                text: libraryAnalyzer && libraryAnalyzer.running ? "◼" : "◎"
                                color: libraryAnalyzer && libraryAnalyzer.running
                                       ? libraryRoot.accentRed : libraryRoot.accentBlueLt
                                font.pixelSize: window.sp(9)
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: libraryAnalyzer && libraryAnalyzer.running ? "Stop" : "Analyse"
                                color: libraryRoot.textPrimary
                                font.pixelSize: window.sp(10); font.weight: Font.Medium
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                visible: libraryAnalyzer !== null && libraryAnalyzer !== undefined
                                         && libraryAnalyzer.running && libraryAnalyzer.total > 0
                                text: (libraryAnalyzer ? libraryAnalyzer.completed : 0)
                                      + "/" + (libraryAnalyzer ? libraryAnalyzer.total : 0)
                                color: libraryRoot.textSecond
                                font.pixelSize: window.sp(9); font.family: "monospace"
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        MouseArea {
                            id: analyzeHover
                            anchors.fill: parent
                            hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (!libraryAnalyzer) return
                                if (libraryAnalyzer.running) libraryAnalyzer.cancel()
                                else libraryRoot.startAnalyzeCurrentView()
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    // ── View mode toggle ───────────────────────────────────
                    Rectangle {
                        Layout.preferredWidth: 54
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4
                        color: "#111111"
                        border.color: "#2a2a2a"; border.width: 1
                        clip: true

                        Row {
                            anchors.fill: parent

                            Rectangle {
                                width: parent.width / 2; height: parent.height
                                color: libraryRoot.viewMode === "compact" ? libraryRoot.accentBlue : "transparent"
                                Behavior on color { ColorAnimation { duration: 120 } }

                                Text {
                                    anchors.centerIn: parent; text: "≡"
                                    color: libraryRoot.viewMode === "compact" ? "#ffffff" : libraryRoot.textSecond
                                    font.pixelSize: window.sp(12)
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: libraryRoot.viewMode = "compact"
                                }
                            }

                            Rectangle {
                                width: parent.width / 2; height: parent.height
                                color: libraryRoot.viewMode === "normal" ? libraryRoot.accentBlue : "transparent"
                                Behavior on color { ColorAnimation { duration: 120 } }

                                Text {
                                    anchors.centerIn: parent; text: "▤"
                                    color: libraryRoot.viewMode === "normal" ? "#ffffff" : libraryRoot.textSecond
                                    font.pixelSize: window.sp(11)
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: libraryRoot.viewMode = "normal"
                                }
                            }
                        }
                    }

                    // ── Search field ───────────────────────────────────────
                    Rectangle {
                        Layout.preferredWidth: Math.min(260, Math.max(150, parent.width * 0.28))
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter
                        color: "#0e0e0e"; radius: 4
                        border.color: searchField.activeFocus ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        Behavior on border.color { ColorAnimation { duration: 150 } }

                        // Search icon
                        Item {
                            id: searchIconItem
                            anchors.left: parent.left; anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 13; height: 13
                            opacity: searchField.activeFocus ? 0.75 : 0.30

                            Rectangle {
                                width: 9; height: 9; radius: 4.5
                                color: "transparent"
                                border.color: libraryRoot.textPrimary; border.width: 1.5
                                anchors.top: parent.top; anchors.left: parent.left
                            }
                            Rectangle {
                                width: 1.5; height: 5
                                color: libraryRoot.textPrimary
                                rotation: -45; transformOrigin: Item.Top
                                anchors.bottom: parent.bottom; anchors.right: parent.right
                            }
                        }

                        // Placeholder text
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 27
                            text: "Suchen…"
                            color: "#3a3a3a"
                            font.pixelSize: window.sp(11)
                            visible: !searchField.activeFocus && searchField.text.length === 0
                        }

                        // Clear button
                        Rectangle {
                            anchors.right: parent.right; anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16; height: 16; radius: 8
                            color: clearHover.containsMouse ? "#303030" : "#1e1e1e"
                            visible: searchField.text.length > 0
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                anchors.centerIn: parent; text: "×"
                                color: libraryRoot.textMeta; font.pixelSize: window.sp(11)
                            }
                            MouseArea {
                                id: clearHover; anchors.fill: parent
                                hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { searchField.text = ""; libraryRoot.searchText = "" }
                            }
                        }

                        TextField {
                            id: searchField
                            anchors.fill: parent
                            selectByMouse: true; color: libraryRoot.textPrimary
                            placeholderText: ""; font.pixelSize: window.sp(11)
                            leftPadding: 27
                            rightPadding: searchField.text.length > 0 ? 26 : 8
                            topPadding: 0; bottomPadding: 0
                            onTextEdited: libraryRoot.searchText = text
                            Component.onCompleted: text = libraryRoot.searchText
                            background: Item {}

                            onActiveFocusChanged: {
                                if (activeFocus)
                                    libraryRoot.focusedPanel = "search"
                                else if (libraryRoot.focusedPanel === "search")
                                    libraryRoot.focusedPanel = "tracks"
                            }
                            Keys.onEscapePressed: {
                                focus = false
                                libraryRoot.forceActiveFocus()
                            }
                            Keys.onTabPressed: (event) => {
                                focus = false
                                libraryRoot.cyclePanel(1)
                                event.accepted = true
                            }
                            Keys.onBacktabPressed: (event) => {
                                focus = false
                                libraryRoot.cyclePanel(-1)
                                event.accepted = true
                            }
                        }
                    }
                }
            }

            // ── View area ──────────────────────────────────────────────────
            Item {
                Layout.fillWidth: true
                // Focus indicator — top accent line when track list is focused
                Rectangle {
                    anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                    height: 2; color: libraryRoot.accentGreen
                    visible: libraryRoot.focusedPanel === "tracks"
                    z: 200
                }
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
                        height: libraryRoot.viewMode === "normal" ? 0 : libraryRoot.hdrH
                        visible: libraryRoot.viewMode === "compact"
                        color: libraryRoot.bgHeader

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

                    // ── Normal-view sort bar ───────────────────────────────
                    Rectangle {
                        id: libSortBar
                        anchors.top: libHeader.bottom; anchors.left: parent.left; anchors.right: parent.right
                        height: libraryRoot.viewMode === "normal" ? 30 : 0
                        visible: libraryRoot.viewMode === "normal"
                        color: libraryRoot.bgHeader
                        clip: true

                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 1; color: libraryRoot.borderMain
                        }

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 10
                            spacing: 5

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "SORT"
                                color: libraryRoot.textDim
                                font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.0
                            }
                            Rectangle {
                                width: 1; height: 12; color: libraryRoot.borderMain
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            SortPill {
                                label: "Titel"; ascending: libraryModel ? libraryModel.sortAscending : true
                                isActive: libraryModel ? libraryModel.sortField === "title" : false
                                onTapped: if (libraryModel) libraryModel.toggleSort("title")
                            }
                            SortPill {
                                label: "Künstler"; ascending: libraryModel ? libraryModel.sortAscending : true
                                isActive: libraryModel ? libraryModel.sortField === "artist" : false
                                onTapped: if (libraryModel) libraryModel.toggleSort("artist")
                            }
                            SortPill {
                                label: "Dauer"; ascending: libraryModel ? libraryModel.sortAscending : true
                                isActive: libraryModel ? libraryModel.sortField === "time" : false
                                onTapped: if (libraryModel) libraryModel.toggleSort("time")
                            }
                            SortPill {
                                label: "BPM"; ascending: libraryModel ? libraryModel.sortAscending : true
                                isActive: libraryModel ? libraryModel.sortField === "bpm" : false
                                onTapped: if (libraryModel) libraryModel.toggleSort("bpm")
                            }
                            SortPill {
                                label: "Key"; ascending: libraryModel ? libraryModel.sortAscending : true
                                isActive: libraryModel ? libraryModel.sortField === "key" : false
                                onTapped: if (libraryModel) libraryModel.toggleSort("key")
                            }
                        }
                    }

                    // ── Track list ─────────────────────────────────────────
                    ListView {
                        id: libTrackList
                        anchors.top: libSortBar.bottom; anchors.left: parent.left
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
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "♫"; color: "#252525"; font.pixelSize: window.sp(42) }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Library is empty"; color: "#333333"; font.pixelSize: window.sp(12) }
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
                        height: libraryRoot.viewMode === "normal" ? 0 : libraryRoot.hdrH
                        visible: libraryRoot.viewMode === "compact"
                        color: libraryRoot.bgHeader

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

                            Item {
                                width: libraryRoot.colTitle(playlistView.width); height: parent.height
                                PlSortHeader { width: parent.width; height: parent.height; field: "title"; label: "TITEL" }
                                Rectangle {
                                    anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
                                    width: 6; color: "transparent"
                                    Rectangle {
                                        anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
                                        width: 1
                                        color: plResizeMa.containsMouse || plResizeMa.pressed ? "#44ffffff" : "#1a1a1a"
                                    }
                                    MouseArea {
                                        id: plResizeMa; anchors.fill: parent
                                        hoverEnabled: true; cursorShape: Qt.SizeHorCursor
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
                            }

                            PlSortHeader { width: libraryRoot.colArtist(playlistView.width); height: parent.height; field: "artist";      label: "KÜNSTLER" }
                            PlSortHeader { width: libraryRoot.colTime;  height: parent.height; field: "durationSec"; label: "ZEIT";       centerAlign: true }
                            PlSortHeader { width: libraryRoot.colBpm;   height: parent.height; field: "bpm";         label: "BPM";        centerAlign: true }
                            PlSortHeader { width: libraryRoot.colKey;   height: parent.height; field: "key";         label: "KEY";        centerAlign: true }
                            PlSortHeader { width: libraryRoot.colKbps;  height: parent.height; field: "bitrateKbps"; label: "KBPS";       centerAlign: true; isLast: true }
                        }
                    }

                    // ── Normal-view sort bar ───────────────────────────────
                    Rectangle {
                        id: plSortBar
                        anchors.top: plHeader.bottom; anchors.left: parent.left; anchors.right: parent.right
                        height: libraryRoot.viewMode === "normal" ? 30 : 0
                        visible: libraryRoot.viewMode === "normal"
                        color: libraryRoot.bgHeader
                        clip: true

                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 1; color: libraryRoot.borderMain
                        }

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 10
                            spacing: 5

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "SORT"
                                color: libraryRoot.textDim
                                font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.0
                            }
                            Rectangle {
                                width: 1; height: 12; color: libraryRoot.borderMain
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            SortPill {
                                label: "Titel"; ascending: libraryRoot.playlistSortAscending
                                isActive: libraryRoot.playlistSortField === "title"
                                onTapped: libraryRoot.togglePlaylistSort("title")
                            }
                            SortPill {
                                label: "Künstler"; ascending: libraryRoot.playlistSortAscending
                                isActive: libraryRoot.playlistSortField === "artist"
                                onTapped: libraryRoot.togglePlaylistSort("artist")
                            }
                            SortPill {
                                label: "Dauer"; ascending: libraryRoot.playlistSortAscending
                                isActive: libraryRoot.playlistSortField === "durationSec"
                                onTapped: libraryRoot.togglePlaylistSort("durationSec")
                            }
                            SortPill {
                                label: "BPM"; ascending: libraryRoot.playlistSortAscending
                                isActive: libraryRoot.playlistSortField === "bpm"
                                onTapped: libraryRoot.togglePlaylistSort("bpm")
                            }
                            SortPill {
                                label: "Key"; ascending: libraryRoot.playlistSortAscending
                                isActive: libraryRoot.playlistSortField === "key"
                                onTapped: libraryRoot.togglePlaylistSort("key")
                            }
                        }
                    }

                    ListView {
                        id: plTrackList
                        anchors.top: plSortBar.bottom; anchors.left: parent.left
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
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "☰"; color: "#252525"; font.pixelSize: window.sp(42) }
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
                                    text: "No audio files in the selected folder"; color: "#2e2e2e"; font.pixelSize: window.sp(12)
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
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: libraryRoot.activeTab === "streaming" ? "◉" : "⎘"; color: "#252525"; font.pixelSize: window.sp(44) }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: (libraryRoot.activeTab.charAt(0).toUpperCase() + libraryRoot.activeTab.slice(1)) + " — Coming soon"
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
        background: Rectangle { implicitWidth: 260; color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }

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
        MenuItem {
            text: "Track analysieren / erneut analysieren"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: {
                if (libraryAnalyzer && libraryRoot.ctxTrackId && libraryRoot.ctxFilePath)
                    libraryAnalyzer.analyzeTrack(libraryRoot.ctxTrackId,
                                                 libraryRoot.ctxFilePath,
                                                 libraryRoot.ctxTitle)
            }
        }
        MenuSeparator {
            contentItem: Rectangle { height: 1; color: "#2a2a2a" }
        }
        Menu {
            id: addToPlaylistMenu
            title: "Zu Playlist hinzufügen"
            background: Rectangle { implicitWidth: 220; color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }

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
            text: "Remove from Library"
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

        background: Rectangle { implicitWidth: 200; color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }

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
                    Text { anchors.centerIn: parent; text: "Cancel"; color: "#888"; font.pixelSize: window.sp(10) }
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
                    Text { anchors.centerIn: parent; text: "Cancel"; color: "#888"; font.pixelSize: window.sp(10) }
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

            Text { text: "Remove from Library?"; color: libraryRoot.textPrimary; font.pixelSize: window.sp(12); font.bold: true }
            Text {
                width: parent.width
                text: '"' + removeFromLibraryPopup.trackTitle + '" will be removed from the library. The file will not be deleted.'
                color: libraryRoot.textSecond; font.pixelSize: window.sp(10); wrapMode: Text.Wrap
            }

            Row {
                anchors.right: parent.right; spacing: 6

                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: cancelRmLib.containsMouse ? "#2e2e2e" : "#242424"
                    border.color: "#333"; border.width: 1
                    Text { anchors.centerIn: parent; text: "Cancel"; color: "#888"; font.pixelSize: window.sp(10) }
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
                    Text { anchors.centerIn: parent; text: "Cancel"; color: "#888"; font.pixelSize: window.sp(10) }
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
