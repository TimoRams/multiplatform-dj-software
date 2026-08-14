import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

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
    property bool usbPlaylistTreeExpanded: false

    // Playlist data (loaded from C++ on demand)
    property var allPlaylists:   []   // [{id, name, parentId, sortOrder, trackCount}, ...]
    property var playlistTracks: []   // [{trackId, title, artist, ...}, ...] for currentPlaylistId
    property var expandedPlaylists: ({})  // id → true if expanded

    // ── New library data ───────────────────────────────────────────────────
    property var favoriteTracks:          []
    property var historyTracks:           []
    property var prepareCrateTracks:      []
    property var queueTracks:             []
    property var allTags:                 []
    property var smartCollections:        []
    property var smartCollectionTracks:   []
    property string currentSmartCollectionId:   ""
    property string currentSmartCollectionName: ""
    property string historyPeriod:        "all"  // "today" | "week" | "month" | "all"

    // Context menu state (shared between library + playlist views)
    property string ctxTrackId:   ""
    property string ctxFilePath:  ""
    property string ctxTitle:     ""

    // Track notes panel state
    property bool   notesPanelOpen:    false
    property string notesPanelTrackId: ""
    property string notesPanelTitle:   ""

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
    property string activeTrackSource:   ""   // "library" | "playlist" | "usb"

    // ── Sidebar keyboard navigation map ─────────────────────────────────
    readonly property int sidebarTopNavCount: 5   // All Tracks + Favorites + History + Crate + Queue
    readonly property int sidebarBottomNavCount: 3 // Files / Streaming / USB
    readonly property int sidebarSmartCollCount: smartCollections.length
    readonly property int sidebarPlaylistStartIndex: sidebarTopNavCount + sidebarSmartCollCount

    onActiveTabChanged: {
        syncSidebarCursorToSelection()
        ensureActiveTrackForCurrentTab()
        if      (activeTab === "favorites")  loadFavorites()
        else if (activeTab === "history")    loadHistory()
        else if (activeTab === "crate")      loadCrate()
        else if (activeTab === "queue")      loadQueue()
        else if (activeTab === "smartcoll")  loadSmartCollectionTracks(currentSmartCollectionId)
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
    readonly property color accentKeyMatch: "#3acc3a"
    readonly property color accentKeyCompat: "#7ad4ff"
    readonly property color sidebarSel:  "#0d2e52"
    readonly property color textMeta:    "#888888"
    readonly property color textNav:     "#aaaaaa"

    readonly property int rowH:       24
    readonly property int rowHNormal: 56
    readonly property int hdrH:       26
    readonly property int toolbarH:   touchMode ? 52 : 36
    readonly property int sidebarW:   touchMode ? 0 : 200
    property string viewMode: "compact"
    readonly property bool touchMode: window.allInOneMode
    property string aioBrowseScreen: "home" // home | source | artist | album | key | playlist | history | matching | folder
    readonly property int aioNavPad: 4
    readonly property int aioNavGap: 2
    readonly property int aioNavTileW: 104
    readonly property int aioNavTileH: 48
    readonly property int aioNavPanelW: touchMode ? aioNavTileW + aioNavPad * 2 : 0
    property string aioBrowseSelection: ""
    property string aioBrowseFocusKey: ""
    property int aioBrowseFocusIndex: -1
    property bool aioBrowseDrilled: false
    // AIO cursor zones: nav (category tiles) → picker → tracks (→ drilled)
    property string aioFocusZone: "tracks"
    property int aioNavCursorIndex: 3
    readonly property bool aioCursorTracksActive: !touchMode || aioFocusZone === "tracks"
    readonly property bool aioCursorPickerActive: touchMode && aioFocusZone === "picker"
    readonly property bool aioCursorNavActive: touchMode && aioFocusZone === "nav"
    property var aioArtistList: []
    property var aioAlbumList: []
    property var aioKeyList: []
    property var aioSourceList: []
    readonly property bool aioLibSplitBrowse: touchMode && (
           aioBrowseScreen === "artist" || aioBrowseScreen === "album"
        || aioBrowseScreen === "key" || aioBrowseScreen === "source"
        || aioBrowseScreen === "matching")
    readonly property bool aioPlaylistSplitBrowse: touchMode && aioBrowseScreen === "playlist"
    readonly property bool aioHistorySplitBrowse: touchMode && aioBrowseScreen === "history"
    readonly property bool aioFolderSplitBrowse: touchMode && aioBrowseScreen === "folder"
    readonly property bool previewActive: typeof libraryPreview !== "undefined"
                                            && libraryPreview && libraryPreview.playing
    readonly property int previewBarHeight: previewActive ? (touchMode ? 58 : 50) : 0
    readonly property int previewBarReserve: previewBarHeight
    property int openSwipeRowIndex: -1
    property bool filterPanelOpen: false
    readonly property bool filtersActive: libraryModel && (
        libraryModel.filterBpmMin > 0 || libraryModel.filterBpmMax > 0
        || (libraryModel.filterKey && libraryModel.filterKey.length > 0)
        || (libraryModel.filterArtist && libraryModel.filterArtist.length > 0)
        || (libraryModel.filterAlbum && libraryModel.filterAlbum.length > 0)
        || (libraryModel.filterSourcePath && libraryModel.filterSourcePath.length > 0)
        || (libraryModel.filterGenre && libraryModel.filterGenre.length > 0)
        || libraryModel.filterRatingMin > 0 || libraryModel.filterEnergyMin > 0
    )

    function trackRowHeight() {
        if (touchMode) {
            if (aioBrowseDrilled)
                return 96
            if ((aioLibSplitBrowse || aioPlaylistSplitBrowse) && aioBrowseFocusKey.length > 0)
                return 56
            return 80
        }
        return viewMode === "normal" ? rowHNormal : rowH
    }

    function loadTrackToDeck(deckLetter, filePath, trackId) {
        if (!filePath) return
        if (typeof libraryPreview !== "undefined" && libraryPreview)
            libraryPreview.stop()
        var deck = deckLetter === "A" ? deckA
                 : deckLetter === "B" ? deckB
                 : deckLetter === "C" ? deckC
                 : deckLetter === "D" ? deckD : null
        if (!deck) return
        closeAllSwipes()
        if (touchMode && window)
            window.activeMainTab = "performance"
        if (trackId && trackId.indexOf("rekordbox:") === 0
                && typeof deviceLibraryManager !== "undefined" && deviceLibraryManager) {
            deviceLibraryManager.requestDeckLoad(trackId, deckLetter)
            return
        }
        Qt.callLater(function() { deck.loadTrack(filePath) })
    }

    function closeAllSwipes() {
        openSwipeRowIndex = -1
    }

    function togglePreview(filePath) {
        if (!filePath || typeof libraryPreview === "undefined" || !libraryPreview)
            return
        libraryPreview.togglePreview(filePath)
    }

    function formatPreviewTime(sec) {
        if (!isFinite(sec) || sec < 0)
            return "0:00"
        var m = Math.floor(sec / 60)
        var s = Math.floor(sec % 60)
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    function aioNavScreens() {
        return ["source", "artist", "album", "home", "key", "playlist", "history", "matching", "folder"]
    }

    function aioHasSplitBrowse() {
        return aioLibSplitBrowse || aioPlaylistSplitBrowse
            || aioHistorySplitBrowse || aioFolderSplitBrowse
    }

    function aioHasPicker() {
        return aioHasSplitBrowse() && !aioBrowseDrilled
    }

    function aioSyncNavCursorToScreen() {
        var screens = aioNavScreens()
        var screen = aioBrowseScreen
        if (screen === "home" || (screen === "home" && activeTab === "library"))
            screen = "home"
        var idx = screens.indexOf(screen)
        if (idx >= 0)
            aioNavCursorIndex = idx
    }

    function aioNavScreenAt(index) {
        var screens = aioNavScreens()
        if (index < 0 || index >= screens.length)
            return ""
        return screens[index]
    }

    function aioMoveNavCursor(delta) {
        var count = aioNavScreens().length
        aioNavCursorIndex = Math.max(0, Math.min(count - 1, aioNavCursorIndex + delta))
    }

    function aioMovePickerCursor(delta) {
        var items = aioBrowsePickerItems()
        if (items.length === 0)
            return
        var idx = aioBrowseFocusIndex < 0 ? 0 : aioBrowseFocusIndex + delta
        idx = Math.max(0, Math.min(items.length - 1, idx))
        aioPreviewBrowseEntryByIndex(idx)
    }

    function aioActivateBrowsePickerEntry(index) {
        if (aioBrowseScreen === "source") {
            aioDrillBrowseEntryByIndex(index)
            return
        }
        aioPreviewBrowseEntryByIndex(index)
        aioFocusZone = "picker"
    }

    function aioBrowseCursorRight() {
        forceActiveFocus()
        if (aioFocusZone === "nav") {
            var screen = aioNavScreenAt(aioNavCursorIndex)
            if (screen === "home")
                aioOpenAllTracks()
            else
                aioEnterBrowse(screen)
            aioFocusZone = aioHasPicker() ? "picker" : "tracks"
            if (aioFocusZone === "tracks")
                ensureActiveTrackForCurrentTab()
            return
        }
        if (aioFocusZone === "picker") {
            if (aioBrowseScreen === "source") {
                aioDrillBrowseEntryByIndex(aioBrowseFocusIndex)
                return
            }
            aioFocusZone = "tracks"
            ensureActiveTrackForCurrentTab()
            return
        }
        if (aioFocusZone === "tracks" && aioHasPicker() && !aioBrowseDrilled
                && aioBrowseFocusIndex >= 0) {
            aioDrillBrowseEntryByIndex(aioBrowseFocusIndex)
        }
    }

    function aioBrowseCursorLeft() {
        forceActiveFocus()
        if (aioBrowseDrilled) {
            aioBrowseUnDrill()
            aioFocusZone = "tracks"
            return
        }
        if (aioFocusZone === "tracks" && aioHasPicker()) {
            aioFocusZone = "picker"
            return
        }
        if (aioFocusZone === "picker" || aioFocusZone === "tracks") {
            aioFocusZone = "nav"
            aioSyncNavCursorToScreen()
        }
    }

    function aioBrowseCursorActivate() {
        if (aioFocusZone === "nav") {
            aioBrowseCursorRight()
            return
        }
        if (aioFocusZone === "picker") {
            if (aioBrowseScreen === "source") {
                aioDrillBrowseEntryByIndex(aioBrowseFocusIndex)
                return
            }
            aioFocusZone = "tracks"
            ensureActiveTrackForCurrentTab()
            return
        }
        if (aioFocusZone === "tracks")
            loadTrackToDeck("A", getCursorFilePath(), getCursorTrackId())
    }

    function aioMoveBrowseVertical(rawDelta) {
        if (!touchMode)
            return
        forceActiveFocus()
        var absD = Math.abs(rawDelta)
        var steps = absD <= 1 ? 1 : (absD <= 3 ? absD * 2 : absD * 3)
        steps = Math.min(steps, 24)
        var delta = rawDelta >= 0 ? steps : -steps

        if (aioFocusZone === "nav") {
            aioMoveNavCursor(delta)
            return
        }
        if (aioFocusZone === "picker") {
            aioMovePickerCursor(delta)
            return
        }
        moveCursor(rawDelta)
    }

    function aioResetToHome() {
        aioBrowseScreen = "home"
        aioBrowseSelection = ""
    }

    function aioWarmBrowseCaches() {
        if (!touchMode || !libraryDb)
            return
        aioArtistList = libraryDb.getDistinctArtists()
        aioAlbumList = libraryDb.getDistinctAlbums()
        aioKeyList = libraryDb.getDistinctKeys()
    }

    function aioRefreshBrowseData(screen) {
        if (!libraryDb)
            return
        if (screen === "artist")
            aioArtistList = libraryDb.getDistinctArtists()
        else if (screen === "album")
            aioAlbumList = libraryDb.getDistinctAlbums()
        else if (screen === "key" || screen === "matching")
            aioKeyList = libraryDb.getDistinctKeys()
        else if (screen === "source")
            aioSourceList = libraryDb.getLibrarySourceRoots()
    }

    function aioEnterBrowse(screen) {
        aioBrowseScreen = screen
        aioBrowseSelection = ""
        aioBrowseFocusKey = ""
        aioBrowseFocusIndex = -1
        aioBrowseDrilled = false
        aioRefreshBrowseData(screen)
        closeAllSwipes()
        aioSyncNavCursorToScreen()

        if (screen === "folder") {
            activeTab = "files"
            aioFocusZone = "picker"
            return
        }

        switch (screen) {
        case "artist":
        case "album":
        case "key":
        case "source":
        case "matching":
            activeTab = "library"
            break
        case "playlist":
            activeTab = "playlist"
            currentPlaylistId = ""
            currentPlaylistName = ""
            playlistTracks = []
            break
        case "history":
            activeTab = "history"
            break
        default:
            break
        }

        aioFocusZone = aioHasPicker() ? "picker" : "tracks"

        Qt.callLater(function() {
            var items = libraryRoot.aioBrowsePickerItems()
            if (items.length > 0
                    && (libraryRoot.aioLibSplitBrowse
                        || libraryRoot.aioPlaylistSplitBrowse
                        || libraryRoot.aioHistorySplitBrowse))
                libraryRoot.aioPreviewBrowseEntryByIndex(0)
            else if (libraryRoot.aioLibSplitBrowse)
                libraryRoot.aioHoldBrowseFilter()
            else if (libraryRoot.aioFocusZone === "tracks")
                libraryRoot.ensureActiveTrackForCurrentTab()
        })
    }

    function aioBrowseUnDrill() {
        aioBrowseDrilled = false
        aioBrowseSelection = ""
        if (aioBrowseScreen === "playlist") {
            currentPlaylistId = ""
            currentPlaylistName = ""
            playlistTracks = []
        }
        var items = aioBrowsePickerItems()
        if (items.length > 0) {
            var idx = aioBrowseFocusIndex >= 0 ? aioBrowseFocusIndex : 0
            idx = Math.min(idx, items.length - 1)
            aioPreviewBrowseEntryByIndex(idx)
        } else {
            aioHoldBrowseFilter()
        }
        aioFocusZone = aioHasPicker() ? "tracks" : "tracks"
        ensureActiveTrackForCurrentTab()
    }

    function aioBrowseEntryAt(index) {
        var items = aioBrowsePickerItems()
        if (index >= 0 && index < items.length)
            return items[index]
        return null
    }

    function aioBrowseEntryLabel(entry) {
        if (!entry)
            return ""
        var name = entry.name
        if (name === undefined || name === null || name === "")
            name = entry.label
        if (name === undefined || name === null || name === "")
            name = entry["name"] || entry["label"] || ""
        return String(name)
    }

    function aioPreviewBrowseEntryByIndex(index) {
        if (index < 0 || aioBrowseDrilled)
            return
        if (index === aioBrowseFocusIndex && aioBrowseFocusKey.length > 0)
            return
        var entry = aioBrowseEntryAt(index)
        if (!entry)
            return
        aioBrowseFocusIndex = index
        aioBrowseFocusKey = aioBrowseEntryKey(entry)
        aioApplyBrowseFilter(entry, false)
    }

    function aioDrillBrowseEntryByIndex(index) {
        var entry = aioBrowseEntryAt(index)
        if (!entry)
            return
        closeAllSwipes()
        aioBrowseDrilled = true
        aioBrowseFocusIndex = index
        aioBrowseSelection = aioBrowseEntryKey(entry)
        aioBrowseFocusKey = aioBrowseSelection
        aioApplyBrowseFilter(entry, true)
        if (touchMode && (aioBrowseScreen === "artist" || aioBrowseScreen === "album"
                          || aioBrowseScreen === "key"))
            viewMode = "normal"
        aioFocusZone = "tracks"
        ensureActiveTrackForCurrentTab()
    }

    function aioPreviewBrowseEntry(entry, index) {
        if (index !== undefined && index >= 0) {
            aioPreviewBrowseEntryByIndex(index)
            return
        }
        if (!entry || aioBrowseDrilled)
            return
        aioBrowseFocusKey = aioBrowseEntryKey(entry)
        aioApplyBrowseFilter(entry, false)
    }

    function aioDrillBrowseEntry(entry) {
        if (!entry)
            return
        var items = aioBrowsePickerItems()
        for (var i = 0; i < items.length; ++i) {
            if (aioBrowseEntryKey(items[i]) === aioBrowseEntryKey(entry)) {
                aioDrillBrowseEntryByIndex(i)
                return
            }
        }
        closeAllSwipes()
        aioBrowseDrilled = true
        aioBrowseSelection = aioBrowseEntryKey(entry)
        aioBrowseFocusKey = aioBrowseSelection
        aioApplyBrowseFilter(entry, true)
        if (touchMode && (aioBrowseScreen === "artist" || aioBrowseScreen === "album"
                          || aioBrowseScreen === "key"))
            viewMode = "normal"
        aioFocusZone = "tracks"
        ensureActiveTrackForCurrentTab()
    }

    function aioApplyBrowseFilter(entry, drilled) {
        var key = aioBrowseEntryKey(entry)
        var label = aioBrowseEntryLabel(entry)

        if (aioBrowseScreen === "artist") {
            activeTab = "library"
            if (libraryModel)
                libraryModel.applyAioBrowseFilter("artist", label)
            return
        }
        if (aioBrowseScreen === "album") {
            activeTab = "library"
            if (libraryModel)
                libraryModel.applyAioBrowseFilter("album", label)
            return
        }
        if (aioBrowseScreen === "key") {
            activeTab = "library"
            if (libraryModel)
                libraryModel.applyAioBrowseFilter("key", label)
            return
        }
        if (aioBrowseScreen === "source") {
            if (key === "collection") {
                if (drilled)
                    aioOpenAllTracks()
                return
            }
            if (key === "files")  { if (drilled) activeTab = "files"; return }
            if (key === "stream") { if (drilled) activeTab = "streaming"; return }
            if (key === "usb")    { if (drilled) activeTab = "usb"; return }
            activeTab = "library"
            if (libraryModel)
                libraryModel.applyAioBrowseFilter("source", entry.path || "")
            return
        }
        if (aioBrowseScreen === "playlist") {
            activeTab = "playlist"
            currentPlaylistId = entry.id || ""
            currentPlaylistName = entry.name || ""
            if (currentPlaylistId)
                loadPlaylistTracks()
            if (drilled)
                aioBrowseSelection = entry.id || ""
            return
        }
        if (aioBrowseScreen === "history") {
            activeTab = "history"
            loadHistory(key)
            if (drilled)
                aioBrowseSelection = key
            return
        }
        if (aioBrowseScreen === "matching") {
            if (key === "harmonic") {
                if (drilled) {
                    aioOpenHarmonicMatch()
                } else {
                    refreshReferenceKeys()
                    var hkeys = []
                    var hentries = aioCompatibleKeyEntries()
                    for (var hi = 0; hi < hentries.length; ++hi)
                        hkeys.push(aioBrowseEntryLabel(hentries[hi]))
                    activeTab = "library"
                    if (libraryModel)
                        libraryModel.applyAioBrowseFilter("keys", "", hkeys)
                }
                return
            }
            if (entry.smart) {
                if (drilled) {
                    aioOpenSmartCollection(entry.id, entry.name)
                    aioBrowseSelection = entry.id
                }
                return
            }
            activeTab = "library"
            if (libraryModel)
                libraryModel.applyAioBrowseFilter("key", label)
            return
        }
        if (aioBrowseScreen === "folder") {
            activeTab = "files"
            if (drilled) {
                if (key === "__up__") {
                    if (libraryManager)
                        libraryManager.navigateUp()
                } else if (libraryManager) {
                    libraryManager.enterFolder(entry.name)
                }
            }
        }
    }

    function aioBrowsePickerTitle() {
        switch (aioBrowseScreen) {
        case "artist":   return "ARTISTS"
        case "album":    return "ALBUMS"
        case "key":      return "KEYS"
        case "source":   return "SOURCE"
        case "playlist": return "PLAYLISTS"
        case "history":  return "HISTORY"
        case "matching": return "MATCHING"
        case "folder":   return "FOLDERS"
        default:         return ""
        }
    }

    function aioBrowsePickerItems() {
        switch (aioBrowseScreen) {
        case "artist":
            return aioArtistList
        case "album":
            return aioAlbumList
        case "key":
            return aioKeyList
        case "source": {
            var src = [
                { key: "collection", name: "COLLECTION",
                  trackCount: libraryModel ? libraryModel.count : 0 },
                { key: "files",  name: "FILES",  trackCount: -1 },
                { key: "stream", name: "STREAM", trackCount: -1 },
                { key: "usb",    name: "USB",    trackCount: -1 }
            ]
            for (var i = 0; i < aioSourceList.length; ++i) {
                var s = aioSourceList[i]
                src.push({ key: s.path, name: s.label || s.path,
                           path: s.path, trackCount: s.trackCount })
            }
            return src
        }
        case "playlist":
            return allPlaylists
        case "history":
            return [
                { key: "today", name: "TODAY",  trackCount: -1 },
                { key: "week",  name: "WEEK",   trackCount: -1 },
                { key: "month", name: "MONTH",  trackCount: -1 },
                { key: "all",   name: "ALL",    trackCount: -1 }
            ]
        case "matching": {
            var m = [{ key: "harmonic", name: "HARMONIC", trackCount: -1 }]
            var keys = aioCompatibleKeyEntries()
            for (var k = 0; k < keys.length; ++k)
                m.push({ key: keys[k].name, name: keys[k].name, trackCount: keys[k].trackCount })
            for (var j = 0; j < smartCollections.length; ++j) {
                var sc = smartCollections[j]
                m.push({ key: sc.id, name: sc.name || "Rule", id: sc.id, trackCount: -1, smart: true })
            }
            return m
        }
        case "folder": {
            var folders = libraryManager ? libraryManager.folders : []
            var out = []
            if (libraryManager && libraryManager.canNavigateUp)
                out.push({ key: "__up__", name: "UP", trackCount: -1 })
            for (var f = 0; f < folders.length; ++f)
                out.push({ key: folders[f], name: folders[f], trackCount: -1 })
            return out
        }
        default:
            return []
        }
    }

    function aioBrowseEntryKey(entry) {
        if (!entry)
            return ""
        if (entry.key !== undefined)
            return String(entry.key)
        if (entry.id !== undefined)
            return String(entry.id)
        return entry.name || ""
    }

    function aioReapplySearchFilter() {
        if (libraryModel && searchText.length > 0)
            libraryModel.setFilterText(searchText)
    }

    function aioHoldBrowseFilter() {
        if (!libraryModel)
            return
        var none = "\u0000"
        if (aioBrowseScreen === "artist")
            libraryModel.applyAioBrowseFilter("artist", none)
        else if (aioBrowseScreen === "album")
            libraryModel.applyAioBrowseFilter("album", none)
        else if (aioBrowseScreen === "key")
            libraryModel.applyAioBrowseFilter("key", none)
        else if (aioBrowseScreen === "source")
            libraryModel.applyAioBrowseFilter("source", none)
    }

    function aioOpenLibraryFiltered(applyFilter) {
        activeTab = "library"
        if (libraryModel) {
            libraryModel.clearFilters()
            if (applyFilter)
                applyFilter()
        }
        focusedPanel = "tracks"
        closeAllSwipes()
    }

    function aioOpenAllTracks() {
        aioBrowseScreen = "home"
        aioBrowseSelection = ""
        aioBrowseFocusKey = ""
        aioBrowseFocusIndex = -1
        aioBrowseDrilled = false
        aioOpenLibraryFiltered(null)
        librarySubTab = "allSongs"
        aioSyncNavCursorToScreen()
        aioFocusZone = "tracks"
        ensureActiveTrackForCurrentTab()
    }

    function aioOpenArtist(name) {
        aioBrowseSelection = name
        aioOpenLibraryFiltered(function() { libraryModel.setFilterArtist(name) })
    }

    function aioOpenAlbum(name) {
        aioBrowseSelection = name
        aioOpenLibraryFiltered(function() { libraryModel.setFilterAlbum(name) })
    }

    function aioOpenKeyFilter(key) {
        aioBrowseSelection = key
        aioOpenLibraryFiltered(function() { libraryModel.setFilterKey(key) })
    }

    function aioOpenSourcePath(path, label) {
        aioBrowseSelection = label || path
        aioOpenLibraryFiltered(function() { libraryModel.setFilterSourcePath(path) })
    }

    function aioCompatibleKeyEntries() {
        refreshReferenceKeys()
        var out = []
        var seen = {}
        for (var i = 0; i < referenceKeys.length; ++i) {
            var ref = parseCamelotKey(referenceKeys[i])
            if (!ref)
                continue
            for (var j = 0; j < aioKeyList.length; ++j) {
                var entry = aioKeyList[j]
                var kn = entry.name
                if (seen[kn])
                    continue
                var pk = parseCamelotKey(kn)
                if (pk && camelotCompatibility(ref, pk) > 0) {
                    seen[kn] = true
                    out.push(entry)
                }
            }
        }
        return out
    }

    function aioOpenHarmonicMatch() {
        refreshReferenceKeys()
        var keys = []
        var entries = aioCompatibleKeyEntries()
        for (var i = 0; i < entries.length; ++i)
            keys.push(entries[i].name)
        activeTab = "library"
        if (libraryModel) {
            libraryModel.clearFilters()
            libraryModel.setFilterKeys(keys)
        }
        aioBrowseSelection = "harmonic"
        aioBrowseDrilled = true
        focusedPanel = "tracks"
        closeAllSwipes()
    }

    function aioOpenTracks(tab, extraAction) {
        activeTab = tab
        if (extraAction)
            extraAction()
        focusedPanel = "tracks"
        closeAllSwipes()
    }

    function aioOpenPlaylist(id, name) {
        currentPlaylistId = id
        currentPlaylistName = name
        activeTab = "playlist"
        loadPlaylistTracks()
        focusedPanel = "tracks"
        closeAllSwipes()
    }

    function aioOpenSmartCollection(id, name) {
        currentSmartCollectionId = id
        currentSmartCollectionName = name
        loadSmartCollectionTracks(id)
        activeTab = "smartcoll"
        focusedPanel = "tracks"
        closeAllSwipes()
    }

    function aioToolbarBack() {
        aioBrowseScreen = "home"
        aioBrowseSelection = ""
    }

    function aioBrowseFocusLabel() {
        var items = aioBrowsePickerItems()
        for (var i = 0; i < items.length; ++i) {
            if (aioBrowseEntryKey(items[i]) === aioBrowseFocusKey)
                return aioBrowseEntryLabel(items[i])
        }
        return ""
    }

    function aioScreenTitle() {
        if (aioBrowseDrilled) {
            if (aioBrowseScreen === "playlist")
                return currentPlaylistName || "Playlist"
            if (aioBrowseScreen === "matching" && activeTab === "smartcoll")
                return currentSmartCollectionName || "Matching"
            var drilled = aioBrowseFocusLabel()
            if (drilled.length > 0)
                return drilled
            return aioBrowsePickerTitle()
        }
        if (aioBrowseFocusKey.length > 0 && aioBrowseScreen !== "home") {
            var preview = aioBrowseFocusLabel()
            if (preview.length > 0)
                return preview
        }
        if (aioBrowseScreen !== "home")
            return aioBrowsePickerTitle()
        if (activeTab === "library")
            return "All Tracks"
        if (activeTab === "playlist")
            return currentPlaylistName || "Playlist"
        if (activeTab === "history")
            return "History"
        if (activeTab === "smartcoll")
            return currentSmartCollectionName || "Matching"
        if (activeTab === "files")
            return "Folder"
        if (activeTab === "streaming")
            return "Streaming"
        return "USB"
    }

    Connections {
        target: typeof window !== "undefined" ? window : null
        function onAllInOneModeChanged() {
            if (window && window.allInOneMode) {
                libraryRoot.viewMode = "normal"
                libraryRoot.aioBrowseScreen = "home"
                libraryRoot.aioWarmBrowseCaches()
            }
        }
    }

    onAioFocusZoneChanged: {
        if (touchMode && aioFocusZone === "tracks")
            focusedPanel = "tracks"
    }

    onVisibleChanged: {
        if (visible && touchMode) {
            if (!_aioWasVisible)
                aioBrowseScreen = "home"
            aioWarmBrowseCaches()
        }
        _aioWasVisible = visible
    }
    property bool _aioWasVisible: false

    property var referenceKeys: []
    property int referenceKeysTick: 0

    function refreshReferenceKeys() {
        referenceKeysTick++
        var keys = []
        var decks = [deckA, deckB, deckC, deckD]
        for (var i = 0; i < decks.length; ++i) {
            var d = decks[i]
            if (d && d.hasTrack) {
                var k = (d.trackKey || "").trim()
                if (k.length > 0 && keys.indexOf(k) < 0)
                    keys.push(k)
            }
        }
        referenceKeys = keys
    }

    function parseCamelotKey(key) {
        if (!key || key.length === 0)
            return null
        var m = String(key).trim().match(/^(1[0-2]|[1-9])\s*([ABab])$/)
        if (!m)
            m = String(key).trim().match(/\b(1[0-2]|[1-9])\s*([ABab])\b/)
        if (!m)
            return null
        var n = parseInt(m[1], 10)
        if (n < 1 || n > 12)
            return null
        return { n: n, l: m[2].toUpperCase() }
    }

    function camelotWrap(n) {
        if (n < 1) return 12
        if (n > 12) return 1
        return n
    }

    // 0 = none, 1 = compatible (±1 / relative), 2 = exact match
    function camelotCompatibility(a, b) {
        if (!a || !b)
            return 0
        if (a.n === b.n && a.l === b.l)
            return 2
        if (a.n === b.n)
            return 1
        if (a.l === b.l && (a.n === camelotWrap(b.n + 1) || a.n === camelotWrap(b.n - 1)))
            return 1
        return 0
    }

    function keyMatchLevel(rowKey) {
        var _tick = referenceKeysTick
        if (!rowKey || referenceKeys.length === 0)
            return 0
        var parts = parseCamelotKey(rowKey)
        if (!parts)
            return 0
        var best = 0
        for (var i = 0; i < referenceKeys.length; ++i) {
            var ref = parseCamelotKey(referenceKeys[i])
            if (!ref)
                continue
            var lvl = camelotCompatibility(parts, ref)
            if (lvl > best)
                best = lvl
            if (best === 2)
                return 2
        }
        return best
    }

    function keyMatchColor(rowKey) {
        var lvl = keyMatchLevel(rowKey)
        if (lvl === 2) return accentKeyMatch
        if (lvl === 1) return accentKeyCompat
        return rowKey ? accentKey : textDim
    }

    function openSmartCollEditor(sc) {
        if (!sc) return
        createSmartCollDialog.pendingSc = sc
        createSmartCollDialog.editId = sc.id
        createSmartCollDialog.open()
    }

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

    function _pad2(value) {
        return value < 10 ? "0" + value : "" + value
    }

    function formatHistoryTime(epochSeconds) {
        if (!epochSeconds || epochSeconds <= 0) return "Unknown time"
        var d = new Date(epochSeconds * 1000)
        return _pad2(d.getHours()) + ":" + _pad2(d.getMinutes()) + ":" + _pad2(d.getSeconds())
    }

    function formatHistoryDate(epochSeconds) {
        if (!epochSeconds || epochSeconds <= 0) return "Unknown date"
        var d = new Date(epochSeconds * 1000)
        var today = new Date()
        var yesterday = new Date()
        yesterday.setDate(today.getDate() - 1)
        if (d.toDateString() === today.toDateString()) return "Today"
        if (d.toDateString() === yesterday.toDateString()) return "Yesterday"
        return _pad2(d.getDate()) + "." + _pad2(d.getMonth() + 1) + "." + d.getFullYear()
    }

    function formatHistoryStamp(epochSeconds) {
        if (!epochSeconds || epochSeconds <= 0) return "Played"
        return "Played " + formatHistoryDate(epochSeconds) + " at " + formatHistoryTime(epochSeconds)
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

    function loadFavorites()  { favoriteTracks      = libraryDb ? libraryDb.getFavoriteTracks()                     : [] }
    function loadHistory(p)   {
        var period = p !== undefined ? p : historyPeriod
        historyPeriod = period
        historyTracks = libraryDb ? libraryDb.getPlayHistory(period) : []
    }
    function loadCrate()      { prepareCrateTracks   = libraryDb ? libraryDb.getPrepareCrateTracks()     : [] }
    function loadQueue()      { queueTracks          = libraryDb ? libraryDb.getQueueTracks()            : [] }
    function loadAllTags()    { allTags              = libraryDb ? libraryDb.getAllTags()                 : [] }
    function loadSmartCollections() { smartCollections = libraryDb ? libraryDb.getAllSmartCollections()  : [] }
    function loadSmartCollectionTracks(id) {
        if (!id || !libraryDb) { smartCollectionTracks = []; return }
        var sc = smartCollections.find(function(s) { return s.id === id })
        smartCollectionTracks = sc ? libraryDb.evaluateSmartCollection(sc.rulesJson) : []
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

    // Unified track list for all non-library/non-playlist tabs.
    readonly property var currentListTracks: {
        var t = activeTab
        if (t === "favorites")  return favoriteTracks
        if (t === "history")    return historyTracks
        if (t === "crate")      return prepareCrateTracks
        if (t === "queue")      return queueTracks
        if (t === "smartcoll")  return smartCollectionTracks
        if (t === "playlist")   return sortedPlaylistTracks
        return []
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
            libraryAnalyzer.analyzePlaylist(currentPlaylistId, false)
        else
            libraryAnalyzer.analyzeAll(false)
    }

    // ── Cursor navigation ──────────────────────────────────────────────────

    function sidebarTotalCount() {
        return sidebarTopNavCount + sidebarSmartCollCount + visiblePlaylists.length + sidebarBottomNavCount
    }

    function sidebarIndexForTab(tab) {
        if (tab === "library")   return 0
        if (tab === "favorites") return 1
        if (tab === "history")   return 2
        if (tab === "crate")     return 3
        if (tab === "queue")     return 4
        if (tab === "smartcoll") {
            for (var i = 0; i < smartCollections.length; i++)
                if (smartCollections[i].id === currentSmartCollectionId)
                    return sidebarTopNavCount + i
            return sidebarTopNavCount
        }
        var bottomStart = sidebarTopNavCount + sidebarSmartCollCount + visiblePlaylists.length
        if (tab === "files")      return bottomStart
        if (tab === "streaming")  return bottomStart + 1
        if (tab === "usb")        return bottomStart + 2
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

        if (index === 0) return { type: "nav", tab: "library",   item: navAllTracks }
        if (index === 1) return { type: "nav", tab: "favorites", item: navFavorites }
        if (index === 2) return { type: "nav", tab: "history",   item: navHistory }
        if (index === 3) return { type: "nav", tab: "crate",     item: navCrate }
        if (index === 4) return { type: "nav", tab: "queue",     item: navQueue }

        var scIndex = index - sidebarTopNavCount
        if (scIndex >= 0 && scIndex < sidebarSmartCollCount) {
            return {
                type: "smartcoll",
                data: smartCollections[scIndex],
                item: smartCollRepeater.itemAt(scIndex)
            }
        }

        var bottomStart = sidebarTopNavCount + sidebarSmartCollCount + visiblePlaylists.length
        var plIndex = index - sidebarTopNavCount - sidebarSmartCollCount
        if (plIndex >= 0 && plIndex < visiblePlaylists.length) {
            return {
                type: "playlist",
                data: visiblePlaylists[plIndex],
                item: sidebarPlaylistRepeater.itemAt(plIndex)
            }
        }

        var bottomIndex = index - bottomStart
        if (bottomIndex === 0) return { type: "nav", tab: "files",     item: navFiles }
        if (bottomIndex === 1) return { type: "nav", tab: "streaming", item: navStreaming }
        if (bottomIndex === 2) return { type: "nav", tab: "usb",       item: navUsb }

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
        if (activeTab === "library") {
            var id = libraryModel ? libraryModel.trackIdAtRow(browserCursorIndex) : ""
            var fp = libraryModel ? libraryModel.filePathAtRow(browserCursorIndex) : ""
            _applyActiveTrack(browserCursorIndex, id, fp, "library")
            return
        }
        if (activeTab === "usb") {
            var usbTracks = deviceLibraryManager ? deviceLibraryManager.currentTracks : []
            var usbTrack = usbTracks[browserCursorIndex]
            if (usbTrack)
                _applyActiveTrack(browserCursorIndex, usbTrack.trackId || "",
                                  usbTrack.filePath || "", "usb")
            return
        }
        // Covers playlist + all varlist tabs
        var t = currentListTracks[browserCursorIndex]
        if (t) _applyActiveTrack(browserCursorIndex, t.trackId || "", t.filePath || "", activeTab)
    }

    readonly property var _varlistTabs: ["favorites","history","crate","queue","smartcoll"]

    function ensureActiveTrackForCurrentTab() {
        var isNavList = activeTab === "library" || activeTab === "playlist"
                     || activeTab === "usb"
                     || _varlistTabs.indexOf(activeTab) >= 0
        if (!isNavList) {
            browserCursorActive = false
            _applyActiveTrack(-1, "", "", "")
            return
        }

        var list = activeTab === "playlist" ? plTrackList
                 : activeTab === "library"  ? libTrackList
                 : activeTab === "usb"      ? usbTrackList
                 : varlistTrackList
        var count = list ? list.count : 0
        if (count === 0) {
            browserCursorActive = false
            _applyActiveTrack(-1, "", "", "")
            return
        }

        var idx = -1
        if (activeTrackId) {
            if (activeTab === "library") {
                idx = libraryModel ? libraryModel.indexOfTrackId(activeTrackId) : -1
            } else if (activeTab === "usb") {
                var usbRows = deviceLibraryManager ? deviceLibraryManager.currentTracks : []
                for (var usbIndex = 0; usbIndex < usbRows.length; ++usbIndex) {
                    if (usbRows[usbIndex].trackId === activeTrackId) {
                        idx = usbIndex
                        break
                    }
                }
            } else {
                var arr = (activeTab === "playlist") ? sortedPlaylistTracks : currentListTracks
                for (var i = 0; i < arr.length; i++) {
                    if (arr[i].trackId === activeTrackId) { idx = i; break }
                }
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
        } else if (activeTab === "smartcoll" && currentSmartCollectionId) {
            for (var i = 0; i < smartCollections.length; i++) {
                if (smartCollections[i].id === currentSmartCollectionId) {
                    idx = sidebarTopNavCount + i; break
                }
            }
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
        var isNavList = activeTab === "library" || activeTab === "playlist"
                     || activeTab === "usb"
                     || _varlistTabs.indexOf(activeTab) >= 0
        if (!isNavList) return
        var list = activeTab === "playlist" ? plTrackList
                 : activeTab === "library"  ? libTrackList
                 : activeTab === "usb"      ? usbTrackList
                 : varlistTrackList
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

    function getCursorTrackId() {
        if (!browserCursorActive || browserCursorIndex < 0) return ""
        if (activeTrackIndex !== browserCursorIndex) syncActiveTrackFromCursor()
        return activeTrackId || ""
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
            if (entry.tab === "library") librarySubTab = "allSongs"
            focusedPanel = "sidebar"
            forceActiveFocus()
            return
        }
        if (entry.type === "smartcoll") {
            var sc = entry.data
            if (sc) {
                currentSmartCollectionId   = sc.id
                currentSmartCollectionName = sc.name
                activeTab = "smartcoll"
                loadSmartCollectionTracks(sc.id)
            }
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
        loadAllTags()
        loadSmartCollections()
        loadFavorites()
        loadCrate()
        loadQueue()
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
        refreshReferenceKeys()
    }

    Connections { target: deckA; function onTrackMetadataChanged() { libraryRoot.refreshReferenceKeys() } }
    Connections { target: deckB; function onTrackMetadataChanged() { libraryRoot.refreshReferenceKeys() } }
    Connections { target: deckC; function onTrackMetadataChanged() { libraryRoot.refreshReferenceKeys() } }
    Connections { target: deckD; function onTrackMetadataChanged() { libraryRoot.refreshReferenceKeys() } }

    Connections {
        target: typeof deviceLibraryManager !== "undefined" ? deviceLibraryManager : null
        function onDeckLoadReady(deckLetter, request) {
            var deck = deckLetter === "A" ? deckA
                     : deckLetter === "B" ? deckB
                     : deckLetter === "C" ? deckC
                     : deckLetter === "D" ? deckD : null
            if (deck)
                deck.loadExternalTrack(request)
        }
        function onCurrentTracksChanged() {
            if (libraryRoot.activeTab === "usb")
                libraryRoot.ensureActiveTrackForCurrentTab()
        }
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
        function onFavoritesChanged() {
            if (libraryRoot.activeTab === "favorites") libraryRoot.loadFavorites()
        }
        function onCrateChanged() {
            if (libraryRoot.activeTab === "crate") libraryRoot.loadCrate()
        }
        function onQueueChanged() {
            if (libraryRoot.activeTab === "queue") libraryRoot.loadQueue()
        }
        function onHistoryChanged() {
            if (libraryRoot.activeTab === "history")
                libraryRoot.loadHistory(libraryRoot.historyPeriod)
        }
        function onTagsChanged() {
            libraryRoot.loadAllTags()
        }
        function onSmartCollectionsChanged() {
            libraryRoot.loadSmartCollections()
            if (libraryRoot.activeTab === "smartcoll")
                libraryRoot.loadSmartCollectionTracks(libraryRoot.currentSmartCollectionId)
        }
        function onTrackMetaChanged(trackId) {
            var tab = libraryRoot.activeTab
            if      (tab === "history")   libraryRoot.loadHistory()
            else if (tab === "smartcoll") libraryRoot.loadSmartCollectionTracks(libraryRoot.currentSmartCollectionId)
            // Always keep badge tabs current (these are cheap queries).
            libraryRoot.loadFavorites()
            libraryRoot.loadCrate()
            libraryRoot.loadQueue()
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
                if (value !== 0) {
                    if (!libraryRoot.visible) {
                        if (!waveformZoomController)
                            return
                        var steps = Math.max(1, Math.round(Math.abs(value)))
                        for (var step = 0; step < steps; ++step) {
                            if (value > 0)
                                waveformZoomController.zoomIn()
                            else
                                waveformZoomController.zoomOut()
                        }
                    } else if (libraryRoot.touchMode) {
                        libraryRoot.aioMoveBrowseVertical(value)
                    } else {
                        libraryRoot.moveCursor(value)
                    }
                }
            } else if (id === "library_load_deck_a") {
                if (value > 0) libraryRoot.loadTrackToDeck("A", libraryRoot.getCursorFilePath(), libraryRoot.getCursorTrackId())
            } else if (id === "library_load_deck_b") {
                if (value > 0) libraryRoot.loadTrackToDeck("B", libraryRoot.getCursorFilePath(), libraryRoot.getCursorTrackId())
            } else if (id === "library_load_deck_c") {
                if (value > 0) libraryRoot.loadTrackToDeck("C", libraryRoot.getCursorFilePath(), libraryRoot.getCursorTrackId())
            } else if (id === "library_load_deck_d") {
                if (value > 0) libraryRoot.loadTrackToDeck("D", libraryRoot.getCursorFilePath(), libraryRoot.getCursorTrackId())
            } else if (id === "library_playlist_next") {
                if (value > 0) libraryRoot.selectNextPlaylist(1)
            } else if (id === "library_playlist_prev") {
                if (value > 0) libraryRoot.selectNextPlaylist(-1)
            } else if (id === "library_back") {
                if (value > 0) {
                    if (libraryRoot.touchMode)
                        libraryRoot.aioBrowseCursorLeft()
                    else
                        libraryRoot.activeTab = "library"
                }
            } else if (id === "library_expand") {
                if (value > 0) {
                    if (libraryRoot.touchMode)
                        libraryRoot.aioBrowseCursorRight()
                    else if (libraryRoot.currentPlaylistId)
                        libraryRoot.toggleExpanded(libraryRoot.currentPlaylistId)
                }
            } else if (id === "library_collapse") {
                if (value > 0) {
                    if (libraryRoot.touchMode)
                        libraryRoot.aioBrowseCursorLeft()
                    else if (libraryRoot.currentPlaylistId) {
                        var ex = Object.assign({}, libraryRoot.expandedPlaylists)
                        delete ex[libraryRoot.currentPlaylistId]
                        libraryRoot.expandedPlaylists = ex
                    }
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

        if (touchMode) {
            if (event.key === Qt.Key_Up) {
                aioMoveBrowseVertical(-1); event.accepted = true; return
            }
            if (event.key === Qt.Key_Down) {
                aioMoveBrowseVertical(1); event.accepted = true; return
            }
            if (event.key === Qt.Key_Right) {
                aioBrowseCursorRight(); event.accepted = true; return
            }
            if (event.key === Qt.Key_Left) {
                aioBrowseCursorLeft(); event.accepted = true; return
            }
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                if (aioFocusZone === "tracks" && (event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier))) {
                    loadTrackToDeck(event.modifiers & Qt.ShiftModifier ? "B"
                        : (event.modifiers & Qt.ControlModifier ? "C"
                        : (event.modifiers & Qt.AltModifier ? "D" : "A")),
                        getCursorFilePath(), getCursorTrackId())
                } else {
                    aioBrowseCursorActivate()
                }
                event.accepted = true; return
            }
            if (event.key === Qt.Key_P && aioFocusZone === "tracks") {
                togglePreview(getCursorFilePath()); event.accepted = true; return
            }
            if (event.key === Qt.Key_Escape) {
                if (aioBrowseDrilled || aioFocusZone !== "nav")
                    aioBrowseCursorLeft()
                else
                    browserCursorActive = false
                event.accepted = true; return
            }
            return
        }

        if (focusedPanel === "tracks") {
            if (event.key === Qt.Key_Up) {
                moveCursor(-1); event.accepted = true
            } else if (event.key === Qt.Key_Down) {
                moveCursor(1); event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                loadTrackToDeck(event.modifiers & Qt.ShiftModifier ? "B"
                    : (event.modifiers & Qt.ControlModifier ? "C"
                    : (event.modifiers & Qt.AltModifier ? "D" : "A")),
                    getCursorFilePath(), getCursorTrackId())
                event.accepted = true
            } else if (event.key === Qt.Key_P) {
                togglePreview(getCursorFilePath()); event.accepted = true
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

    // ── AIO quick-load bar (tap deck without swipe) ─────────────────────────
    component AioLoadBar: Rectangle {
        id: aioBar
        visible: libraryRoot.touchMode && libraryRoot.activeTrackFilePath.length > 0
        height: visible ? 44 : 0
        color: "#121820"
        clip: true

        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            height: 1; color: libraryRoot.borderMain
        }

        Row {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "LOAD"
                color: libraryRoot.textDim
                font.pixelSize: window.sp(9)
                font.bold: true
                font.letterSpacing: 1.0
            }

            Rectangle {
                width: 44
                height: 32
                anchors.verticalCenter: parent.verticalCenter
                radius: 4
                color: previewBtnMa.containsMouse ? "#253018" : "#1a2510"
                border.color: libraryRoot.activeTrackFilePath.length > 0
                              && typeof libraryPreview !== "undefined" && libraryPreview
                              && libraryPreview.playing
                              && libraryPreview.currentPath === libraryRoot.activeTrackFilePath
                              ? libraryRoot.accentKeyMatch : "#2a4020"
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "♪"
                    color: "#9bdc6a"
                    font.pixelSize: window.sp(14)
                    font.bold: true
                }
                MouseArea {
                    id: previewBtnMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: libraryRoot.togglePreview(libraryRoot.activeTrackFilePath)
                }
            }

            Repeater {
                model: ["A", "B", "C", "D"]
                Rectangle {
                    required property string modelData
                    width: 52
                    height: 32
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 4
                    color: deckLoadMa.containsMouse ? "#1a3a52" : "#132840"
                    border.color: "#1e4070"
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "▶ " + modelData
                        color: "#4a99e0"
                        font.pixelSize: window.sp(11)
                        font.bold: true
                    }
                    MouseArea {
                        id: deckLoadMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: libraryRoot.loadTrackToDeck(modelData,
                                                               libraryRoot.activeTrackFilePath,
                                                               libraryRoot.activeTrackId)
                    }
                }
            }

            Item { width: Math.max(4, parent.width - 280); height: 1 }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(80, parent.width - 300)
                text: {
                    var p = libraryRoot.activeTrackFilePath
                    if (!p) return ""
                    var parts = p.split("/")
                    return parts.length ? parts[parts.length - 1] : p
                }
                color: libraryRoot.textSecond
                font.pixelSize: window.sp(10)
                elide: Text.ElideMiddle
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    // ── Preview transport (AIO + desktop) ─────────────────────────────────────
    component PreviewControlBar: Rectangle {
        id: previewBar
        readonly property var previewPlayer:
            typeof libraryPreview !== "undefined" ? libraryPreview : null
        visible: previewPlayer && previewPlayer.playing
        height: visible ? (libraryRoot.touchMode ? 58 : 50) : 0
        color: "#0e1418"
        clip: true

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: libraryRoot.borderMain
        }

        function basename(path) {
            if (!path)
                return ""
            var parts = path.split("/")
            return parts.length ? parts[parts.length - 1] : path
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: libraryRoot.touchMode ? 10 : 8
            anchors.rightMargin: libraryRoot.touchMode ? 10 : 8
            spacing: libraryRoot.touchMode ? 10 : 8

            Rectangle {
                Layout.preferredWidth: libraryRoot.touchMode ? 44 : 36
                Layout.preferredHeight: libraryRoot.touchMode ? 40 : 32
                radius: 4
                color: stopMa.containsMouse ? "#3a1818" : "#251010"
                border.color: libraryRoot.accentRed
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "■"
                    color: libraryRoot.accentRed
                    font.pixelSize: window.sp(libraryRoot.touchMode ? 14 : 11)
                }
                MouseArea {
                    id: stopMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (previewBar.previewPlayer)
                            previewBar.previewPlayer.stop()
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Text {
                    Layout.fillWidth: true
                    text: previewBar.basename(previewBar.previewPlayer
                                               ? previewBar.previewPlayer.currentPath : "")
                    color: libraryRoot.textSecond
                    font.pixelSize: window.sp(9)
                    elide: Text.ElideMiddle
                    maximumLineCount: 1
                }

                Slider {
                    id: previewSeek
                    Layout.fillWidth: true
                    Layout.preferredHeight: libraryRoot.touchMode ? 28 : 22
                    from: 0
                    to: 1
                    live: true
                    value: previewBar.previewPlayer ? previewBar.previewPlayer.progress : 0

                    onPressedChanged: {
                        if (!pressed && previewBar.previewPlayer)
                            previewBar.previewPlayer.seekProgress(value)
                    }
                    onMoved: {
                        if (pressed && previewBar.previewPlayer)
                            previewBar.previewPlayer.seekProgress(value)
                    }

                    Connections {
                        target: previewBar.previewPlayer
                        enabled: previewBar.previewPlayer !== null
                        function onPositionChanged() {
                            if (!previewSeek.pressed && previewBar.previewPlayer)
                                previewSeek.value = previewBar.previewPlayer.progress
                        }
                        function onPlayingChanged() {
                            if (previewBar.previewPlayer && !previewBar.previewPlayer.playing)
                                previewSeek.value = 0
                        }
                    }
                }
            }

            Text {
                text: libraryRoot.formatPreviewTime(previewBar.previewPlayer
                                                      ? previewBar.previewPlayer.positionSec : 0)
                      + " / " + libraryRoot.formatPreviewTime(previewBar.previewPlayer
                                                               ? previewBar.previewPlayer.durationSec : 0)
                color: libraryRoot.textPrimary
                font.pixelSize: window.sp(libraryRoot.touchMode ? 11 : 10)
                font.family: "monospace"
                Layout.preferredWidth: libraryRoot.touchMode ? 96 : 84
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    // ── AIO touch tile (CDJ/XDJ-style icon + label) ───────────────────────────
    component AioNavTile: Rectangle {
        id: aioTile
        required property string tileIcon
        required property string tileLabel
        property int navIndex: -1
        property int tileBadge: -1
        property bool tileActive: false
        property bool tileCursor: false
        property bool compact: true
        property color tileAccent: libraryRoot.accentBlueLt
        signal tapped()

        width: parent ? parent.width : libraryRoot.aioNavTileW
        height: libraryRoot.aioNavTileH
        clip: true
        radius: 0
        color: tileMa.pressed ? "#1a2838"
              : tileCursor ? "#1a3048"
              : "#0e141c"
        border.color: tileCursor ? libraryRoot.accentGreen
                    : tileActive ? aioTile.tileAccent : "#2a3848"
        border.width: tileCursor ? 2 : 1

        Column {
            anchors.centerIn: parent
            width: parent.width - 4
            spacing: compact ? 0 : 4

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: aioTile.tileIcon
                color: tileActive ? aioTile.tileAccent : "#7a9ab8"
                font.pixelSize: window.sp(compact ? 14 : 20)
            }
            Text {
                width: parent.width
                text: aioTile.tileLabel
                color: tileActive ? libraryRoot.textPrimary : libraryRoot.textSecond
                font.pixelSize: window.sp(compact ? 6 : 7)
                font.bold: tileActive
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                maximumLineCount: compact ? 1 : 2
                wrapMode: Text.NoWrap
            }
        }

        Rectangle {
            visible: aioTile.tileBadge > 0
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 2
            width: Math.max(16, badgeLbl.implicitWidth + 4)
            height: 12
            radius: 0
            color: "#1a3050"
            border.color: aioTile.tileAccent
            border.width: 1
            Text {
                id: badgeLbl
                anchors.centerIn: parent
                text: aioTile.tileBadge > 99 ? "99+" : String(aioTile.tileBadge)
                color: aioTile.tileAccent
                font.pixelSize: window.sp(8)
                font.bold: true
                font.family: "monospace"
            }
        }

        MouseArea {
            id: tileMa
            anchors.fill: parent
            onClicked: aioTile.tapped()
        }
    }

    // ── AIO split-browse picker (artists/albums/keys in library pane) ─────────
    component AioBrowsePicker: Rectangle {
        id: browsePicker
        property string categoryLabel: ""
        property var entries: []
        property string focusKey: ""

        color: libraryRoot.bgSidebar
        clip: true

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 2
            color: libraryRoot.accentGreen
            visible: libraryRoot.aioCursorPickerActive
            z: 2
        }

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: libraryRoot.borderMain
        }

        Text {
            id: browsePickerHdr
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 8
            height: 22
            text: browsePicker.categoryLabel
            color: libraryRoot.textSecond
            font.pixelSize: window.sp(9)
            font.bold: true
            font.letterSpacing: 0.8
            verticalAlignment: Text.AlignVCenter
        }

        ListView {
            id: browsePickerList
            anchors.top: browsePickerHdr.bottom
            anchors.topMargin: 4
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            clip: true
            model: browsePicker.entries
            currentIndex: libraryRoot.aioBrowseFocusIndex
            highlightRangeMode: ListView.ApplyRange
            preferredHighlightBegin: 40
            preferredHighlightEnd: 120
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: browseRow
                required property var modelData
                required property int index
                readonly property string rowName: modelData.name || modelData.label || ""
                width: browsePickerList.width
                height: rowFocused ? 52 : 40
                color: rowMa.pressed ? libraryRoot.bgRowHover
                     : rowFocused ? libraryRoot.bgRowActive
                     : (index % 2 === 0 ? libraryRoot.bgRowEven : libraryRoot.bgRowOdd)

                readonly property bool rowFocused:
                    (libraryRoot.aioFocusZone === "picker" && index === libraryRoot.aioBrowseFocusIndex)
                    || (libraryRoot.aioFocusZone !== "picker"
                        && libraryRoot.aioBrowseEntryKey(modelData) === browsePicker.focusKey)

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: rowFocused ? 3 : 0
                    color: libraryRoot.accentBlueLt
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 10
                    anchors.rightMargin: 6
                    spacing: rowFocused ? 2 : 0

                    Text {
                        width: parent.width
                        text: browseRow.rowName || "?"
                        color: rowFocused ? libraryRoot.textPrimary : libraryRoot.textSecond
                        font.pixelSize: window.sp(rowFocused ? 11 : 10)
                        font.bold: rowFocused
                        elide: Text.ElideRight
                        maximumLineCount: rowFocused ? 2 : 1
                    }
                    Text {
                        width: parent.width
                        visible: modelData.trackCount > 0
                        text: modelData.trackCount + (modelData.trackCount === 1 ? " track" : " tracks")
                        color: rowFocused ? libraryRoot.accentBlueLt : libraryRoot.textDim
                        font.pixelSize: window.sp(8)
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    id: rowMa
                    anchors.fill: parent
                    onClicked: libraryRoot.aioActivateBrowsePickerEntry(index)
                }
            }
        }
    }

    component AioQuickBtn: Rectangle {
        id: qbtn
        required property string btnIcon
        required property string btnLabel
        property color btnColor: libraryRoot.accentBlueLt
        signal tapped()

        width: 46
        height: parent ? parent.height - 8 : 52
        radius: 4
        color: qMa.pressed ? "#1a3048" : "#121820"
        border.color: qbtn.btnColor
        border.width: 1

        Column {
            anchors.centerIn: parent
            spacing: 2
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qbtn.btnIcon
                color: qbtn.btnColor
                font.pixelSize: window.sp(14)
                font.bold: true
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qbtn.btnLabel
                color: libraryRoot.textSecond
                font.pixelSize: window.sp(7)
                font.bold: true
                font.letterSpacing: 0.4
            }
        }
        MouseArea {
            id: qMa
            anchors.fill: parent
            onClicked: qbtn.tapped()
        }
    }

    // ── Inline component: track row (shared between library + playlist) ────
    component TrackRow: Item {
        id: tr
        property int    rowIndex: 0
        property string rowTrackId: ""
        property string rowTitle: ""
        property string rowArtist: ""
        property string rowAlbum: ""
        property string rowGenre: ""
        property int    rowDurationSec: 0
        property real   rowBpm: 0
        property string rowKey: ""
        property int    rowBitrateKbps: 0
        property bool   rowIsAnalyzed: false
        property string rowFilePath: ""
        property string rowArtworkPath: ""
        property string rowColor:   ""
        property int    rowRating:  0
        property bool   rowIsHistory: false
        property double rowPlayedAt: 0
        property int    rowPlayEventIndex: 0
        property int    rowTrackPlayCount: 0
        property real viewWidth: parent ? parent.width : 0
        property bool isPlaylistTrack: false
        property string playlistId: ""
        property string rowSourceTab: "library"
        property real swipeX: 0
        property bool swipeActive: false
        property bool swipeGestureDone: false
        property bool swipeLocked: false
        property int lastTapMs: 0

        Behavior on swipeX {
            enabled: !tr.swipeActive
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

        readonly property bool touchMode: window.allInOneMode
        readonly property bool swipeOpen: Math.abs(tr.swipeX) > 40
        readonly property int keyMatch: libraryRoot.keyMatchLevel(tr.rowKey)
        readonly property bool previewActive: typeof libraryPreview !== "undefined" && libraryPreview
                                              && libraryPreview.playing
                                              && libraryPreview.currentPath === tr.rowFilePath
        readonly property int deckActionW: tr.touchMode ? 184 : 0
        readonly property int miscActionW: tr.touchMode ? 148 : 0
        readonly property real slideX: tr.touchMode ? (-tr.deckActionW + tr.swipeX) : 0
        property string rowCoverUrl: ""

        function refreshCoverUrl() {
            if (tr.rowArtworkPath) {
                tr.rowCoverUrl = "file://" + encodeURI(tr.rowArtworkPath)
                    .replace(/#/g, "%23").replace(/\?/g, "%3F")
                return
            }
            if (typeof libraryCover === "undefined" || !libraryCover || !tr.rowFilePath) {
                tr.rowCoverUrl = ""
                return
            }
            tr.rowCoverUrl = libraryCover.urlForPath(tr.rowFilePath, tr.rowTrackId)
        }

        function closeSwipe() {
            tr.swipeX = 0
            if (libraryRoot.openSwipeRowIndex === tr.rowIndex)
                libraryRoot.openSwipeRowIndex = -1
        }

        readonly property bool isCursorRow: libraryRoot.browserCursorActive
                                           && libraryRoot.browserCursorIndex === rowIndex
                                           && libraryRoot.aioCursorTracksActive
                                           && (isPlaylistTrack
                                               ? libraryRoot.activeTab === "playlist"
                                               : libraryRoot.activeTab === rowSourceTab)

        readonly property color artBgColor: {
            var s = rowTitle || rowArtist || ""
            if (s.length === 0) return "#1a2535"
            var h = 0
            for (var i = 0; i < s.length && i < 20; i++) h = (h * 31 + s.charCodeAt(i)) & 0xFFFF
            return Qt.hsla((h % 360) / 360.0, 0.42, 0.20, 1.0)
        }

        height: libraryRoot.trackRowHeight()
        width: viewWidth > 0 ? viewWidth : (parent ? parent.width : 0)
        clip: true

        Rectangle {
            anchors.fill: parent
            color: "#141414"
        }

        Connections {
            target: libraryRoot
            function onOpenSwipeRowIndexChanged() {
                if (libraryRoot.openSwipeRowIndex !== tr.rowIndex && tr.swipeX !== 0)
                    tr.swipeX = 0
            }
        }
        Connections {
            target: typeof libraryCover !== "undefined" ? libraryCover : null
            function onCoverReady(path, trackId, imageUrl) {
                if (path === tr.rowFilePath || trackId === tr.rowTrackId)
                    tr.rowCoverUrl = imageUrl
            }
        }
        Component.onCompleted: {
            if (!tr.rowArtworkPath && typeof libraryCover !== "undefined" && libraryCover && tr.rowFilePath) {
                libraryCover.preload(tr.rowFilePath, tr.rowTrackId)
                tr.refreshCoverUrl()
            }
        }
        onRowFilePathChanged: {
            tr.rowCoverUrl = ""
            if (!tr.rowArtworkPath && typeof libraryCover !== "undefined" && libraryCover && tr.rowFilePath)
                libraryCover.preload(tr.rowFilePath, tr.rowTrackId)
        }
        onRowArtworkPathChanged: tr.refreshCoverUrl()
        onRowTrackIdChanged: tr.refreshCoverUrl()

        Row {
            id: slideRow
            height: parent.height
            x: tr.slideX
            Behavior on x {
                enabled: !tr.swipeActive
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }

            // Deck load strip — revealed by swipe right
            Row {
                width: tr.deckActionW
                height: parent.height
                spacing: 0
                visible: tr.deckActionW > 0

                Repeater {
                    model: ["A", "B", "C", "D"]
                    Rectangle {
                        required property string modelData
                        width: 46; height: parent.height
                        color: loadDeckMa.containsMouse ? "#1a3a52" : "#132840"
                        border.color: "#1e4070"; border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "▶ " + modelData
                            color: "#4a99e0"; font.pixelSize: window.sp(11); font.bold: true
                        }
                        MouseArea {
                            id: loadDeckMa; anchors.fill: parent
                            hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: (mouse) => {
                                var deck = modelData
                                var path = tr.rowFilePath
                                tr.closeSwipe()
                                mouse.accepted = true
                                libraryRoot.loadTrackToDeck(deck, path, tr.rowTrackId)
                            }
                        }
                    }
                }
            }

            Rectangle {
                id: trContent
                width: tr.viewWidth > 0 ? tr.viewWidth : tr.width
                height: parent.height
                color: trMouse.containsMouse
                       ? libraryRoot.bgRowHover
                     : (tr.keyMatch === 2 ? "#142218"
                     : (tr.keyMatch === 1 ? "#121820"
                     : (isCursorRow
                        ? "#163328"
                        : (rowIndex % 2 === 0 ? libraryRoot.bgRowEven : libraryRoot.bgRowOdd))))
                opacity: trDragPayload.dragging ? 0.4 : 1.0

        Rectangle {
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: tr.rowColor !== "" ? 3 : 2
            color: tr.isCursorRow ? libraryRoot.accentGreen
                 : tr.rowColor !== "" ? tr.rowColor
                 : libraryRoot.accentBlue
            visible: trMouse.containsMouse || tr.isCursorRow || tr.rowColor !== ""
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

            Item {
                width: libraryRoot.colStatus
                height: 20
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    anchors.centerIn: parent
                    width: 18; height: 18; radius: 3
                    color: tr.artBgColor; clip: true
                    Image {
                        id: trCoverCompact
                        anchors.fill: parent
                        source: tr.rowCoverUrl
                        fillMode: Image.PreserveAspectCrop
                        visible: status === Image.Ready && tr.rowCoverUrl !== ""
                        asynchronous: true
                        cache: false
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: trCoverCompact.status !== Image.Ready || tr.rowCoverUrl === ""
                        text: (tr.rowTitle || tr.rowArtist || "?").charAt(0).toUpperCase()
                        color: Qt.rgba(1, 1, 1, 0.55)
                        font.pixelSize: window.sp(9); font.bold: true
                    }
                }
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
                text: tr.rowIsHistory
                      ? ((tr.rowArtist || "—") + (tr.rowPlayedAt > 0 ? "  ·  " + libraryRoot.formatHistoryDate(tr.rowPlayedAt) : ""))
                      : ((tr.rowArtist || "—")
                         + (tr.rowAlbum ? "  ·  " + tr.rowAlbum : "")
                         + (tr.rowGenre ? "  ·  " + tr.rowGenre : ""))
                color: libraryRoot.textSecond
                font.pixelSize: window.sp(11)
                elide: Text.ElideRight
            }
            Text {
                width: libraryRoot.colTime
                anchors.verticalCenter: parent.verticalCenter
                text: tr.rowIsHistory && tr.rowPlayedAt > 0
                      ? libraryRoot.formatHistoryTime(tr.rowPlayedAt)
                      : tr.rowDurationSec > 0
                      ? (Math.floor(tr.rowDurationSec / 60) + ":" + ("0" + (tr.rowDurationSec % 60)).slice(-2))
                      : "—"
                color: tr.rowIsHistory ? libraryRoot.accentBlueLt
                    : (tr.rowDurationSec > 0 ? libraryRoot.textMeta : libraryRoot.textDim)
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
                color: libraryRoot.keyMatchColor(tr.rowKey)
                font.pixelSize: window.sp(11); font.family: "monospace"
                font.bold: tr.keyMatch === 2
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
            Row {
                width: 44
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                Repeater {
                    model: 5
                    Rectangle {
                        width: 5; height: 5; radius: 2.5
                        anchors.verticalCenter: parent.verticalCenter
                        color: (index < tr.rowRating) ? "#e8b84b" : "#1e1e1e"
                        border.color: (index < tr.rowRating) ? "#c89a30" : "#2a2a2a"
                        border.width: 1
                    }
                }
            }
        }

        // ── Normal layout ─────────────────────────────────────────────────────
        Item {
            visible: libraryRoot.viewMode === "normal"
            anchors.fill: parent

            // Cover art
            Rectangle {
                id: trArtBox
                anchors.left: parent.left; anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: tr.touchMode ? 52 : 44
                height: tr.touchMode ? 52 : 44
                radius: 5
                color: tr.artBgColor
                clip: true

                Image {
                    id: trCoverImage
                    anchors.fill: parent
                    source: tr.rowCoverUrl
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready && tr.rowCoverUrl !== ""
                    asynchronous: true
                    cache: false
                }

                Rectangle {
                    anchors.top: parent.top; anchors.right: parent.right
                    anchors.topMargin: 3; anchors.rightMargin: 3
                    width: 8; height: 8; radius: 4
                    color: tr.rowIsAnalyzed ? libraryRoot.accentGreen : "#252525"
                    border.color: Qt.rgba(0, 0, 0, 0.4); border.width: 1
                    z: 2
                }

                Text {
                    anchors.centerIn: parent
                    visible: trCoverImage.status !== Image.Ready || tr.rowCoverUrl === ""
                    text: (tr.rowTitle || tr.rowArtist || "?").charAt(0).toUpperCase()
                    color: Qt.rgba(1, 1, 1, 0.60)
                    font.pixelSize: window.sp(tr.touchMode ? 19 : 17); font.bold: true
                }
            }

            // Title + Artist column
            Column {
                anchors.left: trArtBox.right; anchors.leftMargin: 12
                anchors.right: tr.touchMode ? trTouchMeta.right : trInfoCol.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: tr.rowIsHistory ? 3 : 5

                Text {
                    width: parent.width
                    text: tr.rowTitle || "—"
                    color: libraryRoot.textPrimary
                    font.pixelSize: window.sp(12); font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: tr.rowIsHistory
                    text: libraryRoot.formatHistoryStamp(tr.rowPlayedAt)
                    color: libraryRoot.accentBlueLt
                    font.pixelSize: window.sp(9)
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: (tr.rowArtist || "—")
                          + (tr.rowAlbum ? "  ·  " + tr.rowAlbum : "")
                          + (tr.rowGenre ? "  ·  " + tr.rowGenre : "")
                    color: libraryRoot.textSecond
                    font.pixelSize: window.sp(10)
                    elide: Text.ElideRight
                }
                Row {
                    visible: tr.touchMode
                    spacing: 10
                    Text {
                        text: tr.rowBpm > 0 ? tr.rowBpm.toFixed(1) + " BPM" : "— BPM"
                        color: tr.rowBpm > 0 ? libraryRoot.accentGreen : libraryRoot.textDim
                        font.pixelSize: window.sp(9); font.family: "monospace"
                    }
                    Text {
                        text: tr.rowKey || "—"
                        color: libraryRoot.keyMatchColor(tr.rowKey)
                        font.bold: tr.keyMatch === 2
                        font.pixelSize: window.sp(9); font.family: "monospace"
                    }
                }
            }

            // Right meta (touch: duration only; desktop: full info column)
            Column {
                id: trTouchMeta
                visible: tr.touchMode
                anchors.right: parent.right; anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Text {
                    anchors.right: parent.right
                    text: tr.rowDurationSec > 0
                          ? (Math.floor(tr.rowDurationSec / 60) + ":" + ("0" + (tr.rowDurationSec % 60)).slice(-2))
                          : "—"
                    color: tr.rowDurationSec > 0 ? libraryRoot.textMeta : libraryRoot.textDim
                    font.pixelSize: window.sp(10); font.family: "monospace"
                }
            }

            // Right info column (desktop)
            Column {
                id: trInfoCol
                visible: !tr.touchMode
                anchors.right: parent.right; anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Text {
                    anchors.right: parent.right
                    text: tr.rowIsHistory && tr.rowPlayedAt > 0
                          ? libraryRoot.formatHistoryTime(tr.rowPlayedAt)
                          : tr.rowDurationSec > 0
                          ? (Math.floor(tr.rowDurationSec / 60) + ":" + ("0" + (tr.rowDurationSec % 60)).slice(-2))
                          : "—"
                    color: tr.rowIsHistory ? libraryRoot.accentBlueLt
                        : (tr.rowDurationSec > 0 ? libraryRoot.textMeta : libraryRoot.textDim)
                    font.pixelSize: window.sp(11); font.family: "monospace"
                }

                Text {
                    anchors.right: parent.right
                    visible: tr.rowIsHistory && tr.rowTrackPlayCount > 0
                    text: "Play #" + tr.rowTrackPlayCount
                    color: libraryRoot.textMeta
                    font.pixelSize: window.sp(9)
                    font.family: "monospace"
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
                        color: libraryRoot.keyMatchColor(tr.rowKey)
                        font.bold: tr.keyMatch === 2
                        font.pixelSize: window.sp(10); font.family: "monospace"
                    }
                }
                Row {
                    anchors.right: parent.right
                    spacing: 2
                    visible: tr.rowRating > 0
                    Repeater {
                        model: 5
                        Rectangle {
                            width: 6; height: 6; radius: 3
                            color: (index < tr.rowRating) ? "#e8b84b" : "#1e1e1e"
                            border.color: (index < tr.rowRating) ? "#c89a30" : "#2a2a2a"
                            border.width: 1
                        }
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

        DropArea {
            id: trDropArea
            anchors.fill: parent
            keys: ["playlist-track-reorder"]
            enabled: tr.isPlaylistTrack && !tr.touchMode
            property int dropPosition: 0

            onEntered: {
                if (tr.isPlaylistTrack) {
                    var dragIndex = parseInt(drag.getDataAsString("playlist-track-index") || "-1")
                    if (dragIndex >= 0 && dragIndex !== tr.rowIndex)
                        trDropArea.dropPosition = mouse.y < height / 2 ? 0 : 1
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
            cursorShape: tr.touchMode
                         ? (tr.swipeActive ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                         : (trDragPayload.dragging ? Qt.DragMoveCursor : Qt.PointingHandCursor)
            property real pressX: 0
            property real pressY: 0
            property bool rightDragging: false

            onPressed: (mouse) => {
                tr.swipeGestureDone = false
                tr.swipeLocked = false
                if (tr.swipeOpen && !tr.touchMode)
                    tr.closeSwipe()

                libraryRoot.setActiveTrackFromRow(
                    tr.rowIndex,
                    tr.rowTrackId,
                    tr.rowFilePath,
                    tr.isPlaylistTrack ? "playlist" : tr.rowSourceTab)
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
                var dx = mouse.x - pressX
                var dy = mouse.y - pressY
                var moved = Math.abs(dx) + Math.abs(dy) >= 8

                // AIO: horizontal swipe reveals deck/misc actions; yield to vertical scroll.
                if (tr.touchMode && !trDragPayload.dragging) {
                    if (!tr.swipeLocked && moved) {
                        if (Math.abs(dy) > Math.abs(dx) * 1.25)
                            return
                        if (Math.abs(dx) > Math.abs(dy) * 1.25 && Math.abs(dx) > 12)
                            tr.swipeLocked = true
                    }
                    if (tr.swipeLocked || tr.swipeActive) {
                        tr.swipeActive = true
                        tr.swipeX = Math.max(-tr.miscActionW, Math.min(tr.deckActionW, dx))
                        if (Math.abs(tr.swipeX) > 16)
                            libraryRoot.openSwipeRowIndex = tr.rowIndex
                        mouse.accepted = true
                        return
                    }
                }

                if (!tr.touchMode && !trDragPayload.dragging && moved && !tr.swipeActive) {
                    trDragPayload.dragging = true
                    trDragPayload.Drag.active = true
                    if (mouse.buttons & Qt.RightButton) rightDragging = true
                }
            }
            onReleased: (mouse) => {
                if (tr.swipeActive) {
                    if (tr.swipeX >= tr.deckActionW * 0.30)
                        tr.swipeX = tr.deckActionW
                    else if (tr.swipeX <= -tr.miscActionW * 0.30)
                        tr.swipeX = -tr.miscActionW
                    else
                        tr.closeSwipe()
                    tr.swipeActive = false
                    tr.swipeGestureDone = true
                    trDragPayload.Drag.active = false
                    trDragPayload.dragging = false
                    rightDragging = false
                    mouse.accepted = true
                    return
                }
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
                tr.swipeActive = false
                tr.swipeLocked = false
                tr.closeSwipe()
                trDragPayload.Drag.active = false
                trDragPayload.dragging = false
                rightDragging = false
            }
            onClicked: (mouse) => {
                if (tr.swipeGestureDone) {
                    tr.swipeGestureDone = false
                    mouse.accepted = true
                    return
                }
                if (mouse.button === Qt.LeftButton && !(mouse.modifiers & Qt.ControlModifier)) {
                    if (tr.touchMode && !tr.swipeOpen) {
                        var now = Date.now()
                        if (now - tr.lastTapMs < 380)
                            libraryRoot.loadTrackToDeck("A", tr.rowFilePath, tr.rowTrackId)
                        tr.lastTapMs = now
                    }
                    libraryRoot.focusedPanel = "tracks"
                    libraryRoot.forceActiveFocus()
                }
            }
            onDoubleClicked: (mouse) => {
                if (mouse.button === Qt.LeftButton) {
                    libraryRoot.loadTrackToDeck("A", tr.rowFilePath, tr.rowTrackId)
                    mouse.accepted = true
                }
            }
        }

            } // trContent

            Row {
                width: tr.miscActionW
                height: parent.height
                spacing: 0
                visible: tr.miscActionW > 0

                Rectangle {
                    width: 50; height: parent.height
                    color: favoriteMa.containsMouse ? "#2a2510" : "#1a1810"
                    Text { anchors.centerIn: parent; text: "★"; color: "#e8b84b"; font.pixelSize: window.sp(16) }
                    MouseArea {
                        id: favoriteMa; anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (libraryDb && tr.rowTrackId) {
                                if (libraryDb.isFavorite(tr.rowTrackId))
                                    libraryDb.removeFromFavorites(tr.rowTrackId)
                                else
                                    libraryDb.addToFavorites(tr.rowTrackId)
                                libraryRoot.loadFavorites()
                            }
                            tr.closeSwipe()
                        }
                    }
                }
                Rectangle {
                    width: 50; height: parent.height
                    color: crateMa.containsMouse ? "#1a2a1a" : "#121a12"
                    Text { anchors.centerIn: parent; text: "⊞"; color: libraryRoot.accentGreen; font.pixelSize: window.sp(14) }
                    MouseArea {
                        id: crateMa; anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (libraryDb && tr.rowTrackId)
                                libraryDb.addToPrepareCrate(tr.rowTrackId)
                            libraryRoot.loadCrate()
                            tr.closeSwipe()
                        }
                    }
                }
                Rectangle {
                    width: 48; height: parent.height
                    color: queueMa.containsMouse ? "#1a2a3a" : "#121820"
                    Text { anchors.centerIn: parent; text: "►"; color: libraryRoot.accentBlueLt; font.pixelSize: window.sp(14) }
                    MouseArea {
                        id: queueMa; anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (libraryDb && tr.rowTrackId)
                                libraryDb.enqueueTrack(tr.rowTrackId)
                            libraryRoot.loadQueue()
                            tr.closeSwipe()
                        }
                    }
                }
            }
        } // slideRow

        Rectangle {
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            height: 1; color: libraryRoot.borderSub
        }
    } // TrackRow

    // ── Inline component: sidebar nav button ──────────────────────────────
    component NavButton: Rectangle {
        id: navBtn
        required property string tabKey
        required property string btnIcon
        required property string btnLabel
        property int badgeCount: -1
        property var customAction: null
        property var onContextMenu: null
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
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.PointingHandCursor
            property real pressX: 0
            property real pressY: 0
            onPressed: (mouse) => { pressX = mouse.x; pressY = mouse.y }
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton) {
                    if (navBtn.onContextMenu) navBtn.onContextMenu(mouse)
                    return
                }
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
            Layout.minimumWidth: libraryRoot.touchMode ? 0 : libraryRoot.sidebarW
            Layout.maximumWidth: libraryRoot.touchMode ? 0 : libraryRoot.sidebarW
            Layout.fillHeight: true
            visible: !libraryRoot.touchMode
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

                    Item { width: parent.width; height: 4 }

                    // ── TOOLS ─────────────────────────────────────────────────
                    Item {
                        width: parent.width; height: 26
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            height: 1; color: libraryRoot.borderSub
                        }
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 14
                            anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                            text: "TOOLS"
                            color: "#484848"
                            font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.3
                        }
                    }

                    NavButton {
                        id: navFavorites
                        tabKey: "favorites"
                        btnIcon: "★"
                        btnLabel: "Favorites"
                        badgeCount: libraryRoot.favoriteTracks.length > 0 ? libraryRoot.favoriteTracks.length : -1
                        cursorIndex: 1
                    }
                    NavButton {
                        id: navHistory
                        tabKey: "history"
                        btnIcon: "⏱"
                        btnLabel: "History"
                        cursorIndex: 2
                    }
                    NavButton {
                        id: navCrate
                        tabKey: "crate"
                        btnIcon: "⊞"
                        btnLabel: "Prepare Crate"
                        badgeCount: libraryRoot.prepareCrateTracks.length > 0 ? libraryRoot.prepareCrateTracks.length : -1
                        cursorIndex: 3
                    }
                    NavButton {
                        id: navQueue
                        tabKey: "queue"
                        btnIcon: "►"
                        btnLabel: "Queue"
                        badgeCount: libraryRoot.queueTracks.length > 0 ? libraryRoot.queueTracks.length : -1
                        cursorIndex: 4
                    }

                    Item { width: parent.width; height: 4 }

                    // ── SMART COLLECTIONS ─────────────────────────────────────
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
                            text: "SMART"
                            color: "#484848"
                            font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.3
                        }
                        Rectangle {
                            anchors.right: parent.right; anchors.rightMargin: 10
                            anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                            width: 18; height: 18; radius: 3
                            color: newScMa.containsMouse ? "#2a3a4a" : "transparent"
                            border.color: newScMa.containsMouse ? "#2d7dd2" : "#2a2a2a"
                            border.width: 1
                            Text { anchors.centerIn: parent; text: "+"; color: "#aaa"; font.pixelSize: window.sp(12) }
                            MouseArea {
                                id: newScMa; anchors.fill: parent
                                hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: createSmartCollDialog.open()
                            }
                        }
                    }

                    Repeater {
                        id: smartCollRepeater
                        model: libraryRoot.smartCollections
                        NavButton {
                            required property var modelData
                            required property int index
                            tabKey: "smartcoll"
                            btnIcon: "◈"
                            btnLabel: modelData.name
                            cursorIndex: libraryRoot.sidebarTopNavCount + index
                            customAction: function() {
                                libraryRoot.currentSmartCollectionId   = modelData.id
                                libraryRoot.currentSmartCollectionName = modelData.name
                                libraryRoot.loadSmartCollectionTracks(modelData.id)
                            }
                            onContextMenu: function(mouse) {
                                smartCollContextMenu.targetId = modelData.id
                                smartCollContextMenu.targetName = modelData.name
                                smartCollContextMenu.targetData = modelData
                                libraryRoot._popupMenuAt(smartCollContextMenu)
                            }
                            readonly property bool isThisSelected:
                                libraryRoot.activeTab === "smartcoll" &&
                                libraryRoot.currentSmartCollectionId === modelData.id
                        }
                    }

                    Item { width: parent.width; height: 4 }

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
                        cursorIndex: libraryRoot.sidebarTopNavCount + libraryRoot.sidebarSmartCollCount + libraryRoot.visiblePlaylists.length
                    }

                    NavButton {
                        id: navStreaming
                        tabKey: "streaming"
                        btnIcon: "◎"
                        btnLabel: "Streaming"
                        cursorIndex: libraryRoot.sidebarTopNavCount + libraryRoot.sidebarSmartCollCount + libraryRoot.visiblePlaylists.length + 1
                    }

                    NavButton {
                        id: navUsb
                        tabKey: "usb"
                        btnIcon: "⊕"
                        btnLabel: "Devices"
                        badgeCount: typeof deviceLibraryManager !== "undefined" && deviceLibraryManager
                                    ? deviceLibraryManager.devices.length : -1
                        cursorIndex: libraryRoot.sidebarTopNavCount + libraryRoot.sidebarSmartCollCount + libraryRoot.visiblePlaylists.length + 2
                    }

                    Item { width: parent.width; height: 12 }
                }
            }
        }

        // ════════════════════════════════════════════════════════════════════
        // AIO NAV PANEL (permanent left tile column)
        // ════════════════════════════════════════════════════════════════════
        Rectangle {
            id: aioNavPanel
            Layout.preferredWidth: libraryRoot.aioNavPanelW
            Layout.minimumWidth: libraryRoot.touchMode ? libraryRoot.aioNavPanelW : 0
            Layout.maximumWidth: libraryRoot.touchMode ? libraryRoot.aioNavPanelW : 0
            Layout.fillHeight: true
            visible: libraryRoot.touchMode
            color: libraryRoot.bgSidebar
            clip: true

            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: libraryRoot.borderMain
                z: 2
            }

            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 2
                color: libraryRoot.accentGreen
                visible: libraryRoot.aioCursorNavActive
                z: 3
            }

            // ── Main category menu (always visible) ────────────────────────
            Column {
                anchors.fill: parent
                anchors.margins: libraryRoot.aioNavPad
                spacing: libraryRoot.aioNavGap

                readonly property real cellW: width

                AioNavTile {
                    width: parent.cellW
                    navIndex: 0
                    tileIcon: "⊙"; tileLabel: "SOURCE"
                    tileActive: libraryRoot.aioBrowseScreen === "source"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 0
                    onTapped: { libraryRoot.aioNavCursorIndex = 0; libraryRoot.aioEnterBrowse("source") }
                }
                AioNavTile {
                    width: parent.cellW
                    navIndex: 1
                    tileIcon: "♪"; tileLabel: "ARTIST"
                    tileBadge: libraryRoot.aioArtistList.length > 0
                               ? libraryRoot.aioArtistList.length : -1
                    tileActive: libraryRoot.aioBrowseScreen === "artist"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 1
                    onTapped: { libraryRoot.aioNavCursorIndex = 1; libraryRoot.aioEnterBrowse("artist") }
                }
                AioNavTile {
                    width: parent.cellW
                    navIndex: 2
                    tileIcon: "◻"; tileLabel: "ALBUM"
                    tileBadge: libraryRoot.aioAlbumList.length > 0
                               ? libraryRoot.aioAlbumList.length : -1
                    tileActive: libraryRoot.aioBrowseScreen === "album"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 2
                    onTapped: { libraryRoot.aioNavCursorIndex = 2; libraryRoot.aioEnterBrowse("album") }
                }
                AioNavTile {
                    width: parent.cellW
                    navIndex: 3
                    tileIcon: "≡"; tileLabel: "ALL TRACKS"
                    tileBadge: libraryModel ? libraryModel.count : -1
                    tileActive: libraryRoot.aioBrowseScreen === "home"
                              && libraryRoot.activeTab === "library"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 3
                    onTapped: { libraryRoot.aioNavCursorIndex = 3; libraryRoot.aioOpenAllTracks() }
                }
                AioNavTile {
                    width: parent.cellW
                    navIndex: 4
                    tileIcon: "♯"; tileLabel: "KEY"
                    tileBadge: libraryRoot.aioKeyList.length > 0
                               ? libraryRoot.aioKeyList.length : -1
                    tileActive: libraryRoot.aioBrowseScreen === "key"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 4
                    onTapped: { libraryRoot.aioNavCursorIndex = 4; libraryRoot.aioEnterBrowse("key") }
                }
                AioNavTile {
                    width: parent.cellW
                    navIndex: 5
                    tileIcon: "☰"; tileLabel: "PLAYLIST"
                    tileBadge: libraryRoot.allPlaylists.length
                    tileActive: libraryRoot.aioBrowseScreen === "playlist"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 5
                    onTapped: { libraryRoot.aioNavCursorIndex = 5; libraryRoot.aioEnterBrowse("playlist") }
                }
                AioNavTile {
                    width: parent.cellW
                    navIndex: 6
                    tileIcon: "⏱"; tileLabel: "HISTORY"
                    tileActive: libraryRoot.aioBrowseScreen === "history"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 6
                    onTapped: { libraryRoot.aioNavCursorIndex = 6; libraryRoot.aioEnterBrowse("history") }
                }
                AioNavTile {
                    width: parent.cellW
                    navIndex: 7
                    tileIcon: "◈"; tileLabel: "MATCHING"
                    tileAccent: libraryRoot.accentGreen
                    tileActive: libraryRoot.aioBrowseScreen === "matching"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 7
                    onTapped: { libraryRoot.aioNavCursorIndex = 7; libraryRoot.aioEnterBrowse("matching") }
                }
                AioNavTile {
                    width: parent.cellW
                    navIndex: 8
                    tileIcon: "▤"; tileLabel: "FOLDER"
                    tileActive: libraryRoot.aioBrowseScreen === "folder"
                    tileCursor: libraryRoot.aioCursorNavActive && libraryRoot.aioNavCursorIndex === 8
                    onTapped: { libraryRoot.aioNavCursorIndex = 8; libraryRoot.aioEnterBrowse("folder") }
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
                    anchors.fill: parent
                    anchors.leftMargin: libraryRoot.touchMode ? 10 : 14
                    anchors.rightMargin: libraryRoot.touchMode ? 10 : 12
                    spacing: libraryRoot.touchMode ? 10 : 8

                    // ── Browse back (drill-down) ───────────────────────────
                    Rectangle {
                        Layout.preferredWidth: libraryRoot.touchMode
                                               && (libraryRoot.aioBrowseDrilled
                                                   || libraryRoot.aioFocusZone !== "nav") ? 40 : 0
                        Layout.preferredHeight: libraryRoot.touchMode ? 40 : 0
                        Layout.alignment: Qt.AlignVCenter
                        visible: libraryRoot.touchMode
                                 && (libraryRoot.aioBrowseDrilled
                                     || libraryRoot.aioFocusZone !== "nav")
                        radius: 4
                        color: aioBackMa.pressed ? "#1a3048" : "#132840"
                        border.color: "#1e4070"
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "◀"
                            color: libraryRoot.accentBlueLt
                            font.pixelSize: window.sp(14)
                            font.bold: true
                        }
                        MouseArea {
                            id: aioBackMa
                            anchors.fill: parent
                            onClicked: libraryRoot.aioBrowseCursorLeft()
                        }
                    }

                    // ── Tab icon + title ───────────────────────────────────
                    Row {
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 7

                        Rectangle {
                            width: libraryRoot.touchMode ? 28 : 22
                            height: libraryRoot.touchMode ? 28 : 22
                            radius: 4
                            color: "#132840"
                            border.color: "#1e4070"; border.width: 1
                            anchors.verticalCenter: parent.verticalCenter

                            Text {
                                anchors.centerIn: parent
                                text: libraryRoot.activeTab === "library"    ? "♫"
                                    : libraryRoot.activeTab === "playlist"   ? "☰"
                                    : libraryRoot.activeTab === "favorites"  ? "★"
                                    : libraryRoot.activeTab === "history"    ? "⏱"
                                    : libraryRoot.activeTab === "crate"      ? "⊞"
                                    : libraryRoot.activeTab === "queue"      ? "►"
                                    : libraryRoot.activeTab === "smartcoll"  ? "◈"
                                    : libraryRoot.activeTab === "files"      ? "≡"
                                    : libraryRoot.activeTab === "streaming"  ? "◎"
                                    : "⊕"
                                color: libraryRoot.accentBlueLt
                                font.pixelSize: window.sp(libraryRoot.touchMode ? 12 : 10)
                            }
                        }

                        Text {
                            text: libraryRoot.touchMode
                                  ? libraryRoot.aioScreenTitle()
                                  : (libraryRoot.activeTab === "library"    ? "Library"
                                    : libraryRoot.activeTab === "playlist"   ? libraryRoot.currentPlaylistName
                                    : libraryRoot.activeTab === "favorites"  ? "Favorites"
                                    : libraryRoot.activeTab === "history"    ? "History"
                                    : libraryRoot.activeTab === "crate"      ? "Prepare Crate"
                                    : libraryRoot.activeTab === "queue"      ? "Queue"
                                    : libraryRoot.activeTab === "smartcoll"  ? libraryRoot.currentSmartCollectionName
                                    : libraryRoot.activeTab === "files"      ? "File Browser"
                                    : libraryRoot.activeTab === "streaming"  ? "Streaming"
                                    : "USB")
                            color: libraryRoot.textPrimary
                            font.pixelSize: window.sp(libraryRoot.touchMode ? 13 : 12)
                            font.weight: Font.Medium
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
                        Layout.preferredWidth: libraryRoot.touchMode ? 148 : 176
                        Layout.preferredHeight: libraryRoot.touchMode ? 40 : 26
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4; clip: true
                        color: analyzeHover.containsMouse ? "#1a1a1a" : "#111111"
                        border.color: libraryAnalyzer && libraryAnalyzer.running
                                      ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        visible: libraryRoot.activeTab === "library" || libraryRoot.activeTab === "playlist"

                        Behavior on border.color { ColorAnimation { duration: 200 } }

                        // Determinate progress fill for the complete queue,
                        // including the live percentage of the current track.
                        Rectangle {
                            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: parent.width * (libraryAnalyzer ? libraryAnalyzer.progress : 0)
                            color: "#0d2840"
                            visible: libraryAnalyzer && libraryAnalyzer.running
                            Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 3
                            color: "#263442"
                            visible: libraryAnalyzer && libraryAnalyzer.running

                            Rectangle {
                                height: parent.height
                                width: parent.width * (libraryAnalyzer ? libraryAnalyzer.progress : 0)
                                color: libraryRoot.accentBlueLt
                                Behavior on width { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
                            }
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
                                text: Math.round((libraryAnalyzer ? libraryAnalyzer.progress : 0) * 100)
                                      + "%  " + Math.min(libraryAnalyzer ? libraryAnalyzer.total : 0,
                                                           (libraryAnalyzer ? libraryAnalyzer.completed : 0) + 1)
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
                        Layout.preferredWidth: libraryRoot.touchMode ? 72 : 54
                        Layout.preferredHeight: libraryRoot.touchMode ? 40 : 26
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4
                        color: "#111111"
                        border.color: "#2a2a2a"; border.width: 1
                        clip: true
                        visible: !libraryRoot.touchMode

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
                        Layout.preferredWidth: libraryRoot.touchMode
                                                   ? Math.min(360, Math.max(200, parent.width * 0.45))
                                                   : Math.min(260, Math.max(150, parent.width * 0.28))
                        Layout.preferredHeight: libraryRoot.touchMode ? 40 : 26
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

                    // ── Filter button (library tab) ────────────────────────
                    Rectangle {
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4
                        color: filterBtnMa.containsMouse || libraryRoot.filterPanelOpen ? "#1a2a3a" : "#111111"
                        border.color: libraryRoot.filtersActive ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        visible: libraryRoot.activeTab === "library"

                        Text {
                            anchors.centerIn: parent
                            text: "⧩"
                            color: libraryRoot.filtersActive ? libraryRoot.accentBlueLt : libraryRoot.textSecond
                            font.pixelSize: window.sp(12)
                        }
                        MouseArea {
                            id: filterBtnMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: libraryRoot.filterPanelOpen = !libraryRoot.filterPanelOpen
                        }
                    }

                    // ── Smart collection edit (smartcoll tab) ─────────────
                    Rectangle {
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4
                        color: scEditMa.containsMouse ? "#1a2a3a" : "#111111"
                        border.color: "#2a2a2a"; border.width: 1
                        visible: libraryRoot.activeTab === "smartcoll"
                                 && libraryRoot.currentSmartCollectionId !== ""

                        Text {
                            anchors.centerIn: parent; text: "✎"
                            color: libraryRoot.textSecond; font.pixelSize: window.sp(11)
                        }
                        MouseArea {
                            id: scEditMa; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                var sc = libraryRoot.smartCollections.find(function(s) {
                                    return s.id === libraryRoot.currentSmartCollectionId
                                })
                                if (sc) libraryRoot.openSmartCollEditor(sc)
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4
                        color: scRefreshMa.containsMouse ? "#1a2a3a" : "#111111"
                        border.color: "#2a2a2a"; border.width: 1
                        visible: libraryRoot.activeTab === "smartcoll"
                                 && libraryRoot.currentSmartCollectionId !== ""

                        Text {
                            anchors.centerIn: parent; text: "↻"
                            color: libraryRoot.textSecond; font.pixelSize: window.sp(13)
                        }
                        MouseArea {
                            id: scRefreshMa; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: libraryRoot.loadSmartCollectionTracks(libraryRoot.currentSmartCollectionId)
                        }
                    }
                }
            }

            // ── Filter panel (library) ─────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: libraryRoot.filterPanelOpen && libraryRoot.activeTab === "library" ? 34 : 0
                visible: libraryRoot.filterPanelOpen && libraryRoot.activeTab === "library"
                color: "#161616"
                clip: true

                Rectangle {
                    anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                    height: 1; color: libraryRoot.borderMain
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14; anchors.rightMargin: 12
                    spacing: 8

                    Text {
                        text: "BPM"
                        color: libraryRoot.textSecond
                        font.pixelSize: window.sp(10)
                    }

                    Rectangle {
                        Layout.preferredWidth: 52; Layout.preferredHeight: 24
                        radius: 3; color: "#0e0e0e"
                        border.color: filterBpmMinField.activeFocus ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        TextField {
                            id: filterBpmMinField
                            anchors.fill: parent
                            color: libraryRoot.textPrimary
                            font.pixelSize: window.sp(10)
                            placeholderText: "min"
                            horizontalAlignment: Text.AlignHCenter
                            background: Item {}
                            text: libraryModel && libraryModel.filterBpmMin > 0
                                  ? String(libraryModel.filterBpmMin) : ""
                            onEditingFinished: {
                                if (libraryModel)
                                    libraryModel.setFilterBpmMin(parseFloat(text) || 0)
                            }
                        }
                    }

                    Text { text: "–"; color: libraryRoot.textDim; font.pixelSize: window.sp(10) }

                    Rectangle {
                        Layout.preferredWidth: 52; Layout.preferredHeight: 24
                        radius: 3; color: "#0e0e0e"
                        border.color: filterBpmMaxField.activeFocus ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        TextField {
                            id: filterBpmMaxField
                            anchors.fill: parent
                            color: libraryRoot.textPrimary
                            font.pixelSize: window.sp(10)
                            placeholderText: "max"
                            horizontalAlignment: Text.AlignHCenter
                            background: Item {}
                            text: libraryModel && libraryModel.filterBpmMax > 0
                                  ? String(libraryModel.filterBpmMax) : ""
                            onEditingFinished: {
                                if (libraryModel)
                                    libraryModel.setFilterBpmMax(parseFloat(text) || 0)
                            }
                        }
                    }

                    Text {
                        text: "Key"
                        color: libraryRoot.textSecond
                        font.pixelSize: window.sp(10)
                        Layout.leftMargin: 6
                    }

                    Rectangle {
                        Layout.preferredWidth: 56; Layout.preferredHeight: 24
                        radius: 3; color: "#0e0e0e"
                        border.color: filterKeyField.activeFocus ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        TextField {
                            id: filterKeyField
                            anchors.fill: parent
                            color: libraryRoot.accentKey
                            font.pixelSize: window.sp(10)
                            placeholderText: "e.g. Am"
                            leftPadding: 6
                            background: Item {}
                            text: libraryModel ? libraryModel.filterKey : ""
                            onEditingFinished: {
                                if (libraryModel) libraryModel.setFilterKey(text.trim())
                            }
                        }
                    }

                    Text {
                        text: "Genre"
                        color: libraryRoot.textSecond
                        font.pixelSize: window.sp(10)
                    }

                    Rectangle {
                        Layout.preferredWidth: 90; Layout.preferredHeight: 24
                        radius: 3; color: "#0e0e0e"
                        border.color: filterGenreField.activeFocus ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        TextField {
                            id: filterGenreField
                            anchors.fill: parent
                            color: libraryRoot.textPrimary
                            font.pixelSize: window.sp(10)
                            placeholderText: "genre"
                            leftPadding: 6
                            background: Item {}
                            text: libraryModel ? libraryModel.filterGenre : ""
                            onEditingFinished: {
                                if (libraryModel) libraryModel.setFilterGenre(text.trim())
                            }
                        }
                    }

                    Text {
                        text: "★ min"
                        color: libraryRoot.textSecond
                        font.pixelSize: window.sp(10)
                    }

                    Rectangle {
                        Layout.preferredWidth: 36; Layout.preferredHeight: 24
                        radius: 3; color: "#0e0e0e"
                        border.color: filterRatingField.activeFocus ? libraryRoot.accentBlue : "#2a2a2a"
                        border.width: 1
                        TextField {
                            id: filterRatingField
                            anchors.fill: parent
                            color: "#e8b84b"
                            font.pixelSize: window.sp(10)
                            placeholderText: "0"
                            horizontalAlignment: Text.AlignHCenter
                            background: Item {}
                            text: libraryModel && libraryModel.filterRatingMin > 0
                                  ? String(libraryModel.filterRatingMin) : ""
                            onEditingFinished: {
                                if (libraryModel)
                                    libraryModel.setFilterRatingMin(parseInt(text) || 0)
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        Layout.preferredWidth: 64; Layout.preferredHeight: 24
                        radius: 3
                        color: clearFiltersMa.containsMouse ? "#2a2a2a" : "#1c1c1c"
                        border.color: "#333"; border.width: 1
                        visible: libraryRoot.filtersActive

                        Text {
                            anchors.centerIn: parent
                            text: "Reset"
                            color: libraryRoot.textMeta
                            font.pixelSize: window.sp(10)
                        }
                        MouseArea {
                            id: clearFiltersMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (libraryModel) libraryModel.clearFilters()
                                filterBpmMinField.text = ""
                                filterBpmMaxField.text = ""
                                filterKeyField.text = ""
                                filterGenreField.text = ""
                                filterRatingField.text = ""
                            }
                        }
                    }
                }
            }

            // ── View area ──────────────────────────────────────────────────
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Focus indicator — top accent line when track list is focused
                Rectangle {
                    anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                    height: 2; color: libraryRoot.accentGreen
                    visible: libraryRoot.aioCursorTracksActive
                             && libraryRoot.focusedPanel === "tracks"
                    z: 200
                }

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

                    Row {
                        anchors.fill: parent
                        spacing: 0

                        AioBrowsePicker {
                            id: libBrowsePicker
                            width: libraryRoot.aioLibSplitBrowse && !libraryRoot.aioBrowseDrilled
                                   ? Math.round(libraryDbView.width * 0.34) : 0
                            height: parent.height
                            visible: width > 0
                            categoryLabel: libraryRoot.aioBrowsePickerTitle()
                            entries: libraryRoot.aioBrowsePickerItems()
                            focusKey: libraryRoot.aioBrowseFocusKey
                        }

                        Item {
                            id: libTrackPane
                            width: parent.width - libBrowsePicker.width
                            height: parent.height

                    // ── Column headers ─────────────────────────────────────
                    Rectangle {
                        id: libHeader
                        anchors.top: libTrackPane.top
                        anchors.left: libTrackPane.left
                        anchors.right: libTrackPane.right
                        height: libraryRoot.viewMode === "normal" ? 0 : libraryRoot.hdrH
                        visible: libraryRoot.viewMode === "compact"
                                && (!libraryRoot.aioLibSplitBrowse || libraryRoot.aioBrowseDrilled)
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
                                width: libraryRoot.colTitle(libTrackPane.width)
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
                                            var fw = libraryRoot.flexWidth(libTrackPane.width)
                                            if (fw <= 0) return
                                            var newFrac = startFrac + dx / fw
                                            libraryRoot.titleFraction = Math.max(0.2, Math.min(0.8, newFrac))
                                        }
                                    }
                                }
                            }

                            SortHeader { width: libraryRoot.colArtist(libTrackPane.width); height: parent.height; field: "artist"; label: "KÜNSTLER" }
                            SortHeader { width: libraryRoot.colTime;  height: parent.height; field: "time";    label: "ZEIT";    centerAlign: true }
                            SortHeader { width: libraryRoot.colBpm;   height: parent.height; field: "bpm";     label: "BPM";     centerAlign: true }
                            SortHeader { width: libraryRoot.colKey;   height: parent.height; field: "key";     label: "KEY";     centerAlign: true }
                            SortHeader { width: libraryRoot.colKbps;  height: parent.height; field: "kbps";    label: "KBPS";    centerAlign: true; isLast: true }
                        }
                    }

                    // ── Normal-view sort bar ───────────────────────────────
                    Rectangle {
                        id: libSortBar
                        anchors.top: libHeader.bottom
                        anchors.left: libTrackPane.left
                        anchors.right: libTrackPane.right
                        height: libraryRoot.viewMode === "normal" ? 30 : 0
                        visible: libraryRoot.viewMode === "normal"
                                || (libraryRoot.aioLibSplitBrowse && !libraryRoot.aioBrowseDrilled
                                    && libraryRoot.aioBrowseFocusKey.length > 0)
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

                    AioLoadBar {
                        id: libAioLoadBar
                        anchors.top: libSortBar.bottom
                        anchors.left: libTrackPane.left
                        anchors.right: libTrackPane.right
                    }

                    // ── Track list ─────────────────────────────────────────
                    ListView {
                        id: libTrackList
                        anchors.top: libAioLoadBar.bottom
                        anchors.left: libTrackPane.left
                        anchors.right: libTrackPane.right
                        anchors.bottom: libTrackPane.bottom
                        anchors.bottomMargin: libraryRoot.previewBarReserve
                        clip: true
                        model: libraryModel ? libraryModel : null
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        onMovementStarted: libraryRoot.closeAllSwipes()
                        onFlickStarted: libraryRoot.closeAllSwipes()

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
                            required property string trackColor
                            required property int    rating
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
                            rowColor: trackColor
                            rowRating: rating
                            width: ListView.view.width
                            viewWidth: ListView.view.width
                        }

                        // Empty state
                        Column {
                            anchors.centerIn: parent
                            visible: libTrackList.count === 0
                            spacing: 10
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "♫"; color: "#252525"; font.pixelSize: window.sp(42) }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: libraryRoot.aioLibSplitBrowse && !libraryRoot.aioBrowseDrilled
                                      && libraryRoot.aioBrowseFocusKey.length === 0
                                      ? "Artist wählen"
                                      : (libraryRoot.aioLibSplitBrowse && !libraryRoot.aioBrowseDrilled
                                         ? "Keine Treffer"
                                         : "Library is empty")
                                color: "#333333"; font.pixelSize: window.sp(12)
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                visible: !(libraryRoot.aioLibSplitBrowse && !libraryRoot.aioBrowseDrilled
                                           && libraryRoot.aioBrowseFocusKey.length === 0)
                                text: "Lade einen Track auf ein Deck, um ihn hinzuzufügen"
                                color: "#282828"; font.pixelSize: window.sp(10)
                            }
                        }
                    }
                        } // libTrackPane
                    } // Row
                }

                // ════════════════════════════════════════════════
                // B) PLAYLIST VIEW
                // ════════════════════════════════════════════════
                Rectangle {
                    id: playlistView
                    anchors.fill: parent
                    color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab === "playlist"

                    Row {
                        anchors.fill: parent
                        spacing: 0

                        AioBrowsePicker {
                            id: plBrowsePicker
                            width: libraryRoot.aioPlaylistSplitBrowse && !libraryRoot.aioBrowseDrilled
                                   ? Math.round(playlistView.width * 0.34) : 0
                            height: parent.height
                            visible: width > 0
                            categoryLabel: libraryRoot.aioBrowsePickerTitle()
                            entries: libraryRoot.aioBrowsePickerItems()
                            focusKey: libraryRoot.aioBrowseFocusKey
                        }

                        Item {
                            id: plTrackPane
                            width: parent.width - plBrowsePicker.width
                            height: parent.height

                    // ── Column headers ────────────────────────────────────
                    Rectangle {
                        id: plHeader
                        anchors.top: plTrackPane.top
                        anchors.left: plTrackPane.left
                        anchors.right: plTrackPane.right
                        height: libraryRoot.viewMode === "normal" ? 0 : libraryRoot.hdrH
                        visible: libraryRoot.viewMode === "compact"
                                && (!libraryRoot.aioPlaylistSplitBrowse || libraryRoot.aioBrowseDrilled)
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
                        anchors.top: plHeader.bottom
                        anchors.left: plTrackPane.left
                        anchors.right: plTrackPane.right
                        height: libraryRoot.viewMode === "normal" ? 30 : 0
                        visible: libraryRoot.viewMode === "normal"
                                || (libraryRoot.aioPlaylistSplitBrowse && !libraryRoot.aioBrowseDrilled
                                    && libraryRoot.aioBrowseFocusKey.length > 0)
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

                    AioLoadBar {
                        id: plAioLoadBar
                        anchors.top: plSortBar.bottom
                        anchors.left: plTrackPane.left
                        anchors.right: plTrackPane.right
                    }

                    ListView {
                        id: plTrackList
                        anchors.top: plAioLoadBar.bottom
                        anchors.left: plTrackPane.left
                        anchors.right: plTrackPane.right
                        anchors.bottom: plTrackPane.bottom
                        anchors.bottomMargin: libraryRoot.previewBarReserve
                        clip: true
                        model: libraryRoot.sortedPlaylistTracks
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        onMovementStarted: libraryRoot.closeAllSwipes()
                        onFlickStarted: libraryRoot.closeAllSwipes()

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
                            readonly property string trackColor: modelData.color      ?? ""
                            readonly property int    trackRating: modelData.rating    ?? 0
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
                            rowColor: trackColor
                            rowRating: trackRating
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
                            Text { anchors.horizontalCenter: parent.horizontalCenter
                                text: libraryRoot.aioPlaylistSplitBrowse && libraryRoot.currentPlaylistId === ""
                                      ? "Links Playlist wählen"
                                      : "Playlist ist leer"
                                color: "#333333"; font.pixelSize: window.sp(12) }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: 'Rechtsklick auf einen Track → "Zu Playlist hinzufügen"'
                                color: "#282828"; font.pixelSize: window.sp(10)
                            }
                        }
                    }
                        } // plTrackPane
                    } // Row
                }

                // ════════════════════════════════════════════════
                // C) VARLIST VIEW  (Favorites / History / Crate / Queue / Smart Collection)
                // ════════════════════════════════════════════════
                Rectangle {
                    id: varlistView
                    anchors.fill: parent
                    color: libraryRoot.bgBase
                    visible: libraryRoot._varlistTabs.indexOf(libraryRoot.activeTab) >= 0

                    // ── Column header (compact mode) ───────────────────────
                    Rectangle {
                        id: varlistHeader
                        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                        height: libraryRoot.hdrH
                        color: libraryRoot.bgHeader
                        visible: libraryRoot.viewMode === "compact"
                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 1; color: libraryRoot.borderMain
                        }
                        Row {
                            anchors.fill: parent; anchors.leftMargin: 6
                            Item { width: libraryRoot.colStatus; height: parent.height
                                Text { anchors.centerIn: parent; text: "●"; color: libraryRoot.textDim; font.pixelSize: window.sp(8) } }
                            Text {
                                width: libraryRoot.colTitle(varlistView.width)
                                anchors.verticalCenter: parent.verticalCenter
                                text: libraryRoot.activeTab === "history" ? "TITLE" : "TITEL"; color: libraryRoot.textDim
                                font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 0.8
                            }
                            Text {
                                width: libraryRoot.colArtist(varlistView.width)
                                anchors.verticalCenter: parent.verticalCenter
                                text: "ARTIST"; color: libraryRoot.textDim
                                font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 0.8
                            }
                            Text { width: libraryRoot.colTime; anchors.verticalCenter: parent.verticalCenter
                                text: libraryRoot.activeTab === "history" ? "PLAYED" : "ZEIT"; color: libraryRoot.textDim; font.pixelSize: window.sp(9); font.bold: true; horizontalAlignment: Text.AlignHCenter }
                            Text { width: libraryRoot.colBpm; anchors.verticalCenter: parent.verticalCenter
                                text: "BPM"; color: libraryRoot.textDim; font.pixelSize: window.sp(9); font.bold: true; horizontalAlignment: Text.AlignHCenter }
                            Text { width: libraryRoot.colKey; anchors.verticalCenter: parent.verticalCenter
                                text: "KEY"; color: libraryRoot.textDim; font.pixelSize: window.sp(9); font.bold: true; horizontalAlignment: Text.AlignHCenter }
                        }
                    }

                    // ── Action bar (period selector / action buttons) ───────
                    Rectangle {
                        id: varlistActionBar
                        anchors.top: varlistHeader.visible ? varlistHeader.bottom : parent.top
                        anchors.left: parent.left; anchors.right: parent.right
                        height: 30
                        color: "#121212"
                        visible: libraryRoot.activeTab === "history" ||
                                 libraryRoot.activeTab === "crate"   ||
                                 libraryRoot.activeTab === "queue"   ||
                                 libraryRoot.activeTab === "favorites"
                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 1; color: libraryRoot.borderMain
                        }

                        // ── History: period tabs ────────────────────────────
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 10
                            spacing: 4
                            visible: libraryRoot.activeTab === "history"

                            Repeater {
                                model: [
                                    { key: "today", label: "Today" },
                                    { key: "week",  label: "Week" },
                                    { key: "month", label: "Month" },
                                    { key: "all",   label: "All"  }
                                ]
                                Rectangle {
                                    required property var modelData
                                    width: periodLbl.implicitWidth + 16; height: 20; radius: 3
                                    color: libraryRoot.historyPeriod === modelData.key ? libraryRoot.accentBlue : "#1a1a1a"
                                    border.color: libraryRoot.historyPeriod === modelData.key ? libraryRoot.accentBlue : "#2a2a2a"
                                    border.width: 1
                                    Behavior on color { ColorAnimation { duration: 100 } }
                                    Text {
                                        id: periodLbl
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: libraryRoot.historyPeriod === modelData.key ? "#fff" : libraryRoot.textDim
                                        font.pixelSize: window.sp(10)
                                    }
                                    MouseArea {
                                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                        onClicked: libraryRoot.loadHistory(modelData.key)
                                    }
                                }
                            }
                        }

                        // ── Count label ─────────────────────────────────────
                        Text {
                            anchors.right: varlistActionsRight.left; anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: libraryRoot.currentListTracks.length
                                  + (libraryRoot.activeTab === "history"
                                     ? (libraryRoot.currentListTracks.length === 1 ? " play" : " plays")
                                     : (libraryRoot.currentListTracks.length === 1 ? " track" : " tracks"))
                            color: "#383838"; font.pixelSize: window.sp(9)
                        }

                        // ── Right-side action buttons ───────────────────────
                        Row {
                            id: varlistActionsRight
                            anchors.right: parent.right; anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6

                            // Save Crate as Playlist
                            Rectangle {
                                width: implicitWidth + 16; height: 20; radius: 3
                                implicitWidth: saveCrateLbl.implicitWidth
                                color: saveCrateMa.containsMouse ? "#1a3050" : "transparent"
                                border.color: saveCrateMa.containsMouse ? libraryRoot.accentBlue : "#2a2a2a"; border.width: 1
                                visible: libraryRoot.activeTab === "crate" && libraryRoot.prepareCrateTracks.length > 0
                                Text { id: saveCrateLbl; anchors.centerIn: parent
                                    text: "→ Playlist"; color: libraryRoot.accentBlue; font.pixelSize: window.sp(9) }
                                MouseArea { id: saveCrateMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: saveCrateDialog.open() }
                            }

                            // Clear Crate
                            Rectangle {
                                width: implicitWidth + 16; height: 20; radius: 3
                                implicitWidth: clearCrateLbl.implicitWidth
                                color: clearCrateMa.containsMouse ? "#3a1a1a" : "transparent"
                                border.color: clearCrateMa.containsMouse ? "#e06060" : "#2a2a2a"; border.width: 1
                                visible: libraryRoot.activeTab === "crate" && libraryRoot.prepareCrateTracks.length > 0
                                Text { id: clearCrateLbl; anchors.centerIn: parent
                                    text: "✕ Leeren"; color: "#e06060"; font.pixelSize: window.sp(9) }
                                MouseArea { id: clearCrateMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: { if (libraryDb) { libraryDb.clearPrepareCrate(); libraryRoot.loadCrate() } } }
                            }

                            // Clear Queue
                            Rectangle {
                                width: implicitWidth + 16; height: 20; radius: 3
                                implicitWidth: clearQueueLbl.implicitWidth
                                color: clearQueueMa.containsMouse ? "#3a1a1a" : "transparent"
                                border.color: clearQueueMa.containsMouse ? "#e06060" : "#2a2a2a"; border.width: 1
                                visible: libraryRoot.activeTab === "queue" && libraryRoot.queueTracks.length > 0
                                Text { id: clearQueueLbl; anchors.centerIn: parent
                                    text: "✕ Leeren"; color: "#e06060"; font.pixelSize: window.sp(9) }
                                MouseArea { id: clearQueueMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: { if (libraryDb) { libraryDb.clearQueue(); libraryRoot.loadQueue() } } }
                            }
                        }
                    }

                    AioLoadBar {
                        id: varAioLoadBar
                        anchors.top: varlistActionBar.visible ? varlistActionBar.bottom
                                   : varlistHeader.visible ? varlistHeader.bottom : parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                    }

                    ListView {
                        id: varlistTrackList
                        anchors.top: varAioLoadBar.bottom
                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                        anchors.bottomMargin: libraryRoot.previewBarReserve
                        clip: true
                        model: libraryRoot.currentListTracks
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        onMovementStarted: libraryRoot.closeAllSwipes()
                        onFlickStarted: libraryRoot.closeAllSwipes()

                        delegate: TrackRow {
                            required property var  modelData
                            required property int  index
                            rowIndex:       index
                            rowTrackId:     modelData.trackId  || ""
                            rowTitle:       modelData.title    || ""
                            rowArtist:      modelData.artist   || ""
                            rowDurationSec: modelData.durationSec || 0
                            rowBpm:         modelData.bpm      || 0
                            rowKey:         modelData.key      || ""
                            rowBitrateKbps: modelData.bitrateKbps || 0
                            rowIsAnalyzed:  modelData.isAnalyzed  || false
                            rowFilePath:    modelData.filePath || ""
                            rowColor:       modelData.color    || ""
                            rowRating:      modelData.rating   || 0
                            rowIsHistory:   libraryRoot.activeTab === "history"
                            rowPlayedAt:    modelData.playedAt || modelData.lastPlayed || 0
                            rowPlayEventIndex: modelData.playEventIndex || 0
                            rowTrackPlayCount: modelData.playCountAtTrack || modelData.playCount || 0
                            rowSourceTab:   libraryRoot.activeTab
                            width: ListView.view.width
                            viewWidth: ListView.view.width
                        }

                        Column {
                            anchors.centerIn: parent
                            visible: varlistTrackList.count === 0
                            spacing: 10
                            Text { anchors.horizontalCenter: parent.horizontalCenter
                                text: libraryRoot.activeTab === "favorites" ? "★"
                                    : libraryRoot.activeTab === "history"   ? "⏱"
                                    : libraryRoot.activeTab === "crate"     ? "⊞"
                                    : libraryRoot.activeTab === "queue"     ? "►" : "◈"
                                color: "#252525"; font.pixelSize: window.sp(42) }
                            Text { anchors.horizontalCenter: parent.horizontalCenter
                                text: libraryRoot.activeTab === "favorites" ? "No favorites yet"
                                    : libraryRoot.activeTab === "history"   ? "No play history"
                                    : libraryRoot.activeTab === "crate"     ? "Prepare crate is empty"
                                    : libraryRoot.activeTab === "queue"     ? "Queue is empty"
                                    : "No tracks match"
                                color: "#333333"; font.pixelSize: window.sp(12) }
                        }
                    }
                }

                // ════════════════════════════════════════════════
                // D) FILE BROWSER
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
                // D) DEVICE LIBRARY (read-only Rekordbox / generic USB)
                // ════════════════════════════════════════════════
                Rectangle {
                    id: usbDeviceView
                    anchors.fill: parent
                    color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab === "usb"

                    onVisibleChanged: {
                        if (visible && deviceLibraryManager)
                            deviceLibraryManager.setFilterText(libraryRoot.searchText)
                    }
                    Connections {
                        target: libraryRoot
                        function onSearchTextChanged() {
                            if (usbDeviceView.visible && deviceLibraryManager)
                                deviceLibraryManager.setFilterText(libraryRoot.searchText)
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.preferredWidth: 255
                            Layout.fillHeight: true
                            color: libraryRoot.bgSidebar
                            border.color: libraryRoot.borderMain

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 0

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: libraryRoot.hdrH
                                    color: libraryRoot.bgHeader
                                    Text {
                                        anchors.left: parent.left; anchors.leftMargin: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "DEVICES"
                                        color: libraryRoot.textSecond
                                        font.pixelSize: window.sp(10); font.bold: true
                                    }
                                    MouseArea {
                                        anchors.right: parent.right; anchors.rightMargin: 5
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 30; height: 24; cursorShape: Qt.PointingHandCursor
                                        onClicked: if (deviceLibraryManager) deviceLibraryManager.rescanNow()
                                        Text { anchors.centerIn: parent; text: "↻"; color: libraryRoot.textMeta; font.pixelSize: window.sp(13) }
                                    }
                                }

                                ListView {
                                    id: usbDevices
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Math.min(180, Math.max(54, count * 54))
                                    clip: true
                                    model: deviceLibraryManager ? deviceLibraryManager.devices : []
                                    delegate: Rectangle {
                                        required property var modelData
                                        required property int index
                                        width: ListView.view.width; height: 54
                                        color: deviceLibraryManager
                                               && deviceLibraryManager.selectedDeviceId === modelData.id
                                               ? libraryRoot.sidebarSel
                                               : (usbDeviceMouse.containsMouse ? libraryRoot.bgSidebarHv : "transparent")
                                        Rectangle {
                                            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                            width: 3; color: libraryRoot.accentBlue
                                            visible: deviceLibraryManager
                                                     && deviceLibraryManager.selectedDeviceId === modelData.id
                                        }
                                        Column {
                                            anchors.left: parent.left; anchors.leftMargin: 13
                                            anchors.right: parent.right; anchors.rightMargin: 8
                                            anchors.verticalCenter: parent.verticalCenter; spacing: 3
                                            Text { text: modelData.name; color: libraryRoot.textPrimary; font.pixelSize: window.sp(11); elide: Text.ElideRight; width: parent.width }
                                            Row {
                                                spacing: 7
                                                Rectangle {
                                                    width: usbBadge.implicitWidth + 10; height: 15; radius: 3
                                                    color: modelData.badge === "REKORDBOX" ? libraryRoot.bgRowActive : libraryRoot.bgRowEven
                                                    border.color: modelData.badge === "REKORDBOX" ? libraryRoot.accentBlue : libraryRoot.borderHigh
                                                    Text { id: usbBadge; anchors.centerIn: parent; text: modelData.badge; color: modelData.badge === "REKORDBOX" ? libraryRoot.accentBlueLt : libraryRoot.textMeta; font.pixelSize: window.sp(8); font.bold: true }
                                                }
                                                Text { text: modelData.scanning ? "Scanning library…" : (modelData.trackCount > 0 ? modelData.trackCount + " tracks" : modelData.status); color: libraryRoot.textDim; font.pixelSize: window.sp(8); elide: Text.ElideRight; width: 112 }
                                            }
                                        }
                                        MouseArea {
                                            id: usbDeviceMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                libraryRoot.usbPlaylistTreeExpanded = false
                                                deviceLibraryManager.chooseDevice(modelData.id)
                                            }
                                        }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: libraryRoot.borderMain }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 38
                                    visible: deviceLibraryManager && deviceLibraryManager.selectedDeviceId !== ""
                                    color: deviceLibraryManager && deviceLibraryManager.selectedViewName === "All Tracks"
                                           ? libraryRoot.sidebarSel
                                           : (usbAllTracksMouse.containsMouse ? libraryRoot.bgSidebarHv : "transparent")
                                    Rectangle {
                                        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                        width: 3; color: libraryRoot.accentBlue
                                        visible: deviceLibraryManager && deviceLibraryManager.selectedViewName === "All Tracks"
                                    }
                                    Text {
                                        anchors.left: parent.left; anchors.leftMargin: 16
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "♫  All Tracks"
                                        color: libraryRoot.textSecond
                                        font.pixelSize: window.sp(10)
                                        font.bold: true
                                    }
                                    MouseArea {
                                        id: usbAllTracksMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            libraryRoot.usbPlaylistTreeExpanded = false
                                            deviceLibraryManager.chooseTracks()
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 38
                                    visible: deviceLibraryManager && deviceLibraryManager.selectedDeviceId !== ""
                                    color: deviceLibraryManager && deviceLibraryManager.selectedViewName === "Playlists"
                                           ? libraryRoot.sidebarSel
                                           : (usbPlaylistRootMouse.containsMouse ? libraryRoot.bgSidebarHv : "transparent")
                                    Rectangle {
                                        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                        width: 3; color: libraryRoot.accentBlue
                                        visible: deviceLibraryManager && deviceLibraryManager.selectedViewName === "Playlists"
                                    }
                                    Text {
                                        anchors.left: parent.left; anchors.leftMargin: 16
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: (libraryRoot.usbPlaylistTreeExpanded ? "▾  " : "▸  ") + "Playlists"
                                        color: libraryRoot.textSecond
                                        font.pixelSize: window.sp(10)
                                        font.bold: true
                                    }
                                    MouseArea {
                                        id: usbPlaylistRootMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            libraryRoot.usbPlaylistTreeExpanded = !libraryRoot.usbPlaylistTreeExpanded
                                            if (libraryRoot.usbPlaylistTreeExpanded)
                                                deviceLibraryManager.choosePlaylists()
                                            else
                                                deviceLibraryManager.chooseDevice(deviceLibraryManager.selectedDeviceId)
                                        }
                                    }
                                }

                                ListView {
                                    id: usbPlaylists
                                    Layout.fillWidth: true; Layout.fillHeight: true
                                    visible: libraryRoot.usbPlaylistTreeExpanded
                                    clip: true
                                    model: deviceLibraryManager ? deviceLibraryManager.currentPlaylists : []
                                    delegate: Rectangle {
                                        required property var modelData
                                        width: ListView.view.width; height: 34
                                        color: !modelData.folder && usbPlaylistMouse.containsMouse ? libraryRoot.bgRowHover : "transparent"
                                        Text {
                                            anchors.left: parent.left; anchors.leftMargin: 12 + modelData.depth * 14
                                            anchors.right: usbPlaylistCount.left; anchors.verticalCenter: parent.verticalCenter
                                            text: (modelData.folder ? "▾  " : "♫  ") + modelData.name
                                            color: modelData.folder ? libraryRoot.textNav : libraryRoot.textSecond
                                            font.pixelSize: window.sp(10); font.bold: modelData.folder; elide: Text.ElideRight
                                        }
                                        Text { id: usbPlaylistCount; anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: modelData.folder ? "" : modelData.trackCount; color: libraryRoot.textDim; font.pixelSize: window.sp(9) }
                                        MouseArea { id: usbPlaylistMouse; anchors.fill: parent; enabled: !modelData.folder; hoverEnabled: true; cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor; onClicked: deviceLibraryManager.choosePlaylist(modelData.id) }
                                    }
                                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                }

                                Item {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    visible: !libraryRoot.usbPlaylistTreeExpanded
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: libraryRoot.hdrH
                                color: libraryRoot.bgHeader
                                Text { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: deviceLibraryManager ? deviceLibraryManager.selectedViewName : "Devices"; color: libraryRoot.textPrimary; font.pixelSize: window.sp(11); font.bold: true }
                                Text { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: deviceLibraryManager ? deviceLibraryManager.statusMessage : ""; color: libraryRoot.textDim; font.pixelSize: window.sp(9) }
                            }
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 30
                                color: libraryRoot.bgHeader
                                Row {
                                    anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; spacing: 5
                                    Text { text: "SORT"; color: libraryRoot.textDim; font.pixelSize: window.sp(9); font.bold: true }
                                    SortPill { label: "Titel"; ascending: deviceLibraryManager ? deviceLibraryManager.sortAscending : true; isActive: deviceLibraryManager && deviceLibraryManager.sortField === "title"; onTapped: deviceLibraryManager.toggleSort("title") }
                                    SortPill { label: "Künstler"; ascending: deviceLibraryManager ? deviceLibraryManager.sortAscending : true; isActive: deviceLibraryManager && deviceLibraryManager.sortField === "artist"; onTapped: deviceLibraryManager.toggleSort("artist") }
                                    SortPill { label: "Album"; ascending: deviceLibraryManager ? deviceLibraryManager.sortAscending : true; isActive: deviceLibraryManager && deviceLibraryManager.sortField === "album"; onTapped: deviceLibraryManager.toggleSort("album") }
                                    SortPill { label: "Genre"; ascending: deviceLibraryManager ? deviceLibraryManager.sortAscending : true; isActive: deviceLibraryManager && deviceLibraryManager.sortField === "genre"; onTapped: deviceLibraryManager.toggleSort("genre") }
                                    SortPill { label: "BPM"; ascending: deviceLibraryManager ? deviceLibraryManager.sortAscending : true; isActive: deviceLibraryManager && deviceLibraryManager.sortField === "bpm"; onTapped: deviceLibraryManager.toggleSort("bpm") }
                                    SortPill { label: "Key"; ascending: deviceLibraryManager ? deviceLibraryManager.sortAscending : true; isActive: deviceLibraryManager && deviceLibraryManager.sortField === "key"; onTapped: deviceLibraryManager.toggleSort("key") }
                                }
                            }
                            ListView {
                                id: usbTrackList
                                Layout.fillWidth: true; Layout.fillHeight: true
                                clip: true
                                model: deviceLibraryManager ? deviceLibraryManager.currentTracks : []
                                delegate: TrackRow {
                                    required property var modelData
                                    required property int index
                                    rowIndex: index
                                    rowTrackId: modelData.trackId || ""
                                    rowTitle: modelData.title || ""
                                    rowArtist: modelData.artist || ""
                                    rowAlbum: modelData.album || ""
                                    rowGenre: modelData.genre || ""
                                    rowDurationSec: Math.round(modelData.durationSec || 0)
                                    rowBpm: modelData.bpm || 0
                                    rowKey: modelData.key || ""
                                    rowBitrateKbps: modelData.bitrateKbps || 0
                                    rowIsAnalyzed: modelData.isAnalyzed || false
                                    rowFilePath: modelData.filePath || ""
                                    rowArtworkPath: modelData.artworkPath || ""
                                    rowColor: modelData.trackColor || ""
                                    rowRating: modelData.rating || 0
                                    rowSourceTab: "usb"
                                    width: ListView.view.width; viewWidth: ListView.view.width
                                }
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                Column {
                                    anchors.centerIn: parent; spacing: 8
                                    visible: usbTrackList.count === 0
                                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: deviceLibraryManager && deviceLibraryManager.busy ? "Scanning library…" : (deviceLibraryManager ? deviceLibraryManager.statusMessage : "No device"); color: libraryRoot.textDim; font.pixelSize: window.sp(12) }
                                    BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: deviceLibraryManager ? deviceLibraryManager.busy : false; visible: running; width: 28; height: 28 }
                                }
                            }
                        }
                    }
                }

                // ════════════════════════════════════════════════
                // E) PLACEHOLDER (Streaming)
                // ════════════════════════════════════════════════
                Rectangle {
                    anchors.fill: parent; color: libraryRoot.bgBase
                    visible: libraryRoot.activeTab !== "files" && libraryRoot.activeTab !== "library"
                             && libraryRoot.activeTab !== "playlist" && libraryRoot.activeTab !== "usb"
                             && libraryRoot._varlistTabs.indexOf(libraryRoot.activeTab) < 0

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
        readonly property bool externalTrack: libraryRoot.ctxTrackId.indexOf("rekordbox:") === 0
        background: Rectangle { implicitWidth: 260; color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }

        MenuItem {
            text: "Preview track"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: { libraryRoot.togglePreview(libraryRoot.ctxFilePath) }
        }
        MenuSeparator {
            contentItem: Rectangle { height: 1; color: "#2a2a2a" }
        }
        MenuItem {
            text: "Load to Deck A"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: { libraryRoot.loadTrackToDeck("A", libraryRoot.ctxFilePath, libraryRoot.ctxTrackId) }
        }
        MenuItem {
            text: "Load to Deck B"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: { libraryRoot.loadTrackToDeck("B", libraryRoot.ctxFilePath, libraryRoot.ctxTrackId) }
        }
        MenuItem {
            text: "Load to Deck C"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: { libraryRoot.loadTrackToDeck("C", libraryRoot.ctxFilePath, libraryRoot.ctxTrackId) }
        }
        MenuItem {
            text: "Load to Deck D"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: { libraryRoot.loadTrackToDeck("D", libraryRoot.ctxFilePath, libraryRoot.ctxTrackId) }
        }
        MenuSeparator {
            contentItem: Rectangle { height: 1; color: "#2a2a2a" }
        }
        MenuItem {
            text: "Track analysieren / erneut analysieren"
            visible: !trackContextMenu.externalTrack
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
            visible: !trackContextMenu.externalTrack
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
        MenuItem {
            text: "Notes bearbeiten…"
            visible: !trackContextMenu.externalTrack
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: {
                libraryRoot.notesPanelTrackId = libraryRoot.ctxTrackId
                libraryRoot.notesPanelTitle   = libraryRoot.ctxTitle
                libraryRoot.notesPanelOpen    = true
            }
        }
        MenuSeparator { contentItem: Rectangle { height: 1; color: "#2a2a2a" } }

        // ── Rating submenu ──────────────────────────────────────────────
        Menu {
            title: "Bewertung setzen"
            visible: !trackContextMenu.externalTrack
            background: Rectangle { implicitWidth: 160; color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }
            contentItem: Text { leftPadding: 12; text: "Bewertung setzen ★"; color: "#dcdcdc"; font.pixelSize: window.sp(11) }

            Repeater {
                model: [
                    { stars: 0, label: "— Keine" },
                    { stars: 1, label: "★ 1" },
                    { stars: 2, label: "★★ 2" },
                    { stars: 3, label: "★★★ 3" },
                    { stars: 4, label: "★★★★ 4" },
                    { stars: 5, label: "★★★★★ 5" }
                ]
                MenuItem {
                    required property var modelData
                    text: modelData.label
                    contentItem: Text { text: parent.text; color: "#e8b84b"; font.pixelSize: window.sp(11); leftPadding: 12 }
                    background: Rectangle { color: parent.highlighted ? "#2a2a10" : "transparent" }
                    onTriggered: {
                        if (libraryDb && libraryRoot.ctxTrackId)
                            libraryDb.setTrackRating(libraryRoot.ctxTrackId, modelData.stars)
                    }
                }
            }
        }

        // ── Color picker submenu ────────────────────────────────────────
        Menu {
            title: "Farbe setzen"
            visible: !trackContextMenu.externalTrack
            background: Rectangle { implicitWidth: 160; color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }
            contentItem: Text { leftPadding: 12; text: "Farbe setzen ●"; color: "#dcdcdc"; font.pixelSize: window.sp(11) }

            Repeater {
                model: [
                    { hex: "",        label: "— Keine"   },
                    { hex: "#e84040", label: "● Rot"     },
                    { hex: "#e87830", label: "● Orange"  },
                    { hex: "#e8c030", label: "● Gelb"    },
                    { hex: "#40c040", label: "● Grün"    },
                    { hex: "#4090e8", label: "● Blau"    },
                    { hex: "#a040e8", label: "● Lila"    },
                    { hex: "#e040a0", label: "● Pink"    }
                ]
                MenuItem {
                    required property var modelData
                    text: modelData.label
                    contentItem: Text { text: parent.text; color: modelData.hex || "#888"; font.pixelSize: window.sp(11); leftPadding: 12 }
                    background: Rectangle { color: parent.highlighted ? "#1a1a2a" : "transparent" }
                    onTriggered: {
                        if (libraryDb && libraryRoot.ctxTrackId)
                            libraryDb.setTrackColor(libraryRoot.ctxTrackId, modelData.hex)
                    }
                }
            }
        }

        MenuSeparator { contentItem: Rectangle { height: 1; color: "#2a2a2a" } }

        // ── Favorites / Crate / Queue (tab-sensitive) ───────────────────
        MenuItem {
            id: ctxFavoriteItem
            visible: !trackContextMenu.externalTrack
            readonly property bool isFav: libraryDb ? libraryDb.isFavorite(libraryRoot.ctxTrackId) : false
            text: isFav ? "★ Aus Favoriten entfernen" : "★ Zu Favoriten"
            contentItem: Text { text: parent.text; color: "#e8b84b"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2a2a10" : "transparent" }
            onTriggered: {
                if (!libraryDb || !libraryRoot.ctxTrackId) return
                if (ctxFavoriteItem.isFav)
                    libraryDb.removeFromFavorites(libraryRoot.ctxTrackId)
                else
                    libraryDb.addToFavorites(libraryRoot.ctxTrackId)
                libraryRoot.loadFavorites()
            }
        }
        MenuItem {
            visible: !trackContextMenu.externalTrack
            text: libraryRoot.activeTab === "crate" ? "⊞ Aus Crate entfernen" : "⊞ Zu Prepare Crate"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: {
                if (!libraryDb || !libraryRoot.ctxTrackId) return
                if (libraryRoot.activeTab === "crate")
                    libraryDb.removeFromPrepareCrate(libraryRoot.ctxTrackId)
                else
                    libraryDb.addToPrepareCrate(libraryRoot.ctxTrackId)
                libraryRoot.loadCrate()
            }
        }
        MenuItem {
            visible: !trackContextMenu.externalTrack
            text: libraryRoot.activeTab === "queue" ? "► Aus Queue entfernen" : "► In Queue"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: {
                if (!libraryDb || !libraryRoot.ctxTrackId) return
                if (libraryRoot.activeTab === "queue")
                    libraryDb.dequeueTrack(libraryRoot.ctxTrackId)
                else
                    libraryDb.enqueueTrack(libraryRoot.ctxTrackId)
                libraryRoot.loadQueue()
            }
        }
        MenuSeparator { contentItem: Rectangle { height: 1; color: "#2a2a2a" } }
        MenuItem {
            text: "Remove from Library"
            visible: !trackContextMenu.externalTrack
            contentItem: Text { text: parent.text; color: "#e06060"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#3a1a1a" : "transparent" }
            onTriggered: {
                removeFromLibraryPopup.trackId    = libraryRoot.ctxTrackId
                removeFromLibraryPopup.trackTitle = libraryRoot.ctxTitle
                removeFromLibraryPopup.open()
            }
        }
    }

    // Smart collection context menu
    Menu {
        id: smartCollContextMenu
        property string targetId: ""
        property string targetName: ""
        property var targetData: null

        background: Rectangle { implicitWidth: 220; color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }

        MenuItem {
            text: "Bearbeiten…"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: libraryRoot.openSmartCollEditor(smartCollContextMenu.targetData)
        }
        MenuItem {
            text: "Aktualisieren"
            contentItem: Text { text: parent.text; color: "#dcdcdc"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#2d7dd2" : "transparent" }
            onTriggered: {
                if (libraryRoot.activeTab === "smartcoll"
                        && libraryRoot.currentSmartCollectionId === smartCollContextMenu.targetId)
                    libraryRoot.loadSmartCollectionTracks(smartCollContextMenu.targetId)
            }
        }
        MenuSeparator { contentItem: Rectangle { height: 1; color: "#2a2a2a" } }
        MenuItem {
            text: "Löschen"
            contentItem: Text { text: parent.text; color: "#e06060"; font.pixelSize: window.sp(11); leftPadding: 12 }
            background: Rectangle { color: parent.highlighted ? "#3a1a1a" : "transparent" }
            onTriggered: {
                if (libraryDb && smartCollContextMenu.targetId) {
                    libraryDb.deleteSmartCollection(smartCollContextMenu.targetId)
                    if (libraryRoot.currentSmartCollectionId === smartCollContextMenu.targetId) {
                        libraryRoot.currentSmartCollectionId = ""
                        libraryRoot.currentSmartCollectionName = ""
                        libraryRoot.smartCollectionTracks = []
                        libraryRoot.activeTab = "library"
                    }
                    libraryRoot.loadSmartCollections()
                }
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

    // Smart Collection creation / edit dialog
    Popup {
        id: createSmartCollDialog
        property string editId: ""  // empty = create, set = edit existing
        property var pendingSc: null

        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: 420
        height: scDialogContent.implicitHeight + 28
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2

        background: Rectangle { color: "#1a1a1a"; border.color: "#383838"; border.width: 1; radius: 4 }

        property var ruleModels: []

        function addRule() {
            ruleModels = ruleModels.concat([{ field: "bpm", op: "gte", value: "" }])
        }

        function removeRule(idx) {
            var a = ruleModels.slice()
            a.splice(idx, 1)
            ruleModels = a
        }

        function buildRulesJson() {
            var rules = []
            for (var i = 0; i < scRulesRepeater.count; i++) {
                var item = scRulesRepeater.itemAt(i)
                if (item) rules.push({ field: item.ruleField, op: item.ruleOp, value: item.ruleValue })
            }
            return JSON.stringify(rules)
        }

        function doSave() {
            var n = scNameField.text.trim()
            if (!n || !libraryDb) return
            var rj = buildRulesJson()
            if (editId)
                libraryDb.updateSmartCollection(editId, n, rj)
            else
                libraryDb.createSmartCollection(n, rj)
            libraryRoot.loadSmartCollections()
        }

        onOpened: {
            if (pendingSc) {
                scNameField.text = pendingSc.name || ""
                try {
                    var parsed = JSON.parse(pendingSc.rulesJson || "[]")
                    ruleModels = parsed.length > 0 ? parsed : [{ field: "bpm", op: "gte", value: "" }]
                } catch (e) {
                    ruleModels = [{ field: "bpm", op: "gte", value: "" }]
                }
                pendingSc = null
            } else {
                scNameField.text = ""
                ruleModels = [{ field: "bpm", op: "gte", value: "" }]
            }
            scNameField.forceActiveFocus()
        }
        onClosed: { editId = ""; ruleModels = []; pendingSc = null }

        Column {
            id: scDialogContent
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: parent.top; anchors.margins: 14
            spacing: 10

            Text {
                text: createSmartCollDialog.editId ? "Smart Collection bearbeiten" : "Neue Smart Collection"
                color: libraryRoot.textPrimary; font.pixelSize: window.sp(12); font.bold: true
            }

            // Name field
            Rectangle {
                width: parent.width; height: 26; radius: 2
                color: "#0e0e0e"
                border.color: scNameField.activeFocus ? libraryRoot.accentBlue : "#2c2c2c"; border.width: 1

                TextField {
                    id: scNameField
                    anchors.fill: parent
                    color: libraryRoot.textPrimary; font.pixelSize: window.sp(11)
                    placeholderText: "Name…"; leftPadding: 8; topPadding: 0; bottomPadding: 0
                    background: Item {}
                    Keys.onEscapePressed: createSmartCollDialog.close()
                }
            }

            // Rules
            Text {
                text: "REGELN"
                color: "#484848"; font.pixelSize: window.sp(9); font.bold: true; font.letterSpacing: 1.2
            }

            Column {
                width: parent.width; spacing: 4

                Repeater {
                    id: scRulesRepeater
                    model: createSmartCollDialog.ruleModels

                    Item {
                        id: ruleRow
                        width: scDialogContent.width; height: 26
                        required property var modelData
                        required property int index

                        property string ruleField: fieldBox.currentText
                        property string ruleOp:    opBox.currentText
                        property string ruleValue: ruleValField.text

                        Row {
                            anchors.fill: parent; spacing: 4

                            ComboBox {
                                id: fieldBox
                                width: 90; height: parent.height
                                model: ["bpm","key","genre","rating","energy","playCount","dateAdded","title","artist","album","isAnalyzed"]
                                currentIndex: model.indexOf(ruleRow.modelData.field) >= 0
                                             ? model.indexOf(ruleRow.modelData.field) : 0
                                background: Rectangle { radius: 2; color: "#0e0e0e"; border.color: "#2c2c2c"; border.width: 1 }
                                contentItem: Text { leftPadding: 6; text: fieldBox.currentText
                                    color: libraryRoot.textPrimary; font.pixelSize: window.sp(10)
                                    verticalAlignment: Text.AlignVCenter }
                                delegate: ItemDelegate {
                                    width: fieldBox.width
                                    contentItem: Text { text: modelData; color: libraryRoot.textPrimary
                                        font.pixelSize: window.sp(10); leftPadding: 6 }
                                    background: Rectangle { color: highlighted ? "#1d5d9e" : "#1e1e1e" }
                                }
                                popup: Popup {
                                    y: fieldBox.height + 2; width: fieldBox.width
                                    padding: 0
                                    background: Rectangle { color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }
                                    contentItem: ListView { implicitHeight: contentHeight; model: fieldBox.delegateModel; clip: true }
                                }
                            }

                            ComboBox {
                                id: opBox
                                width: 90; height: parent.height
                                model: ["gte","lte","gt","lt","eq","ne","contains","not_contains","is_empty","is_not_empty"]
                                currentIndex: model.indexOf(ruleRow.modelData.op) >= 0
                                             ? model.indexOf(ruleRow.modelData.op) : 0
                                background: Rectangle { radius: 2; color: "#0e0e0e"; border.color: "#2c2c2c"; border.width: 1 }
                                contentItem: Text { leftPadding: 6; text: opBox.currentText
                                    color: libraryRoot.textPrimary; font.pixelSize: window.sp(10)
                                    verticalAlignment: Text.AlignVCenter }
                                delegate: ItemDelegate {
                                    width: opBox.width
                                    contentItem: Text { text: modelData; color: libraryRoot.textPrimary
                                        font.pixelSize: window.sp(10); leftPadding: 6 }
                                    background: Rectangle { color: highlighted ? "#1d5d9e" : "#1e1e1e" }
                                }
                                popup: Popup {
                                    y: opBox.height + 2; width: opBox.width
                                    padding: 0
                                    background: Rectangle { color: "#1e1e1e"; border.color: "#333"; border.width: 1; radius: 2 }
                                    contentItem: ListView { implicitHeight: contentHeight; model: opBox.delegateModel; clip: true }
                                }
                            }

                            Rectangle {
                                width: parent.width - fieldBox.width - opBox.width - 28 - 12; height: parent.height
                                radius: 2; color: "#0e0e0e"
                                border.color: ruleValField.activeFocus ? libraryRoot.accentBlue : "#2c2c2c"; border.width: 1
                                visible: opBox.currentText !== "is_empty" && opBox.currentText !== "is_not_empty"

                                TextField {
                                    id: ruleValField
                                    anchors.fill: parent
                                    color: libraryRoot.textPrimary; font.pixelSize: window.sp(10)
                                    placeholderText: "Wert"; leftPadding: 6; topPadding: 0; bottomPadding: 0
                                    text: ruleRow.modelData.value || ""
                                    background: Item {}
                                }
                            }

                            Rectangle {
                                width: 24; height: parent.height; radius: 2
                                color: rmRuleMa.containsMouse ? "#3a1a1a" : "transparent"
                                border.color: rmRuleMa.containsMouse ? "#e06060" : "#2a2a2a"; border.width: 1
                                Text { anchors.centerIn: parent; text: "×"; color: "#e06060"; font.pixelSize: window.sp(12) }
                                MouseArea {
                                    id: rmRuleMa; anchors.fill: parent
                                    hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: createSmartCollDialog.removeRule(ruleRow.index)
                                }
                            }
                        }
                    }
                }

                // Add rule button
                Rectangle {
                    width: 90; height: 22; radius: 2
                    color: addRuleMa.containsMouse ? "#1a2a3a" : "transparent"
                    border.color: addRuleMa.containsMouse ? libraryRoot.accentBlue : "#2a2a2a"; border.width: 1

                    Text { anchors.centerIn: parent; text: "+ Regel"; color: libraryRoot.accentBlue; font.pixelSize: window.sp(10) }
                    MouseArea {
                        id: addRuleMa; anchors.fill: parent
                        hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: createSmartCollDialog.addRule()
                    }
                }
            }

            // Buttons
            Row {
                anchors.right: parent.right; spacing: 6; bottomPadding: 0

                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: cancelScMa.containsMouse ? "#2e2e2e" : "#242424"
                    border.color: "#333"; border.width: 1
                    Text { anchors.centerIn: parent; text: "Cancel"; color: "#888"; font.pixelSize: window.sp(10) }
                    MouseArea { id: cancelScMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: createSmartCollDialog.close() }
                }
                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: saveScMa.containsMouse ? libraryRoot.accentBlue : "#1d5d9e"
                    Text { anchors.centerIn: parent; text: "Speichern"; color: "#fff"; font.pixelSize: window.sp(10) }
                    MouseArea { id: saveScMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { createSmartCollDialog.doSave(); createSmartCollDialog.close() } }
                }
            }
        }
    }

    // Save Crate as Playlist dialog
    Popup {
        id: saveCrateDialog
        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: 270; height: 104
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2
        background: Rectangle { color: "#1e1e1e"; border.color: "#383838"; border.width: 1; radius: 3 }

        Column {
            anchors.fill: parent; anchors.margins: 14; spacing: 8

            Text { text: "Crate als Playlist speichern"
                color: libraryRoot.textPrimary; font.pixelSize: window.sp(12); font.bold: true }

            Rectangle {
                width: parent.width; height: 26; radius: 2
                color: "#141414"
                border.color: saveCrateNameField.activeFocus ? libraryRoot.accentBlue : "#2c2c2c"; border.width: 1

                TextField {
                    id: saveCrateNameField
                    anchors.fill: parent
                    color: libraryRoot.textPrimary; font.pixelSize: window.sp(11)
                    background: Item {}
                    leftPadding: 8; topPadding: 0; bottomPadding: 0
                    placeholderText: "Playlist-Name…"
                    Keys.onReturnPressed: { doSaveCrate(); saveCrateDialog.close() }
                    Keys.onEscapePressed: saveCrateDialog.close()
                }
            }

            Row {
                anchors.right: parent.right; spacing: 6

                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: cancelSaveCrateMa.containsMouse ? "#2e2e2e" : "#242424"
                    border.color: "#333"; border.width: 1
                    Text { anchors.centerIn: parent; text: "Cancel"; color: "#888"; font.pixelSize: window.sp(10) }
                    MouseArea { id: cancelSaveCrateMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: saveCrateDialog.close() }
                }
                Rectangle {
                    width: 64; height: 24; radius: 2
                    color: okSaveCrateMa.containsMouse ? libraryRoot.accentBlue : "#1d5d9e"
                    Text { anchors.centerIn: parent; text: "Speichern"; color: "#fff"; font.pixelSize: window.sp(10) }
                    MouseArea { id: okSaveCrateMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { doSaveCrate(); saveCrateDialog.close() } }
                }
            }
        }

        function doSaveCrate() {
            var n = saveCrateNameField.text.trim()
            if (!n || !libraryDb) return
            libraryDb.savePrepareCrateAsPlaylist(n)
        }

        onOpened: { saveCrateNameField.text = ""; saveCrateNameField.forceActiveFocus() }
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

    PreviewControlBar {
        id: globalPreviewBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        z: 280
    }

    // Track notes panel — slides up from the bottom of the content area
    Rectangle {
        id: notesPanel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: libraryRoot.previewBarReserve
        height: libraryRoot.notesPanelOpen ? 160 : 0
        clip: true
        color: "#161616"
        border.color: "#2a2a2a"; border.width: notesPanel.height > 0 ? 1 : 0
        z: 300

        Behavior on height { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        // Load notes when panel opens
        property string loadedNotes: ""
        onVisibleChanged: if (libraryRoot.notesPanelOpen) loadNotesForPanel()

        Connections {
            target: libraryRoot
            function onNotesPanelOpenChanged() {
                if (libraryRoot.notesPanelOpen) notesPanel.loadNotesForPanel()
            }
            function onNotesPanelTrackIdChanged() {
                if (libraryRoot.notesPanelOpen) notesPanel.loadNotesForPanel()
            }
        }

        function loadNotesForPanel() {
            if (!libraryDb || !libraryRoot.notesPanelTrackId) { loadedNotes = ""; return }
            var meta = libraryDb.getTrackMeta(libraryRoot.notesPanelTrackId)
            loadedNotes = meta ? (meta.notes || "") : ""
            notesEdit.text = loadedNotes
        }

        Column {
            anchors.fill: parent; anchors.margins: 10; spacing: 6
            visible: notesPanel.height > 20

            // Header row
            Item {
                width: parent.width
                height: 22

                Text {
                    text: "Notes — " + libraryRoot.notesPanelTitle
                    color: libraryRoot.textSecond; font.pixelSize: window.sp(10)
                    elide: Text.ElideRight
                    width: parent.width - 50
                    anchors.verticalCenter: parent.verticalCenter
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    Rectangle {
                        width: 52; height: 20; radius: 2
                        color: saveNotesMa.containsMouse ? "#1a3a1a" : "transparent"
                        border.color: saveNotesMa.containsMouse ? libraryRoot.accentGreen : "#2a2a2a"; border.width: 1
                        Text { anchors.centerIn: parent; text: "Speichern"; color: libraryRoot.accentGreen; font.pixelSize: window.sp(9) }
                        MouseArea {
                            id: saveNotesMa; anchors.fill: parent
                            hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (libraryDb && libraryRoot.notesPanelTrackId)
                                    libraryDb.setTrackNotes(libraryRoot.notesPanelTrackId, notesEdit.text)
                                libraryRoot.notesPanelOpen = false
                            }
                        }
                    }

                    Rectangle {
                        width: 20; height: 20; radius: 2
                        color: closeNotesMa.containsMouse ? "#2d2d2d" : "transparent"
                        border.color: "#2a2a2a"; border.width: 1
                        Text { anchors.centerIn: parent; text: "×"; color: "#888"; font.pixelSize: window.sp(12) }
                        MouseArea {
                            id: closeNotesMa; anchors.fill: parent
                            hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: libraryRoot.notesPanelOpen = false
                        }
                    }
                }
            }

            // Notes text area
            Rectangle {
                width: parent.width
                height: parent.height - 30
                color: "#0e0e0e"; radius: 2
                border.color: notesEdit.activeFocus ? libraryRoot.accentBlue : "#222"; border.width: 1

                TextEdit {
                    id: notesEdit
                    anchors.fill: parent
                    anchors.margins: 4
                    color: libraryRoot.textPrimary
                    font.pixelSize: window.sp(11)
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    Keys.onEscapePressed: libraryRoot.notesPanelOpen = false
                }
            }
        }
    }
}
