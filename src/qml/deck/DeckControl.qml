import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import DJSoftware

Item {
    id: deck
    property string deckName: "A"
    property var engine: null
    // The performance surface deliberately exposes only hot cues.  The full
    // transport/control strip lives in the development-controls window.
    property bool developmentControls: false
    // Development window cards intentionally omit the duplicated display and
    // pad surface, leaving a dense transport/loop/tempo control strip.
    property bool controlsOnly: false
    property var hostWindow: null
    property var mixerControl: null
    property string channelId: ""
    property string _manualBpmInput: ""

    Layout.fillWidth: true
    Layout.fillHeight: true

    property bool dropHovered: false

    property bool   _hasTrack:      false
    property string _trackTitle:    "No Track Loaded"
    property string _trackArtist:   ""
    property string _trackAlbum:    ""
    property string _trackDuration: ""
    property string _trackKey:      ""
    property string _trackBpm:      ""
    property string _currentBpm:    ""
    readonly property bool linkAvailable: (typeof linkManager !== "undefined" && linkManager !== null && linkManager.enabled)
    readonly property bool linkMode: deck.hostWindow !== null && deck.hostWindow.linkedDeckName === deck.deckName
    readonly property bool sameTrackDoubleWarning: {
        if (!deck.engine || !deck.engine.hasTrack || !deck.engine.isPlaying || !deck.engine.trackFilePath)
            return false
        const path = deck.engine.trackFilePath
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
    readonly property int headerCellHeight: 20
    property double _linkSuppressPublishUntilMs: 0
    property double _linkFollowBlockedUntilMs: 0
    property double _lastLinkPublishMs: 0
    property double _lastLinkPublishBpm: 0

    // ── Theme ────────────────────────────────────────────────────────────
    readonly property color accent:       UiTheme.deckColor(deck.deckName)
    readonly property color accentGrn:      UiTheme.green
    readonly property color accentBlu:      UiTheme.blue
    // Design-space layout (parent deckMixerCanvas applies uniformScale — no spViewport here)
    readonly property int headerH:        44
    readonly property int coverSize:      44
    readonly property int deckLabelW:     24
    readonly property int stripLabelW:    48
    readonly property int metaStripW:     208
    readonly property int tempoPanelW:    52
    readonly property int btnH:           24
    readonly property int wPlay:          38
    readonly property int wCue:           34
    readonly property int wBtn:           32
    readonly property int wBtnSm:         28
    readonly property int wBtnLg:         36
    readonly property int wLoopLen:       56
    readonly property int wReloop:        46
    readonly property color btnBg:          "#212121"
    readonly property color btnBgActive:    "#2c2c2c"
    readonly property color btnBgPressed:   UiTheme.bg5
    readonly property color btnLine:        UiTheme.separatorSubtle
    readonly property color btnLineActive:  "#aaaaaa"
    readonly property color btnText:        "#808080"
    readonly property color btnTextActive:  "#f0f0f0"
    readonly property color btnTextBright:  "#b8b8b8"
    readonly property color sectionLabel:   "#606060"
    readonly property color metaLabel:      "#666666"
    readonly property color metaValue:      "#a8a8a8"
    readonly property color metaEmpty:      "#484848"

    component SectionLabel: Text {
        required property string label
        Layout.preferredWidth: deck.stripLabelW
        Layout.minimumWidth: deck.stripLabelW
        Layout.maximumWidth: deck.stripLabelW
        Layout.alignment: Qt.AlignVCenter
        text: label
        color: deck.sectionLabel
        font.pixelSize: 8
        font.family: "monospace"
        font.letterSpacing: 0.5
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    component MetaCell: Item {
        required property string metaLabel
        required property string metaValue
        property bool metaHighlight: metaValue !== "" && metaValue !== "--" && metaValue !== "--:--"

        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 48

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 3
            anchors.topMargin: 4
            anchors.bottomMargin: 3
            spacing: 1

            Text {
                text: metaLabel
                color: deck.metaLabel
                font.pixelSize: 7
                font.bold: true
                font.family: "monospace"
            }
            Text {
                Layout.fillWidth: true
                text: metaValue
                color: metaHighlight ? deck.metaValue : deck.metaEmpty
                font.pixelSize: 10
                font.bold: true
                font.family: "monospace"
                elide: Text.ElideRight
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: metaHighlight ? deck.btnLineActive : deck.btnLine
        }
    }

    component GroupSpacer: Item {
        Layout.preferredWidth: 6
        Layout.minimumWidth: 6
        Layout.maximumWidth: 6
    }

    component BtnGroup: RowLayout {
        spacing: 2
        Layout.alignment: Qt.AlignVCenter
    }

    // ── Flat button — FxBar-style neutral chrome ─────────────────────────
    component FlatBtn: Rectangle {
        id: fb
        required property string btnText
        property bool   fbActive:           false
        property color  fbAccent:           deck.btnLineActive
        property color  fbActiveText:       deck.btnTextActive
        property color  fbInactiveText:     deck.btnTextBright
        property real   fbPreferredHeight:  deck.btnH
        property real   fbFontPx:           9
        property string fbFontFamily:       "monospace"

        Layout.preferredHeight: fbPreferredHeight
        Layout.minimumHeight:   fbPreferredHeight
        Layout.maximumHeight:   fbPreferredHeight

        radius: 0
        color: fbMouse.pressed ? deck.btnBgPressed : (fbActive ? deck.btnBgActive : deck.btnBg)

        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: fb.fbActive ? 2 : 1
            color: fb.fbActive ? fb.fbAccent
                 : (fbHov.hovered ? "#454545" : "#333333")
        }

        HoverHandler { id: fbHov }
        Rectangle {
            anchors.fill: parent
            color: "#ffffff"
            opacity: fbHov.hovered && !fbMouse.pressed ? 0.06 : 0
        }

        signal clicked()
        signal rightClicked()
        signal btnPressed()
        signal btnReleased()

        Text {
            anchors.centerIn:    parent
            text:                fb.btnText
            color:               fb.fbActive ? fb.fbActiveText : fb.fbInactiveText
            font.pixelSize:      fb.fbFontPx
            font.bold:           fb.fbActive
            font.letterSpacing:  0
            font.family:         fb.fbFontFamily
            horizontalAlignment: Text.AlignHCenter
            elide:               Text.ElideRight
        }

        MouseArea {
            id: fbMouse
            anchors.fill:    parent
            hoverEnabled:    true
            cursorShape:     Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton) fb.rightClicked()
                else fb.clicked()
            }
            onPressed:  fb.btnPressed()
            onReleased: fb.btnReleased()
        }
    }

    // ── Tempo slider ──────────────────────────────────────────────────────
    component DeckSlider: Controls.Slider {
        id: ds
        property bool centerFill:   false
        property bool dragActive:   false
        property real defaultValue: 0

        implicitWidth:  orientation === Qt.Vertical ? 22 : 150
        implicitHeight: orientation === Qt.Vertical ? 150 : 22

        background: Rectangle {
            x: ds.orientation === Qt.Horizontal ? ds.leftPadding  : ds.width  / 2 - 2
            y: ds.orientation === Qt.Horizontal ? ds.height / 2 - 2 : ds.topPadding
            width:  ds.orientation === Qt.Horizontal ? ds.availableWidth  : 4
            height: ds.orientation === Qt.Horizontal ? 4 : ds.availableHeight
            radius: 0
            color:  UiTheme.faderTrack

            Rectangle {
                visible: ds.orientation === Qt.Vertical && !ds.centerFill
                x: 1
                y: parent.height - 1 - Math.max(0, (1.0 - ds.visualPosition) * (parent.height - 2))
                width:  parent.width  - 2
                height: Math.max(0, (1.0 - ds.visualPosition) * (parent.height - 2))
                radius: 1
                color:  ds.pressed ? "#555555" : "#3a3a3a"
            }

            Rectangle {
                visible: ds.orientation === Qt.Vertical && ds.centerFill
                x: 1; width: parent.width - 2; radius: 1
                color:  ds.pressed ? "#555555" : "#3a3a3a"
                readonly property real midPy: parent.height / 2
                readonly property real posPy: 1 + ds.visualPosition * (parent.height - 2)
                y:      Math.min(midPy, posPy)
                height: Math.max(0, Math.abs(posPy - midPy))
            }
        }

        handle: Rectangle {
            implicitWidth:  ds.orientation === Qt.Vertical ? 26 : 18
            implicitHeight: ds.orientation === Qt.Vertical ? 10 : 22
            x: ds.orientation === Qt.Horizontal
               ? ds.leftPadding + ds.visualPosition * (ds.availableWidth - width)
               : ds.width / 2 - width / 2
            y: ds.orientation === Qt.Horizontal
               ? ds.height / 2 - height / 2
               : ds.topPadding + ds.visualPosition * (ds.availableHeight - height)
            radius: 1
            color:  ds.pressed || ds.dragActive ? "#e0e0e0" : "#c8c8c8"

            Rectangle {
                anchors.centerIn: parent
                width: 2; height: parent.height * 0.48
                color: "#888888"
            }
        }

        MouseArea {
            id: dsDragLock
            anchors.fill: parent
            z: 100
            acceptedButtons: Qt.LeftButton
            preventStealing: true

            property real _pressGX:  0
            property real _pressGY:  0
            property real _pressVal: 0

            onPressed: (mouse) => {
                var g    = dsDragLock.mapToGlobal(mouse.x, mouse.y)
                _pressGX  = g.x
                _pressGY  = g.y
                _pressVal = ds.value
                ds.dragActive = false
                mouse.accepted = true
            }

            onPositionChanged: (mouse) => {
                var g     = dsDragLock.mapToGlobal(mouse.x, mouse.y)
                var isV   = ds.orientation === Qt.Vertical
                var delta = isV ? (_pressGY - g.y) : (g.x - _pressGX)
                if (!ds.dragActive) {
                    if (Math.abs(delta) < 4) return
                    ds.dragActive = true
                    cursorControl.hideCursor()
                }
                var newVal = _pressVal + delta * (ds.to - ds.from) / 150.0
                var lo = Math.min(ds.from, ds.to)
                var hi = Math.max(ds.from, ds.to)
                ds.value = Math.max(lo, Math.min(hi, newVal))
            }

            onReleased: {
                if (ds.dragActive) {
                    ds.dragActive = false
                    cursorControl.restoreCursor()
                    cursorControl.moveCursor(_pressGX, _pressGY)
                }
            }

            onDoubleClicked: {
                ds.enabled = false
                ds.value   = ds.defaultValue
                ds.enabled = true
            }
        }
    }

    // ── Logic ─────────────────────────────────────────────────────────────

    function loopLabel() {
        if (!deck.engine || !deck.engine.loopActive)
            return "4 BEAT"
        var beats = deck.engine.loopLengthBeats
        if (Math.abs(beats - 1.5) < 0.08)  return "1.5"
        if (Math.abs(beats - 0.75) < 0.05) return "3/4"
        if (Math.abs(beats - 0.5)  < 0.04) return "1/2"
        if (Math.abs(beats - 0.25) < 0.03) return "1/4"
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
    }

    function _syncBpm() {
        if (!deck.engine || !deck.engine.trackData) return
        if (deck.engine.trackData.isBpmAnalyzed)
            _trackBpm = deck.engine.trackData.bpm.toFixed(2)
    }

    function _syncTempo() {
        if (!deck.engine) return
        var cb = deck.engine.currentBpm
        if (cb > 0 && deck.engine.loopActive) {
            var beats = deck.engine.loopLengthBeats
            if (beats > 0.001 && beats < 1.0)
                cb = cb / beats
            if (beats > 0.001 && beats < 1.0) {
                var minBpm = 70.0
                var maxBpm = 150.0
                while (cb > maxBpm) cb = cb / 2.0
                while (cb < minBpm) cb = cb * 2.0
            }
        }
        _currentBpm = cb > 0 ? cb.toFixed(2) : ""
    }

    function _syncTempoSlider() {
        if (!deck.engine) return
        var percent = Number(deck.engine.tempoPercent)
        if (!isFinite(percent)) return
        if (Math.abs(tempoSlider.value - percent) > 0.0001)
            tempoSlider.value = percent
    }

    function _showLiveBpmIndicator() {
        if (!deck.engine || deck._currentBpm === "")
            return false
        if (Math.abs(deck.engine.tempoPercent) > 0.01)
            return true
        var base = Number(deck._trackBpm)
        var live = Number(deck._currentBpm)
        if (!isNaN(base) && !isNaN(live))
            return Math.abs(live - base) > 0.01
        return false
    }

    function _setLinkMode(on) {
        if (deck.hostWindow === null)
            return
        deck.hostWindow.linkedDeckName = on ? deck.deckName : ""
    }

    function _isLinkLeader() {
        if (!deck.engine || !deck.linkAvailable) return false
        if (!linkManager) return false
        if (linkManager.numPeers <= 0) return true
        return deck.engine.syncMaster
    }

    function _shouldFollowLinkTempo() {
        if (!deck.engine || !deck.linkMode || !deck.linkAvailable) return false
        if (!linkManager || linkManager.numPeers <= 0) return false
        if (deck._isLinkLeader()) return false
        if (!deck.engine.isPlaying) return false
        if (deck.engine.scrubbing) return false
        if (Date.now() < deck._linkFollowBlockedUntilMs) return false
        if (!deck.engine.trackData || !deck.engine.trackData.isBpmAnalyzed) return false
        return true
    }

    function _followAbletonLinkTempo(forceNow) {
        if (!deck._shouldFollowLinkTempo()) return
        var linkBpm = Number(linkManager.bpm)
        var baseBpm = Number(deck.engine.trackData.bpm)
        if (isNaN(linkBpm) || !isFinite(linkBpm) || linkBpm <= 0.0) return
        if (isNaN(baseBpm) || !isFinite(baseBpm) || baseBpm <= 0.0) return
        var targetPct = ((linkBpm / baseBpm) - 1.0) * 100.0
        targetPct = Math.max(-100.0, Math.min(100.0, targetPct))
        var deltaPct = Math.abs(Number(deck.engine.tempoPercent) - targetPct)
        if (!forceNow && deltaPct < 0.03) return
        deck.engine.setTempoPercent(targetPct)
        deck._linkSuppressPublishUntilMs = Date.now() + 420
    }

    function _publishDeckToAbletonLink() {
        if (!deck.engine || !deck.linkMode || !deck.linkAvailable) return
        if (!deck.engine.isPlaying) return
        if (!deck._isLinkLeader()) return
        if (Date.now() < deck._linkSuppressPublishUntilMs) return
        if (!deck.engine.trackData || !deck.engine.trackData.isBpmAnalyzed) return
        var liveBpm = Number(deck.engine.currentBpm)
        if (isNaN(liveBpm) || liveBpm <= 0.0) return
        var playheadSec = deck.engine.getPlayheadPositionAtomic()
        if (playheadSec === undefined || isNaN(playheadSec)) return
        var sampleRate = Number(deck.engine.trackData.sampleRate)
        if (isNaN(sampleRate) || sampleRate <= 0.0) sampleRate = 44100.0
        var firstBeatSec = Number(deck.engine.trackData.firstBeatSample) / sampleRate
        var analyzedBpm = Number(deck.engine.trackData.bpm)
        if (isNaN(analyzedBpm) || analyzedBpm <= 0.0) analyzedBpm = liveBpm
        var beatDur = 60.0 / analyzedBpm
        var absoluteBeat = (playheadSec - firstBeatSec) / beatDur
        if (isNaN(absoluteBeat) || !isFinite(absoluteBeat)) return
        var nowMs = Date.now()
        var minPublishIntervalMs = deck.engine.scrubbing ? 70 : 120
        if ((nowMs - deck._lastLinkPublishMs) < minPublishIntervalMs
            && Math.abs(liveBpm - deck._lastLinkPublishBpm) < 0.02)
            return
        linkManager.publishDeckState(liveBpm, absoluteBeat, 4.0)
        deck._lastLinkPublishMs = nowMs
        deck._lastLinkPublishBpm = liveBpm
    }

    function _handleLinkEnabledChanged() {
        if (typeof linkManager === "undefined" || linkManager === null) return
        if (!linkManager.enabled && deck.linkMode) deck._setLinkMode(false)
        if (linkManager.enabled && deck.linkMode)  deck._followAbletonLinkTempo(true)
    }

    Component.onCompleted: {
        if (typeof linkManager !== "undefined" && linkManager !== null)
            linkManager.enabledChanged.connect(deck._handleLinkEnabledChanged)
        deck._syncTempoSlider()
    }

    Component.onDestruction: {
        if (typeof linkManager !== "undefined" && linkManager !== null)
            linkManager.enabledChanged.disconnect(deck._handleLinkEnabledChanged)
    }

    onLinkModeChanged: {
        // The always-present AIO deck owns Link clock publication.  The
        // development-window duplicate is only an input surface.
        if (deck.developmentControls)
            return
        if (deck.linkMode) {
            deck._linkSuppressPublishUntilMs = Date.now() + 180
            deck._publishDeckToAbletonLink()
            deck._followAbletonLinkTempo(true)
        }
    }

    Connections {
        target: deck.engine
        function onTrackMetadataChanged() { deck._syncMetadata() }
        function onTempoChanged()         { deck._syncTempo(); deck._syncTempoSlider() }
        function onTempoRangeChanged()    { deck._syncTempoSlider() }
        function onLoopChanged()          { deck._syncTempo() }
        function onScrubbingChanged() {
            if (!deck.engine) return
            if (deck.engine.scrubbing) {
                deck._linkFollowBlockedUntilMs   = Date.now() + 120
            } else {
                deck._linkFollowBlockedUntilMs   = Date.now() + 280
                deck._linkSuppressPublishUntilMs = Date.now() + 320
                deck._followAbletonLinkTempo(true)
            }
        }
    }

    Connections {
        target: deck.engine ? deck.engine.trackData : null
        function onBpmAnalyzed()    { deck._syncBpm(); deck._syncTempo() }
        function onBeatgridChanged(){ deck._syncBpm(); deck._syncTempo() }
    }

    Connections {
        target: (typeof linkManager !== "undefined" && linkManager !== null) ? linkManager : null
        function onBpmChanged()  { deck._followAbletonLinkTempo(false) }
        function onPhaseChanged(){}
    }

    Connections {
        target: (typeof controlClock !== "undefined") ? controlClock : null
        function onLinkTick() {
            if (!deck.developmentControls && deck.linkMode && deck.linkAvailable && deck.engine !== null
                    && deck.engine.isPlaying)
                deck._publishDeckToAbletonLink()
        }
    }

    Connections {
        target: parameterStore
        function onParameterChanged(id, value) {
            if (!deck.engine) return
            // Tempo: MIDI 0-1 → slider percentage ±range
            // (play/cue are handled in main.cpp; duplicating here causes double-toggle bugs)
            if (id === "deck" + deck.deckName + "_tempo")
                tempoSlider.value = value * 2.0 * tempoPanel.tempoRange - tempoPanel.tempoRange
        }
    }

    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list", "text/plain"]
        onEntered: (drag) => { drag.accept(Qt.CopyAction); deck.dropHovered = true }
        onExited:  ()      => { deck.dropHovered = false }
        onDropped: (drop)  => {
            deck.dropHovered = false
            var path = ""
            if (drop.hasUrls && drop.urls.length > 0) path = drop.urls[0].toString()
            else if (drop.hasText)                    path = drop.text
            if (path.startsWith("file://")) path = path.substring(7)
            if (path !== "" && deck.engine)  deck.engine.loadTrack(path)
        }
    }

    // ── Visual ────────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        color: UiTheme.panel

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            DeckTrackInfoPanel {
                visible: !deck.controlsOnly
                Layout.fillWidth: true
                Layout.preferredHeight: deck.controlsOnly ? 0 : 275
                Layout.minimumHeight: deck.controlsOnly ? 0 : 275
                Layout.maximumHeight: deck.controlsOnly ? 0 : 275
                deckName: deck.deckName
                engine: deck.engine
                onAir: deck.engine ? deck.engine.onAir : false
            }

            // ── Track-info header ──────────────────────────────────────────
            Rectangle {
                visible: false
                Layout.fillWidth: true
                Layout.preferredHeight: 0
                Layout.minimumHeight: 0
                Layout.maximumHeight: 0
                color: UiTheme.panel

                RowLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.preferredWidth: deck.deckLabelW
                        Layout.minimumWidth: deck.deckLabelW
                        Layout.fillHeight: true
                        color: UiTheme.panelDeep

                        Text {
                            anchors.centerIn: parent
                            text: deck.deckName
                            color: deck.accent
                            opacity: deck._hasTrack ? 1.0 : 0.4
                            font.pixelSize: 13
                            font.bold: true
                            font.family: "monospace"
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 1
                        Layout.fillHeight: true
                        color: deck.accent
                        opacity: deck._hasTrack ? 0.75 : 0.18
                    }

                    Rectangle {
                        id: coverArtBox
                        Layout.preferredWidth: deck.coverSize
                        Layout.minimumWidth: deck.coverSize
                        Layout.maximumWidth: deck.coverSize
                        Layout.fillHeight: true
                        color: UiTheme.panelDeep

                        Text {
                            anchors.centerIn: parent
                            text: deck._hasTrack ? "♪" : deck.deckName
                            color: deck.accent
                            font.pixelSize: deck._hasTrack ? 14 : 16
                            font.bold: !deck._hasTrack
                            opacity: deck._hasTrack ? 0.45 : 0.22
                            visible: coverImage.status !== Image.Ready
                        }
                        Image {
                            id: coverImage
                            anchors.fill: parent
                            source: deck.engine && deck.engine.hasCoverArt ? deck.engine.coverArtUrl : ""
                            fillMode: Image.PreserveAspectCrop
                            visible: status === Image.Ready
                        }

                        // Eject overlay — HoverHandler sits on the container so it
                        // always fires regardless of child opacity.
                        HoverHandler { id: coverHov }

                        Rectangle {
                            anchors.fill: parent
                            visible: deck.developmentControls
                            color: "#000000"
                            opacity: deck._hasTrack && coverHov.hovered ? 0.72 : 0.0
                            Behavior on opacity { NumberAnimation { duration: 100 } }

                            Column {
                                anchors.centerIn: parent
                                spacing: 2

                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "▲"
                                    color: "#ffffff"
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "EJECT"
                                    color: "#aaaaaa"
                                    font.pixelSize: 7
                                    font.bold: true
                                    font.letterSpacing: 0.5
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            visible: deck.developmentControls
                            cursorShape: deck._hasTrack ? Qt.PointingHandCursor : Qt.ArrowCursor
                            enabled: deck.developmentControls
                            onClicked: { if (deck._hasTrack && deck.engine) deck.engine.ejectTrack() }
                        }
                    }

                    // Title + Artist
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        spacing: 0

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Text {
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.leftMargin: 7; anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: -1
                                text: deck._hasTrack ? deck._trackTitle : "No Track Loaded"
                                color: deck._hasTrack ? "#ececec" : "#707070"
                                font.pixelSize: 11
                                font.bold: true
                                elide: Text.ElideRight
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Text {
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.leftMargin: 7; anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: 1
                                text: deck._hasTrack
                                      ? (deck._trackArtist !== "" ? deck._trackArtist : "Unknown Artist")
                                      : ""
                                color: deck._hasTrack ? "#999999" : "#505050"
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }
                    }

                    GroupSpacer {}

                    // Track meta
                    RowLayout {
                        id: metaBadges
                        Layout.preferredWidth: deck.metaStripW
                        Layout.minimumWidth: deck.metaStripW
                        Layout.maximumWidth: deck.metaStripW
                        Layout.fillHeight: true
                        spacing: 0

                        Item {
                            id: bpmBadge
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            MetaCell {
                                anchors.fill: parent
                                metaLabel: "BPM"
                                metaValue: deck._trackBpm !== "" ? deck._trackBpm : "--"
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: deck.developmentControls
                                acceptedButtons: Qt.RightButton
                                cursorShape: Qt.PointingHandCursor
                                onClicked: (mouse) => {
                                    if (mouse.button !== Qt.RightButton || !deck.engine) return
                                    deck._manualBpmInput = deck._trackBpm !== "" ? deck._trackBpm : (deck._currentBpm !== "" ? deck._currentBpm : "120.00")
                                    manualBpmField.text = deck._manualBpmInput
                                    manualBpmPopup.visible = true
                                    manualBpmField.forceActiveFocus()
                                    manualBpmField.selectAll()
                                }
                            }
                        }

                        MetaCell {
                            Layout.fillWidth: true
                            metaLabel: "LIVE"
                            metaValue: deck._currentBpm !== "" ? deck._currentBpm : "--"
                            metaHighlight: deck._showLiveBpmIndicator()
                        }

                        MetaCell {
                            Layout.fillWidth: true
                            metaLabel: "KEY"
                            metaValue: deck._trackKey !== "" ? deck._trackKey : "--"
                        }

                        MetaCell {
                            Layout.fillWidth: true
                            metaLabel: "LEN"
                            metaValue: deck._trackDuration !== "" ? deck._trackDuration : "--:--"
                        }
                    }
                }
            }

            Rectangle {
                visible: false
                Layout.fillWidth: true
                Layout.preferredHeight: 0
                Layout.minimumHeight: 0
                Layout.maximumHeight: 0
                color: UiTheme.separatorSubtle
            }

            // The previous overview and analysis bar are no longer part of the
            // touch surface. Do not instantiate hidden waveform components here:
            // they keep signal connections and per-frame work alive off-screen.

            Rectangle {
                visible: false
                Layout.fillWidth: true
                Layout.preferredHeight: 0
                Layout.minimumHeight: 0
                Layout.maximumHeight: 0
                color: UiTheme.separatorSubtle
            }

            // ── Transport / loop controls ─────────────────────────────────
            Item {
                Layout.fillWidth: true
                readonly property int controlsHeight: deck.developmentControls ? deck.btnH * 2 + 1 : 0
                Layout.minimumHeight: controlsHeight
                Layout.preferredHeight: controlsHeight
                Layout.maximumHeight: controlsHeight
                visible: deck.developmentControls

                Rectangle {
                    anchors.fill: parent
                    color: UiTheme.panelInset
                }

                Text {
                    visible: deck.controlsOnly
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: "DECK " + deck.deckName
                    color: deck.accent
                    font.pixelSize: 8
                    font.bold: true
                    font.letterSpacing: 0.8
                    font.family: "monospace"
                }

            ColumnLayout {
                id: deckControlsCol
                anchors.fill: parent
                spacing: 1

                // Row 1 — transport
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: deck.btnH
                    spacing: 0

                    SectionLabel { label: "TRNS" }

                    BtnGroup {
                        FlatBtn {
                            btnText: deck.engine && deck.engine.isPlaying ? "Ⅱ" : "▶"
                            Layout.preferredWidth: deck.wPlay
                            Layout.minimumWidth: deck.wPlay
                            Layout.maximumWidth: deck.wPlay
                            fbFontPx: 14
                            fbFontFamily: "sans-serif"
                            fbActive: deck.engine ? deck.engine.isPlaying : false
                            fbAccent: "#3acc3a"
                            fbActiveText: "#3acc3a"
                            fbInactiveText: deck.btnTextBright
                            onClicked: {
                                if (!deck.engine) return
                                if (performancePads.consumeHotCueHoldPlayLatch()) return
                                deck.engine.togglePlay()
                            }
                        }
                        FlatBtn {
                            btnText: "CUE"
                            Layout.preferredWidth: deck.wCue
                            Layout.minimumWidth: deck.wCue
                            Layout.maximumWidth: deck.wCue
                            fbFontPx: 9
                            fbInactiveText: deck.btnTextBright
                            onBtnPressed:  { if (deck.engine) deck.engine.cueButtonPress() }
                            onBtnReleased: { if (deck.engine) deck.engine.cueButtonRelease() }
                        }
                    }

                    GroupSpacer {}

                    BtnGroup {
                        FlatBtn {
                            btnText: "REV"
                            Layout.preferredWidth: deck.wBtn
                            Layout.minimumWidth: deck.wBtn
                            Layout.maximumWidth: deck.wBtn
                            fbActive: deck.engine ? deck.engine.isReverse : false
                            onClicked: { if (deck.engine) deck.engine.setReverse(!deck.engine.isReverse) }
                        }
                        FlatBtn {
                            id: syncBtn
                            property bool isMaster: deck.engine ? deck.engine.syncMaster : false
                            btnText: isMaster ? "MASTER" : "SYNC"
                            Layout.preferredWidth: deck.wBtnLg
                            Layout.minimumWidth: deck.wBtnLg
                            Layout.maximumWidth: deck.wBtnLg
                            fbActive: deck.engine ? deck.engine.syncEnabled : false
                            fbAccent: isMaster ? "#ffb000" : "#3acc3a"
                            fbActiveText: isMaster ? "#ffb000" : "#3acc3a"
                            onClicked: {
                                if (!deck.engine) return
                                if (deck.engine.syncEnabled) deck.engine.reSync()
                                else deck.engine.setSyncEnabled(true)
                            }
                            onRightClicked: { if (deck.engine) deck.engine.setSyncEnabled(false) }

                            HoverHandler { id: syncTipHover }
                            Controls.ToolTip {
                                visible: deck.sameTrackDoubleWarning && syncTipHover.hovered
                                delay: 400
                                text: "Same file on multiple decks can sound thin (comb filtering). Nudge phase, use EQ, or polarity (−) on one channel."
                            }
                        }
                        FlatBtn {
                            btnText: "LINK"
                            Layout.preferredWidth: deck.wBtn
                            Layout.minimumWidth: deck.wBtn
                            Layout.maximumWidth: deck.wBtn
                            fbActive: deck.linkMode
                            fbAccent: "#3acc3a"
                            fbActiveText: "#3acc3a"
                            fbInactiveText: deck.linkAvailable ? deck.btnText : "#444444"
                            onClicked: {
                                if (!deck.linkAvailable) { deck._setLinkMode(false); return }
                                deck._setLinkMode(!deck.linkMode)
                                if (deck.linkMode && !deck.developmentControls) deck._publishDeckToAbletonLink()
                            }
                        }
                    }

                    GroupSpacer {}

                    BtnGroup {
                        FlatBtn {
                            btnText: "Q"
                            Layout.preferredWidth: deck.wBtnSm
                            Layout.minimumWidth: deck.wBtnSm
                            Layout.maximumWidth: deck.wBtnSm
                            fbActive: deck.engine ? deck.engine.quantizeEnabled : false
                            fbAccent: deck.accent
                            fbActiveText: deck.accent
                            onClicked: { if (deck.engine) deck.engine.quantizeEnabled = !deck.engine.quantizeEnabled }
                        }
                        FlatBtn {
                            btnText: "KL"
                            Layout.preferredWidth: deck.wBtnSm
                            Layout.minimumWidth: deck.wBtnSm
                            Layout.maximumWidth: deck.wBtnSm
                            fbActive: deck.engine ? deck.engine.keylock : false
                            fbAccent: deck.accent
                            fbActiveText: deck.accent
                            onClicked: { if (deck.engine) deck.engine.keylock = !deck.engine.keylock }
                        }
                        FlatBtn {
                            btnText: "SLIP"
                            Layout.preferredWidth: deck.wBtnLg
                            Layout.minimumWidth: deck.wBtnLg
                            Layout.maximumWidth: deck.wBtnLg
                            fbActive: deck.engine ? deck.engine.slipActive : false
                            fbAccent: deck.accent
                            fbActiveText: deck.accent
                            onClicked: { if (deck.engine) deck.engine.setSlip(!deck.engine.slipActive) }
                        }
                    }

                    Item { Layout.fillWidth: true }
                }

                // Row 2 — loop
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: deck.btnH
                    spacing: 0

                    SectionLabel { label: "LOOP" }

                    BtnGroup {
                        FlatBtn {
                            btnText: "IN"
                            Layout.preferredWidth: deck.wBtnSm
                            Layout.minimumWidth: deck.wBtnSm
                            Layout.maximumWidth: deck.wBtnSm
                            onClicked: { if (deck.engine) deck.engine.setLoopIn() }
                        }
                        FlatBtn {
                            btnText: "OUT"
                            Layout.preferredWidth: deck.wBtnSm
                            Layout.minimumWidth: deck.wBtnSm
                            Layout.maximumWidth: deck.wBtnSm
                            onClicked: { if (deck.engine) deck.engine.setLoopOut() }
                        }
                    }

                    GroupSpacer {}

                    BtnGroup {
                        FlatBtn {
                            btnText: "◀"
                            Layout.preferredWidth: deck.wBtnSm
                            Layout.minimumWidth: deck.wBtnSm
                            Layout.maximumWidth: deck.wBtnSm
                            onClicked: { if (deck.engine) deck.engine.halveLoopLength() }
                        }
                        FlatBtn {
                            btnText: deck.loopLabel()
                            Layout.preferredWidth: deck.wLoopLen
                            Layout.minimumWidth: deck.wLoopLen
                            Layout.maximumWidth: deck.wLoopLen
                            fbActive: deck.engine ? deck.engine.loopActive : false
                            fbAccent: "#3acc3a"
                            fbActiveText: "#3acc3a"
                            onClicked: { if (deck.engine) deck.engine.toggleLoop4Beats() }
                        }
                        FlatBtn {
                            btnText: "▶"
                            Layout.preferredWidth: deck.wBtnSm
                            Layout.minimumWidth: deck.wBtnSm
                            Layout.maximumWidth: deck.wBtnSm
                            onClicked: { if (deck.engine) deck.engine.doubleLoopLength() }
                        }
                        FlatBtn {
                            btnText: "3/4"
                            Layout.preferredWidth: deck.wBtnSm
                            Layout.minimumWidth: deck.wBtnSm
                            Layout.maximumWidth: deck.wBtnSm
                            fbActive: deck.engine ? (deck.engine.loopActive && Math.abs(deck.engine.loopLengthBeats - 0.75) < 0.06) : false
                            onClicked: { if (deck.engine) deck.engine.toggleLoopThreeQuarter() }
                        }
                    }

                    GroupSpacer {}

                    BtnGroup {
                        FlatBtn {
                            btnText: "RELOOP"
                            Layout.preferredWidth: deck.wReloop
                            Layout.minimumWidth: deck.wReloop
                            Layout.maximumWidth: deck.wReloop
                            readonly property bool _hasLoop: deck.engine ? deck.engine.loopInPosition < deck.engine.loopOutPosition : false
                            opacity: _hasLoop ? 1.0 : 0.35
                            enabled: _hasLoop
                            fbActive: deck.engine ? deck.engine.loopActive : false
                            fbAccent: "#3acc3a"
                            fbActiveText: "#3acc3a"
                            onClicked: {
                                if (!deck.engine) return
                                if (deck.engine.loopActive)
                                    deck.engine.deactivateLoop()
                                else
                                    deck.engine.reactivateLoop()
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }
                }
            }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.minimumHeight: deck.developmentControls ? 1 : 0
                Layout.preferredHeight: deck.developmentControls ? 1 : 0
                Layout.maximumHeight: deck.developmentControls ? 1 : 0
                visible: deck.developmentControls
                color: UiTheme.separatorSubtle
            }

            // ── Performance pads + tempo fader ────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: false
                Layout.preferredHeight: deck.controlsOnly ? 88 : 100
                Layout.minimumHeight: deck.controlsOnly ? 88 : 100
                Layout.maximumHeight: deck.controlsOnly ? 88 : 100
                spacing: 0

                PerformancePads {
                    id: performancePads
                    visible: !deck.controlsOnly
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: deck.controlsOnly ? 0 : -1
                    Layout.minimumWidth: deck.controlsOnly ? 0 : 0
                    Layout.maximumWidth: deck.controlsOnly ? 0 : 16777215
                    engine: deck.engine
                    deckId: deck.channelId !== "" ? deck.channelId : (deck.deckName === "B" ? "deckB" : "deckA")
                    accentColor: deck.accent
                    compact: true
                    // The touch surface keeps its complete pad-page selector;
                    // transport and mixer controls still live in the separate
                    // development window.
                    hotCueOnly: false
                }

                Rectangle {
                    visible: deck.developmentControls
                    Layout.preferredWidth: deck.developmentControls ? 1 : 0
                    Layout.minimumWidth: deck.developmentControls ? 1 : 0
                    Layout.maximumWidth: deck.developmentControls ? 1 : 0
                    Layout.fillHeight: true
                    color: UiTheme.separatorSubtle
                }

                Rectangle {
                        id: tempoPanel
                        visible: deck.developmentControls
                        Layout.preferredWidth: deck.developmentControls ? deck.tempoPanelW : 0
                        Layout.minimumWidth: deck.developmentControls ? deck.tempoPanelW : 0
                        Layout.maximumWidth: deck.developmentControls ? deck.tempoPanelW : 0
                        Layout.fillHeight: true
                        color: UiTheme.panel

                        property real tempoRange: deck.engine ? deck.engine.tempoRangePercent : 8

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 2
                            spacing: 1

                            Rectangle {
                                id: tempoHeader
                                Layout.fillWidth: true
                                Layout.preferredHeight: 16
                                color: "transparent"

                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text { text: "TEMPO"; color: "#666666"; font.pixelSize: 7; font.bold: true; font.letterSpacing: 0.6; font.family: "monospace"; anchors.verticalCenter: parent.verticalCenter }
                                    Text { text: "▾"; color: "#777777"; font.pixelSize: 8; anchors.verticalCenter: parent.verticalCenter }
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: tempoRangePopup.visible = !tempoRangePopup.visible
                                }
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: UiTheme.separatorSubtle }
                            }

                            DeckSlider {
                                id: tempoSlider
                                Layout.fillWidth: true; Layout.fillHeight: true; Layout.alignment: Qt.AlignHCenter
                                orientation: Qt.Vertical
                                from: tempoPanel.tempoRange; to: -tempoPanel.tempoRange; value: 0
                                centerFill: true
                                stepSize: tempoPanel.tempoRange <= 8  ? 0.1
                                        : tempoPanel.tempoRange <= 16 ? 0.25
                                        : tempoPanel.tempoRange <= 32 ? 0.5
                                        : 1.0
                                onValueChanged: { if (deck.engine) deck.engine.setTempoPercent(value) }
                            }

                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: (tempoSlider.value >= 0 ? "+" : "") + tempoSlider.value.toFixed(1) + "%"
                                color: Math.abs(tempoSlider.value) > 0.05 ? deck.metaValue : deck.metaEmpty
                                font.pixelSize: 9; font.bold: true; font.family: "monospace"
                            }
                        }
                    }
                }
        }

        // ── Drop-hover overlay (must be above all content) ────────────────
        Rectangle {
            anchors.fill: parent
            z: 5
            color: "#5599ff"
            opacity: deck.dropHovered ? 0.08 : 0.0
            Behavior on opacity { NumberAnimation { duration: 80 } }
        }
        Rectangle {
            anchors.fill: parent
            z: 5
            color: "transparent"
            border.color: "#5599ff"
            border.width: 3
            visible: deck.dropHovered
            Behavior on opacity { NumberAnimation { duration: 80 } }
        }

        // ── Tempo range popup ──────────────────────────────────────────────
        Rectangle {
            id: tempoRangePopup
            visible: false; z: 999
            x: tempoPanel.x; y: tempoPanel.y + tempoHeader.height + 2
            width: tempoPanel.width; height: rangeCol.implicitHeight + 10
            color: UiTheme.panel
            border.color: UiTheme.separator
            border.width: 1
            radius: 0

            MouseArea { anchors.fill: parent; onClicked: mouse => mouse.accepted = true }

            Column {
                id: rangeCol
                anchors { top: parent.top; left: parent.left; right: parent.right; margins: 5 }
                spacing: 2

                Repeater {
                    model: [
                        { label: "6%",   value: 6   },
                        { label: "8%",   value: 8   },
                        { label: "10%",  value: 10  },
                        { label: "16%",  value: 16  },
                        { label: "20%",  value: 20  },
                        { label: "32%",  value: 32  },
                        { label: "WIDE", value: 100 }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool isActive: tempoPanel.tempoRange === modelData.value
                        width: rangeCol.width
                        height: 18
                        radius: 0
                        color: isActive ? UiTheme.panelRaised : UiTheme.panel

                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 1
                            color: isActive ? deck.btnLineActive : deck.btnLine
                        }

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: isActive ? deck.btnTextActive : "#484848"
                            font.pixelSize: 8
                            font.bold: isActive
                            font.family: "monospace"
                        }

                        HoverHandler { id: rangeHov }
                        Rectangle { anchors.fill: parent; color: "#ffffff"; opacity: rangeHov.hovered ? 0.04 : 0 }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                var clamped = Math.max(-modelData.value, Math.min(modelData.value, tempoSlider.value))
                                tempoSlider.enabled = false; tempoSlider.value = clamped; tempoSlider.enabled = true
                                if (deck.engine) {
                                    deck.engine.setTempoRangePercent(modelData.value)
                                    deck.engine.setTempoPercent(clamped)
                                }
                                tempoRangePopup.visible = false
                            }
                        }
                    }
                }
            }
        }

        // ── Manual BPM popup ──────────────────────────────────────────────
        Rectangle {
            id: manualBpmPopup
            visible: false; z: 1000
            x: Math.max(6, Math.min(parent.width - width - 6,
                                    metaBadges.x + bpmBadge.x + bpmBadge.width - width))
            y: metaBadges.y + bpmBadge.height + 6
            width: 160; height: 92; radius: 0
            color: "#181818"; border.color: "#333333"; border.width: 1

            MouseArea { anchors.fill: parent; onClicked: mouse => mouse.accepted = true }

            Column {
                anchors.fill: parent; anchors.margins: 6; spacing: 6
                Text { text: "MANUAL BPM"; color: "#555555"; font.pixelSize: 8; font.bold: true; font.family: "monospace" }
                Controls.TextField {
                    id: manualBpmField
                    width: parent.width; height: 28
                    placeholderText: "e.g. 124.50"
                    color: "#e8e8e8"
                    placeholderTextColor: "#484848"
                    selectionColor: deck.btnLineActive
                    selectedTextColor: deck.btnBg
                    font.pixelSize: 10; font.bold: true; font.family: "monospace"
                    background: Rectangle {
                        color: "#1e1e1e"
                        radius: 0
                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 1
                            color: manualBpmField.activeFocus ? deck.btnLineActive : "#333333"
                        }
                    }
                    onAccepted: applyManualBpm()
                    function applyManualBpm() {
                        var v = Number(text.replace(",", "."))
                        if (!deck.engine || isNaN(v) || v <= 0) return
                        deck.engine.setManualBpm(v); deck._syncBpm(); deck._syncTempo()
                        manualBpmPopup.visible = false
                    }
                }
                Text { text: "Enter to apply"; color: "#484848"; font.pixelSize: 7; font.family: "monospace" }
            }
        }

        // Dismiss overlay
        MouseArea {
            anchors.fill: parent; z: 998
            visible: tempoRangePopup.visible || manualBpmPopup.visible
            onClicked: { tempoRangePopup.visible = false; manualBpmPopup.visible = false }
        }
    }
}
