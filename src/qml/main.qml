import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    minimumWidth: 800
    minimumHeight: 600
    visible: true
    title: "BrockDJ"
    color: UiTheme.bgDeep
    font.hintingPreference: Font.PreferFullHinting
    property bool libraryExpanded: false
    property string linkedDeckName: ""
    property bool exitPromptVisible: false
    property bool exitShutdownInProgress: false
    property bool exitManualBackupRequested: false
    property bool allowDirectClose: false
    property bool exitCleanupTriggered: false
    property bool uncleanShutdownWarningVisible: false
    property bool startupLibraryReady: false
    property real exitProgress: 0.0
    readonly property color unifiedGray: UiTheme.bg1

    property bool showWaveforms: true
    property bool showDeckA: true
    property bool showDeckB: true
    property bool showMixer: true
    property bool showFxBar: true
    property bool showLibrary: true
    property bool showCrossfader: true
    // Desktop-only bridge while we develop the standalone/AIO surface.
    property bool showDevelopmentControls: true
    property bool fourDeckMode: false
    property bool allInOneMode: false
    property string activeMainTab: "performance"
    // Temporary quick-access overlay, controlled by the handle in TopHeader.
    // This intentionally is not persisted: it is a momentary touch affordance.
    property real topBarPullProgress: 0.0
    property bool _desktopShowMixer: true
    property bool _desktopShowFxBar: true
    property bool _uiStateRestoring: false

    readonly property bool allInOnePanelActive: window.allInOneMode && window.activeMainTab !== "performance"
    readonly property bool libraryPanelActive: window.allInOneMode && window.activeMainTab === "library"
    readonly property bool settingsPanelActive: window.allInOneMode && window.activeMainTab === "settings"
    readonly property bool effectiveLibraryVisible: window.allInOneMode ? window.libraryPanelActive : window.showLibrary
    readonly property bool aioTwoDeckWaveformSlots:
        window.allInOneMode && !window.fourDeckMode && !window.allInOnePanelActive

    function isDuplicatePlayingTrack(engine) {
        if (!engine || !engine.hasTrack || !engine.isPlaying || !engine.trackFilePath)
            return false
        const path = engine.trackFilePath
        const decks = []
        if (typeof deckA !== "undefined" && deckA) decks.push(deckA)
        if (typeof deckB !== "undefined" && deckB) decks.push(deckB)
        if (typeof deckC !== "undefined" && deckC) decks.push(deckC)
        if (typeof deckD !== "undefined" && deckD) decks.push(deckD)
        let count = 0
        for (let i = 0; i < decks.length; ++i) {
            const d = decks[i]
            if (d.hasTrack && d.isPlaying && d.trackFilePath === path)
                count++
        }
        return count >= 2
    }

    function requestAppClose() {
        if (exitShutdownInProgress)
            return
        exitManualBackupRequested = false
        exitPromptVisible = true
    }

    function setAllInOneMode(enabled) {
        if (enabled) {
            window._desktopShowMixer = window.showMixer
            window._desktopShowFxBar = window.showFxBar
            window.showMixer = false
            window.showFxBar = false
        } else {
            window.showMixer = window._desktopShowMixer
            window.showFxBar = window._desktopShowFxBar
        }
        allInOneMode = enabled
        libraryExpanded = false
        activeMainTab = "performance"
    }

    function toggleAllInOneLibrary() {
        if (!allInOneMode) {
            showLibrary = !showLibrary
            return
        }
        activeMainTab = libraryPanelActive ? "performance" : "library"
        libraryExpanded = false
    }

    function toggleAllInOneSettings() {
        if (!allInOneMode)
            return false
        activeMainTab = settingsPanelActive ? "performance" : "settings"
        libraryExpanded = false
        return true
    }

    // ── Persist / restore layout mode across launches ────────────────────────
    function _persistUiState() {
        if (_uiStateRestoring)
            return
        if (typeof settingsManager === "undefined" || !settingsManager)
            return
        var s = function(key, val) { settingsManager.setUiState(key, val ? "1" : "0") }
        s("allInOneMode", window.allInOneMode)
        s("fourDeckMode", window.fourDeckMode)
        s("showWaveforms", window.showWaveforms)
        s("showDeckA", window.showDeckA)
        s("showDeckB", window.showDeckB)
        // Persist the desktop intent for mixer/FX (stashed while AIO is active).
        s("showMixer", window.allInOneMode ? window._desktopShowMixer : window.showMixer)
        s("showFxBar", window.allInOneMode ? window._desktopShowFxBar : window.showFxBar)
        s("showLibrary", window.showLibrary)
        s("showCrossfader", window.showCrossfader)
        s("showDevelopmentControls", window.showDevelopmentControls)
        settingsManager.setUiState("activeMainTab", window.activeMainTab)
    }

    function _restoreUiState() {
        if (typeof settingsManager === "undefined" || !settingsManager)
            return
        _uiStateRestoring = true
        var b = function(key, def) { return settingsManager.getUiState(key, def ? "1" : "0") === "1" }
        window.showWaveforms  = b("showWaveforms", true)
        window.showDeckA      = b("showDeckA", true)
        window.showDeckB      = b("showDeckB", true)
        window.showLibrary    = b("showLibrary", true)
        window.showCrossfader = b("showCrossfader", true)
        window.showDevelopmentControls = b("showDevelopmentControls", true)
        window.fourDeckMode   = b("fourDeckMode", false)
        window.showMixer      = b("showMixer", true)
        window.showFxBar      = b("showFxBar", true)

        if (b("allInOneMode", false)) {
            // Stashes the desktop mixer/FX prefs set above, then enters AIO.
            window.setAllInOneMode(true)
            var tab = settingsManager.getUiState("activeMainTab", "performance")
            if (tab === "library" || tab === "settings" || tab === "performance")
                window.activeMainTab = tab
        }
        _uiStateRestoring = false
    }

    function _scheduleUiPersist() {
        if (!_uiStateRestoring)
            uiStatePersistTimer.restart()
    }

    Timer {
        id: uiStatePersistTimer
        interval: 200
        repeat: false
        onTriggered: window._persistUiState()
    }

    onAllInOneModeChanged:  _scheduleUiPersist()
    onFourDeckModeChanged:  _scheduleUiPersist()
    onShowWaveformsChanged: _scheduleUiPersist()
    onShowDeckAChanged:     _scheduleUiPersist()
    onShowDeckBChanged:     _scheduleUiPersist()
    onShowMixerChanged:     _scheduleUiPersist()
    onShowFxBarChanged:     _scheduleUiPersist()
    onShowLibraryChanged:   _scheduleUiPersist()
    onShowCrossfaderChanged:_scheduleUiPersist()
    onShowDevelopmentControlsChanged: _scheduleUiPersist()
    onActiveMainTabChanged: _scheduleUiPersist()

    function cancelAppClosePrompt() {
        if (exitShutdownInProgress)
            return
        exitManualBackupRequested = false
        exitPromptVisible = false
    }

    function confirmAppClose() {
        if (exitShutdownInProgress)
            return

        exitShutdownInProgress = true
        exitCleanupTriggered = false
        exitProgress = 0.0
        exitShutdownTimer.restart()
    }

    function finalizeAppClose() {
        allowDirectClose = true
        exitPromptVisible = false
        window.visible = false
        if (typeof appExit !== "undefined" && appExit)
            appExit.finalizeExit(window.exitManualBackupRequested)
        else
            Qt.quit()
    }

    onClosing: function(close) {
        if (allowDirectClose) {
            close.accepted = true
            return
        }

        close.accepted = false
        requestAppClose()
    }

    Timer {
        id: exitShutdownTimer
        interval: 70
        repeat: true
        running: false
        onTriggered: {
            if (!window.exitShutdownInProgress) {
                stop()
                return
            }

            window.exitProgress = Math.min(1.0, window.exitProgress + 0.09)

            if (!window.exitCleanupTriggered && window.exitProgress >= 0.25) {
                window.exitCleanupTriggered = true
                if (typeof settingsManager !== "undefined" && settingsManager && settingsManager.flushToDisk)
                    settingsManager.flushToDisk()
            }

            if (window.exitProgress >= 1.0) {
                stop()
                window.finalizeAppClose()
            }
        }
    }

    // Two stacked performance waveforms need enough vertical room for their
    // beat grid and fixed playhead; deck controls live below in a compact row.
    readonly property real baseWaveformHeight: 340
    readonly property real baseDeckMixerHeight: baseUiHeight - baseWaveformHeight

    function _isTextInputFocused() {
        var focused = activeFocusItem
        if (!focused)
            return false
        return (typeof focused.echoMode !== "undefined")
            || (typeof focused.inputMask === "string")
            || (typeof focused.cursorPosition === "number")
            || (typeof focused.inputMethodComposing === "boolean")
    }

    function _handleLinkEnabledChanged() {
        if (typeof linkManager === "undefined" || linkManager === null)
            return
        if (!linkManager.enabled)
            window.linkedDeckName = ""
    }

    function _refreshLibraryAfterStartup() {
        if (!startupLibraryReady || !performanceWorkspace.librarySection)
            return

        var librarySection = performanceWorkspace.librarySection
        librarySection.loadPlaylists()
        librarySection.loadAllTags()
        librarySection.loadSmartCollections()
        librarySection.loadFavorites()
        librarySection.loadCrate()
        librarySection.loadQueue()

        if (typeof libraryDb !== "undefined" && libraryDb) {
            var vm = libraryDb.getSetting("library_view_mode", librarySection.viewMode)
            if (vm === "compact" || vm === "normal")
                librarySection.viewMode = vm
        }

        if (typeof libraryDb !== "undefined" && libraryDb && typeof libraryModel !== "undefined" && libraryModel) {
            var sf = libraryDb.getSetting("allTracks_sf", "title")
            var sa = libraryDb.getSetting("allTracks_sa", "1") === "1"
            libraryModel.setSort(sf, sa)
        }

        librarySection.syncSidebarCursorToSelection()
        librarySection.ensureActiveTrackForCurrentTab()
    }

    onStartupLibraryReadyChanged: _refreshLibraryAfterStartup()

    Component.onCompleted: {
        window._restoreUiState()
        if (typeof linkManager !== "undefined" && linkManager !== null)
            linkManager.enabledChanged.connect(window._handleLinkEnabledChanged)
    }

    Component.onDestruction: {
        if (typeof linkManager !== "undefined" && linkManager !== null)
            linkManager.enabledChanged.disconnect(window._handleLinkEnabledChanged)
    }

    StartupOverlay {
        anchors.fill: parent
        appWindow: window
        mainLayout: mainLayout
        welcomeOverlay: welcomeOverlay
        uncleanShutdownWarning: statusOverlay.uncleanShutdownWarning
    }

    // ── Global font sizing ───────────────────────────────────────────────────
    readonly property real _refHeight: 800
    // Gently scale text with window height so the UI stays legible on big screens
    // and fits on small ~10" panels (e.g. 1280x800, 1024x600). 1.0 at the 800px
    // reference height; clamped so layouts never break at the extremes.
    readonly property real responsiveFontScale: {
        var hs = Math.max(360, height) / _refHeight
        return Math.max(0.84, Math.min(1.18, hs))
    }
    // True on small panels — used to tighten fixed-height bars for 10" screens.
    readonly property bool compactLayout: height < 720 || width < 1100
    // AIO reference sizes for layout DoD (see .cursor/memory/activeContext.md)
    readonly property bool aioPrimaryProfile: width <= 1280 && height <= 800
    readonly property bool aioCompactProfile: width <= 1024 && height <= 600

    function _dpr() {
        var dpr = Screen.devicePixelRatio
        if (!isFinite(dpr) || dpr <= 0)
            dpr = 1.0
        return dpr
    }

    function _snapScaleToPhysicalPixels(rawScale) {
        if (!isFinite(rawScale) || rawScale <= 0)
            rawScale = 1.0
        var dpr = _dpr()
        var scaledPhysicalWidth = Math.round(window.baseUiWidth * rawScale * dpr)
        if (!isFinite(scaledPhysicalWidth) || scaledPhysicalWidth <= 0)
            return 1.0
        return scaledPhysicalWidth / (window.baseUiWidth * dpr)
    }

    function _snapLengthToPhysicalPixels(length) {
        if (!isFinite(length) || length <= 0)
            return 0
        var dpr = _dpr()
        return Math.round(length * dpr) / dpr
    }

    function _scaledFontSize(basePx) {
        var scale = responsiveFontScale * (uiScaleController ? uiScaleController.scale : 1.0)
        var scaled = basePx * scale

        if (basePx <= 8)
            scaled *= 1.24
        else if (basePx <= 10)
            scaled *= 1.18
        else if (basePx <= 12)
            scaled *= 1.10

        var dpr = _dpr()
        var snapped = Math.round(scaled * dpr) / dpr
        return Math.max(1, snapped)
    }

    function sp(basePx) {
        return Math.round(_scaledFontSize(basePx))
    }

    function spViewport(basePx) {
        var logicalPx = _scaledFontSize(basePx)

        if (basePx <= 6)
            logicalPx += 1.4
        else if (basePx <= 8)
            logicalPx += 1.0
        else if (basePx <= 10)
            logicalPx += 0.6

        var dpr = _dpr()
        var snapped = Math.round(logicalPx * dpr) / dpr
        return Math.max(1, Math.round(snapped))
    }

    readonly property real waveformZoom: waveformZoomController ? waveformZoomController.zoom : 0.22

    UiShortcutManager {
        appWindow: window
    }

    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (window._isTextInputFocused())
                return
            if (window.exitPromptVisible)
                return
            if (window.allInOneMode)
                window.toggleAllInOneLibrary()
            else
                window.libraryExpanded = !window.libraryExpanded
        }
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (window._isTextInputFocused())
                return

            if (window.exitPromptVisible) {
                window.cancelAppClosePrompt()
                return
            }

            if (window.allInOnePanelActive) {
                window.activeMainTab = "performance"
                return
            }

            window.requestAppClose()
        }
    }

    Connections {
        target: (typeof linkManager !== "undefined" && linkManager !== null) ? linkManager : null
    }

    Connections {
        target: (typeof midiManager !== "undefined" && midiManager !== null) ? midiManager : null
        function onLibraryViewToggleRequested() {
            window.toggleAllInOneLibrary()
        }
    }

    // -------------------------------------------------------------------------
    // VIEWPORT SCALING
    // -------------------------------------------------------------------------
    readonly property real baseUiWidth: 1600
    readonly property real rawUiScale: (width / baseUiWidth)
                                       * (uiScaleController ? uiScaleController.scale : 1.0)
    readonly property real uiScale: _snapScaleToPhysicalPixels(rawUiScale)
    readonly property real scaledWaveformHeight:
        _snapLengthToPhysicalPixels(window.baseWaveformHeight * window.uiScale)
    readonly property int scaledDeckMixerHeight: Math.round(window.baseDeckMixerHeight * window.uiScale)
    // The quick-access tray floats above the workspace; it must never resize
    // or push the decks, waveform, or library below it.
    readonly property int topBarHeight: UiMetrics.toolbarHeight
    readonly property int fxBarHeight: UiMetrics.px(window.compactLayout ? 74 : 90)
    readonly property int crossfaderBarHeight: UiMetrics.px(window.compactLayout ? 30 : 36)
    readonly property int mixerBaseWidth: UiMetrics.mixerPreferredWidth

    // Keep the full 375 px deck surface below the taller waveform viewport.
    // baseDeckMixerHeight is derived from these two values.
    readonly property real baseUiHeight: baseWaveformHeight + 375 + 4
    // The primary display is the standalone/AIO surface.  Mixer, FX and
    // transport controls are intentionally kept out of it during development.
    readonly property bool primaryDeckRowVisible: !window.libraryExpanded
                                                   && !window.allInOnePanelActive
                                                   && (window.showDeckA || window.showDeckB)
    readonly property bool secondaryDeckRowVisible: window.fourDeckMode && !window.libraryExpanded && !window.allInOnePanelActive
    readonly property bool crossfaderVisible: false
    readonly property bool fxVisible: false
    readonly property real waveformMinimumHeight: window.scaledWaveformHeight
    readonly property int libraryReserveHeight: !window.effectiveLibraryVisible ? 0 : Math.round(180 * window.uiScale)
    readonly property int fixedPerformanceHeight:
        (window.primaryDeckRowVisible ? window.scaledDeckMixerHeight : 0)
        + (window.secondaryDeckRowVisible ? window.scaledDeckMixerHeight : 0)
        + (window.crossfaderVisible ? window.crossfaderBarHeight + 1 : 0)
        + (window.fxVisible ? window.fxBarHeight : 0)
    readonly property int hiddenPerformanceHeight:
        (!window.libraryExpanded && !window.allInOnePanelActive && !window.primaryDeckRowVisible ? window.scaledDeckMixerHeight : 0)
        + (!window.libraryExpanded && !window.allInOnePanelActive && window.fourDeckMode && !window.secondaryDeckRowVisible ? window.scaledDeckMixerHeight : 0)
        + (!window.libraryExpanded && !window.allInOnePanelActive && !window.crossfaderVisible ? window.crossfaderBarHeight + 1 : 0)
        + (!window.libraryExpanded && !window.allInOnePanelActive && !window.fxVisible ? window.fxBarHeight : 0)
    readonly property real waveformAvailableHeight: _snapLengthToPhysicalPixels(Math.max(
        0,
        height - window.topBarHeight - window.fixedPerformanceHeight - window.libraryReserveHeight - 6
    ))
    readonly property real adaptiveWaveformHeight: !window.showWaveforms ? 0
        : _snapLengthToPhysicalPixels(Math.max(
            0,
            !window.effectiveLibraryVisible
                ? window.waveformAvailableHeight
                : Math.min(
                    window.waveformAvailableHeight,
                    window.scaledWaveformHeight + Math.round(window.hiddenPerformanceHeight * 0.75)
                )
        ))

    // ─────────────────────────────────────────────────────────────────────────
    // MAIN LAYOUT – direct child, no async Loader wrapping
    // ─────────────────────────────────────────────────────────────────────────
    DevelopmentControlsWindow {
        id: developmentControlsWindow
        appWindow: window
    }

    ColumnLayout {
        id: mainLayout
        anchors.fill: parent
        spacing: 0
        visible: false

        TopHeader {
            id: topHeader
            Layout.fillWidth: true
            Layout.minimumHeight: window.topBarHeight
            Layout.preferredHeight: window.topBarHeight
            Layout.maximumHeight: window.topBarHeight
            z: 10
        }

        PerformanceWorkspace {
            id: performanceWorkspace
            Layout.fillWidth: true
            Layout.fillHeight: true
            appWindow: window
        }
    }

    StatusOverlay {
        id: statusOverlay
        anchors.fill: parent
        appWindow: window
    }

    WelcomeScreen {
        id: welcomeOverlay
        active: false
    }

    ExitOverlay {
        appWindow: window
    }
}
