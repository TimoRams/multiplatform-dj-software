import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import DJSoftware

Item {
    id: deck
    property string deckName: "A"
    property var engine: null
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
    readonly property bool linkMode: (typeof window !== "undefined" && window !== null && window.linkedDeckName === deck.deckName)
    readonly property int headerCellHeight: 20
    property double _linkSuppressPublishUntilMs: 0
    property double _linkFollowBlockedUntilMs: 0
    property double _lastLinkPublishMs: 0
    property double _lastLinkPublishBpm: 0

    // ── Theme ────────────────────────────────────────────────────────────
    readonly property color accent:    UiTheme.deckColor(deck.deckName)
    readonly property color accentGrn: UiTheme.green
    readonly property color accentBlu: UiTheme.blue
    readonly property int   btnH:      20

    // ── Flat button — matches FxBar / FxUnit chrome ───────────────────────
    component FlatBtn: Rectangle {
        id: fb
        required property string btnText
        property bool   fbActive:           false
        property color  fbAccent:           deck.accent
        property color  fbActiveText:       fbAccent
        property color  fbInactiveText:     "#666666"
        property real   fbPreferredHeight:  deck.btnH
        property real   fbFontPx:           8
        property string fbFontFamily:       "monospace"

        Layout.preferredHeight: fbPreferredHeight
        Layout.minimumHeight:   fbPreferredHeight
        Layout.maximumHeight:   fbPreferredHeight

        radius: 0
        color: fbMouse.pressed ? "#2d2d2d" : (fbActive ? "#1a1a2a" : "#181818")

        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: fb.fbActive ? 2 : 1
            color: fb.fbActive ? fb.fbAccent : "#222222"
        }

        HoverHandler { id: fbHov }
        Rectangle {
            anchors.fill: parent
            color: "#ffffff"
            opacity: fbHov.hovered && !fbMouse.pressed ? 0.04 : 0
        }

        signal clicked()
        signal rightClicked()
        signal btnPressed()
        signal btnReleased()

        Text {
            anchors.centerIn:    parent
            text:                fb.btnText
            color:               fb.fbActive ? fb.fbActiveText : fb.fbInactiveText
            font.pixelSize:      window.spViewport(fb.fbFontPx)
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
            radius: 1
            color:  "#111111"

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
        if (typeof window === "undefined" || window === null)
            return
        window.linkedDeckName = on ? deck.deckName : ""
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
    }

    Component.onDestruction: {
        if (typeof linkManager !== "undefined" && linkManager !== null)
            linkManager.enabledChanged.disconnect(deck._handleLinkEnabledChanged)
    }

    onLinkModeChanged: {
        if (deck.linkMode) {
            deck._linkSuppressPublishUntilMs = Date.now() + 180
            deck._publishDeckToAbletonLink()
            deck._followAbletonLinkTempo(true)
        }
    }

    Connections {
        target: deck.engine
        function onTrackMetadataChanged() { deck._syncMetadata() }
        function onTempoChanged()         { deck._syncTempo() }
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

    Timer {
        id: linkDeckSyncTimer
        interval: 50
        repeat: true
        running: deck.linkMode && deck.linkAvailable && deck.engine !== null && deck.engine.isPlaying
        onTriggered: deck._publishDeckToAbletonLink()
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
        color: "#181818"

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: "#0a0a0a"
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Track-info header ──────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: deck.headerCellHeight * 2
                color: "#181818"

                RowLayout {
                    anchors.fill: parent
                    spacing: 0

                    // Deck label strip
                    Rectangle {
                        Layout.preferredWidth: 22
                        Layout.fillHeight: true
                        color: "#111111"

                        Text {
                            anchors.centerIn: parent
                            text: deck.deckName
                            color: deck.accent
                            opacity: deck._hasTrack ? 1.0 : 0.35
                            font.pixelSize: window.spViewport(14)
                            font.bold: true
                            font.family: "monospace"
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.fillHeight: true
                        color: deck.accent
                        opacity: deck._hasTrack ? 1.0 : 0.25
                    }

                    // Cover art
                    Rectangle {
                        id: coverArtBox
                        Layout.preferredWidth: deck.headerCellHeight * 2
                        Layout.minimumWidth:   deck.headerCellHeight * 2
                        Layout.maximumWidth:   deck.headerCellHeight * 2
                        Layout.fillHeight: true
                        color: "#0a0a0a"

                        Text {
                            anchors.centerIn: parent
                            text: deck._hasTrack ? "♪" : deck.deckName
                            color: deck.accent
                            font.pixelSize: deck._hasTrack ? window.spViewport(16) : window.spViewport(24)
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
                                    font.pixelSize: window.spViewport(13)
                                    font.bold: true
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "EJECT"
                                    color: "#aaaaaa"
                                    font.pixelSize: window.spViewport(7)
                                    font.bold: true
                                    font.letterSpacing: 0.5
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: deck._hasTrack ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: { if (deck._hasTrack && deck.engine) deck.engine.ejectTrack() }
                        }
                    }

                    Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#0a0a0a" }

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
                                color: deck._hasTrack ? "#e8e8e8" : "#666666"
                                font.pixelSize: window.spViewport(10)
                                font.bold: true
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "#222222" }

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
                                color: deck._hasTrack ? "#888888" : "#484848"
                                font.pixelSize: window.spViewport(9)
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#0a0a0a" }

                    // Meta badges 2×2
                    GridLayout {
                        id: metaBadges
                        columns: 2; rowSpacing: 0; columnSpacing: 0
                        Layout.preferredWidth: 164; Layout.fillHeight: true

                        Rectangle {
                            id: bpmBadge
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#181818"

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 1
                                color: deck._trackBpm !== "" ? "#3acc3a" : "#222222"
                            }

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "BPM"; color: "#555555"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: deck._trackBpm !== "" ? deck._trackBpm : "--"; color: deck._trackBpm !== "" ? "#888888" : "#333333"; font.pixelSize: window.spViewport(10); font.bold: true; font.family: "monospace" }
                            }
                            MouseArea {
                                anchors.fill: parent; acceptedButtons: Qt.RightButton; cursorShape: Qt.PointingHandCursor
                                onClicked: (mouse) => {
                                    if (mouse.button !== Qt.RightButton || !deck.engine) return
                                    deck._manualBpmInput = deck._trackBpm !== "" ? deck._trackBpm : (deck._currentBpm !== "" ? deck._currentBpm : "120.00")
                                    manualBpmField.text = deck._manualBpmInput
                                    manualBpmPopup.visible = true
                                    manualBpmField.forceActiveFocus()
                                    manualBpmField.selectAll()
                                }
                            }
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#222222" }
                        }

                        Rectangle {
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#181818"

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 1
                                color: deck._showLiveBpmIndicator() ? deck.accentBlu : "#222222"
                            }

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "LIVE"; color: "#555555"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: deck._currentBpm !== "" ? deck._currentBpm : "--"
                                    color: {
                                        if (!deck._showLiveBpmIndicator()) return "#333333"
                                        if (!deck.engine) return "#888888"
                                        return deck.engine.tempoPercent > 0 ? "#ffaa00" : "#888888"
                                    }
                                    font.pixelSize: window.spViewport(10); font.bold: true; font.family: "monospace"
                                }
                            }
                            Rectangle { anchors.left: parent.left; height: parent.height; width: 1; color: "#0a0a0a" }
                        }

                        Rectangle {
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#181818"

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 1
                                color: deck._trackKey !== "" ? deck.accentBlu : "#222222"
                            }

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "KEY"; color: "#555555"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: deck._trackKey !== "" ? deck._trackKey : "--"; color: deck._trackKey !== "" ? "#888888" : "#333333"; font.pixelSize: window.spViewport(10); font.bold: true; font.family: "monospace" }
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#181818"

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 1
                                color: deck._trackDuration !== "" ? "#3a3a3a" : "#222222"
                            }

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "LEN"; color: "#555555"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: deck._trackDuration !== "" ? deck._trackDuration : "--:--"; color: deck._trackDuration !== "" ? "#888888" : "#333333"; font.pixelSize: window.spViewport(10); font.bold: true; font.family: "monospace" }
                            }
                            Rectangle { anchors.left: parent.left; height: parent.height; width: 1; color: "#0a0a0a" }
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#0a0a0a" }

            // ── Overview waveform ─────────────────────────────────────────
            OverallWaveform {
                engine: deck.engine
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                stripeColor: "#0d0d0d"
            }

            AnalysisProgressBar {
                Layout.fillWidth: true
                Layout.preferredHeight: 4
                engine: deck.engine
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#0a0a0a" }

            // ── Transport / loop controls ─────────────────────────────────
            RowLayout {
                id: deckControlsCol
                Layout.fillWidth: true
                Layout.minimumHeight: deckControlsCol.controlsHeight
                Layout.preferredHeight: deckControlsCol.controlsHeight
                Layout.maximumHeight: deckControlsCol.controlsHeight
                spacing: 0

                property real unit: Math.max(24, window.spViewport(26))
                property real controlsHeight: deck.btnH * 2 + 1

                // PLAY — primary action, spans both control rows.
                FlatBtn {
                    btnText: deck.engine && deck.engine.isPlaying ? "Ⅱ" : "▶"
                    fbPreferredHeight: deckControlsCol.controlsHeight
                    fbFontPx: 18
                    fbFontFamily: "sans-serif"
                    Layout.preferredWidth: deckControlsCol.unit * 1.45
                    Layout.minimumWidth: 34
                    fbActive: deck.engine ? deck.engine.isPlaying : false
                    fbAccent: deck.accentGrn
                    fbActiveText: deck.accentGrn
                    fbInactiveText: "#e8e8e8"
                    onClicked: {
                        if (!deck.engine) return
                        if (performancePads.consumeHotCueHoldPlayLatch()) return
                        deck.engine.togglePlay()
                    }
                }

                Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#0a0a0a" }

                // CUE — momentary transport button, spans both control rows.
                FlatBtn {
                    btnText: "CUE"
                    fbPreferredHeight: deckControlsCol.controlsHeight
                    fbFontPx: 10
                    Layout.preferredWidth: deckControlsCol.unit * 1.0
                    Layout.minimumWidth: 28
                    fbInactiveText: "#888888"
                    onBtnPressed:  { if (deck.engine) deck.engine.cueButtonPress() }
                    onBtnReleased: { if (deck.engine) deck.engine.cueButtonRelease() }
                }

                Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#0a0a0a" }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumHeight: deckControlsCol.controlsHeight
                    Layout.preferredHeight: deckControlsCol.controlsHeight
                    Layout.maximumHeight: deckControlsCol.controlsHeight
                    spacing: 0

                    // Row 1 — Transport
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.minimumHeight: deck.btnH
                        Layout.preferredHeight: deck.btnH
                        Layout.maximumHeight: deck.btnH
                        color: "#181818"

                        RowLayout {
                            anchors.fill: parent
                            spacing: 1

                            FlatBtn {
                                btnText: "REV"
                                Layout.preferredWidth: deckControlsCol.unit
                                fbActive: deck.engine ? deck.engine.isReverse : false
                                fbAccent: "#ff6600"
                                fbActiveText: "#ff6600"
                                onClicked: { if (deck.engine) deck.engine.setReverse(!deck.engine.isReverse) }
                            }

                            FlatBtn {
                                btnText: "SYNC"
                                Layout.preferredWidth: deckControlsCol.unit * 1.2
                                fbActive: deck.engine ? deck.engine.syncEnabled : false
                                fbAccent: deck.engine && deck.engine.syncMaster ? "#ffd24d" : deck.accentGrn
                                fbActiveText: fbAccent
                                onClicked: {
                                    if (!deck.engine) return
                                    if (deck.engine.syncEnabled) deck.engine.reSync()
                                    else deck.engine.setSyncEnabled(true)
                                }
                                onRightClicked: { if (deck.engine) deck.engine.setSyncEnabled(false) }
                            }

                            FlatBtn {
                                btnText: "LINK"
                                Layout.preferredWidth: deckControlsCol.unit * 1.1
                                fbActive: deck.linkMode
                                fbAccent: "#3de87a"
                                fbActiveText: "#3de87a"
                                fbInactiveText: deck.linkAvailable ? "#666666" : "#444444"
                                onClicked: {
                                    if (!deck.linkAvailable) { deck._setLinkMode(false); return }
                                    deck._setLinkMode(!deck.linkMode)
                                    if (deck.linkMode) deck._publishDeckToAbletonLink()
                                }
                            }

                            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: deck.btnH; color: "#0a0a0a" }

                            FlatBtn {
                                btnText: "Q"
                                Layout.preferredWidth: deckControlsCol.unit * 0.85
                                fbActive: deck.engine ? deck.engine.quantizeEnabled : false
                                onClicked: { if (deck.engine) deck.engine.quantizeEnabled = !deck.engine.quantizeEnabled }
                            }

                            FlatBtn {
                                btnText: "KL"
                                Layout.preferredWidth: deckControlsCol.unit * 0.85
                                fbActive: deck.engine ? deck.engine.keylock : false
                                onClicked: { if (deck.engine) deck.engine.keylock = !deck.engine.keylock }
                            }

                            FlatBtn {
                                btnText: "SLIP"
                                Layout.preferredWidth: deckControlsCol.unit
                                fbActive: deck.engine ? deck.engine.slipActive : false
                                onClicked: { if (deck.engine) deck.engine.setSlip(!deck.engine.slipActive) }
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.minimumHeight: 1
                        Layout.preferredHeight: 1
                        Layout.maximumHeight: 1
                        color: "#0a0a0a"
                    }

                    // Row 2 — Loop
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.minimumHeight: deck.btnH
                        Layout.preferredHeight: deck.btnH
                        Layout.maximumHeight: deck.btnH
                        color: "#181818"

                        RowLayout {
                            anchors.fill: parent
                            spacing: 1

                            FlatBtn {
                                btnText: "L IN"
                                Layout.preferredWidth: deckControlsCol.unit * 1.1
                                Layout.minimumWidth: 36
                                onClicked: { if (deck.engine) deck.engine.setLoopIn() }
                            }

                            FlatBtn {
                                btnText: "L OUT"
                                Layout.preferredWidth: deckControlsCol.unit * 1.2
                                Layout.minimumWidth: 40
                                onClicked: { if (deck.engine) deck.engine.setLoopOut() }
                            }

                            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: deck.btnH; color: "#0a0a0a" }

                            FlatBtn {
                                btnText: "<"
                                Layout.preferredWidth: deckControlsCol.unit * 0.75
                                Layout.minimumWidth: 22
                                onClicked: { if (deck.engine) deck.engine.halveLoopLength() }
                            }

                            FlatBtn {
                                btnText: deck.loopLabel()
                                Layout.preferredWidth: deckControlsCol.unit * 2.0
                                Layout.minimumWidth: 54
                                fbActive: deck.engine ? deck.engine.loopActive : false
                                fbAccent: deck.accentGrn
                                fbActiveText: deck.accentGrn
                                onClicked: { if (deck.engine) deck.engine.toggleLoop4Beats() }
                            }

                            FlatBtn {
                                btnText: ">"
                                Layout.preferredWidth: deckControlsCol.unit * 0.75
                                Layout.minimumWidth: 22
                                onClicked: { if (deck.engine) deck.engine.doubleLoopLength() }
                            }

                            FlatBtn {
                                btnText: "3/4"
                                Layout.preferredWidth: deckControlsCol.unit * 0.95
                                Layout.minimumWidth: 30
                                fbActive: deck.engine ? (deck.engine.loopActive && Math.abs(deck.engine.loopLengthBeats - 0.75) < 0.06) : false
                                fbAccent: deck.accentBlu
                                fbActiveText: deck.accentBlu
                                onClicked: { if (deck.engine) deck.engine.toggleLoopThreeQuarter() }
                            }

                            FlatBtn {
                                btnText: "LOOP"
                                Layout.preferredWidth: deckControlsCol.unit * 1.3
                                Layout.minimumWidth: 38
                                readonly property bool _hasLoop: deck.engine ? deck.engine.loopInPosition < deck.engine.loopOutPosition : false
                                opacity: _hasLoop ? 1.0 : 0.35
                                enabled: _hasLoop
                                fbActive: deck.engine ? deck.engine.loopActive : false
                                fbAccent: deck.accentGrn
                                fbActiveText: deck.accentGrn
                                onClicked: {
                                    if (!deck.engine) return
                                    if (deck.engine.loopActive)
                                        deck.engine.deactivateLoop()
                                    else
                                        deck.engine.reactivateLoop()
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.minimumHeight: 1
                Layout.preferredHeight: 1
                Layout.maximumHeight: 1
                color: "#0a0a0a"
            }

            // ── Performance pads + tempo fader ────────────────────────────
            Item {
                id: padsArea
                Layout.fillWidth: true
                Layout.fillHeight: true

                RowLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: Math.max(72, Math.round(padsArea.height * 2 / 3))
                    spacing: 0

                    PerformancePads {
                        id: performancePads
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deck.engine
                        accentColor: deck.accent
                    }

                    Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#0a0a0a" }

                    Rectangle {
                        id: tempoPanel
                        Layout.preferredWidth: 56
                        Layout.fillHeight: true
                        color: "#181818"

                        property real tempoRange: deck.engine ? deck.engine.tempoRangePercent : 8

                        Rectangle {
                            anchors.left: parent.left
                            height: parent.height
                            width: 1
                            color: "#0a0a0a"
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 3
                            spacing: 2

                            Rectangle {
                                id: tempoHeader
                                Layout.fillWidth: true
                                Layout.preferredHeight: 18
                                color: "transparent"

                                Row {
                                    anchors.centerIn: parent; spacing: 3
                                    Text { text: "TEMPO"; color: "#555555"; font.pixelSize: window.spViewport(7); font.bold: true; font.letterSpacing: 0.6; font.family: "monospace"; anchors.verticalCenter: parent.verticalCenter }
                                    Text { text: "▾"; color: "#666666"; font.pixelSize: window.spViewport(8); anchors.verticalCenter: parent.verticalCenter }
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: tempoRangePopup.visible = !tempoRangePopup.visible
                                }
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#222222" }
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
                                color: tempoSlider.value > 0 ? "#ffaa00"
                                     : tempoSlider.value < 0 ? "#888888"
                                     : "#333333"
                                font.pixelSize: window.spViewport(9); font.bold: true; font.family: "monospace"
                            }
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
            color: "#181818"
            border.color: "#333333"
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
                        { label: "16%",  value: 16  },
                        { label: "32%",  value: 32  },
                        { label: "WIDE", value: 100 }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool isActive: tempoPanel.tempoRange === modelData.value
                        width: rangeCol.width
                        height: 18
                        radius: 0
                        color: isActive ? "#1a1a2a" : "#181818"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 1
                            color: isActive ? deck.accent : "#222222"
                        }

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: isActive ? deck.accent : "#484848"
                            font.pixelSize: window.spViewport(8)
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
                Text { text: "MANUAL BPM"; color: "#555555"; font.pixelSize: window.spViewport(8); font.bold: true; font.family: "monospace" }
                Controls.TextField {
                    id: manualBpmField
                    width: parent.width; height: 28
                    placeholderText: "e.g. 124.50"
                    color: "#e8e8e8"
                    placeholderTextColor: "#484848"
                    selectionColor: deck.accentGrn
                    selectedTextColor: "#181818"
                    font.pixelSize: window.spViewport(10); font.bold: true; font.family: "monospace"
                    background: Rectangle {
                        color: "#1e1e1e"
                        radius: 0
                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 1
                            color: manualBpmField.activeFocus ? deck.accentGrn : "#333333"
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
                Text { text: "Enter to apply"; color: "#484848"; font.pixelSize: window.spViewport(7); font.family: "monospace" }
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
