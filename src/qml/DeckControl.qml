import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls

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
    readonly property color accent:    deckName === "A" ? "#ff9900"
                                     : deckName === "B" ? "#00ccff"
                                     : deckName === "C" ? "#cc44ff"
                                     : "#44ddaa"
    readonly property color accentGrn: "#4dd98a"
    readonly property color accentBlu: "#5bb6ff"
    readonly property int   btnH:      22

    // ── Reusable flat button component ───────────────────────────────────
    component FlatBtn: Rectangle {
        id: fb
        required property string btnText
        property bool   fbActive:           false
        property color  fbActiveColor:      "#0d2018"
        property color  fbActiveTxtColor:   deck.accentGrn
        property color  fbInactiveColor:    "#1e1e1e"
        property color  fbInactiveTxtColor: "#666666"

        Layout.preferredHeight: deck.btnH
        Layout.minimumHeight:   deck.btnH
        Layout.maximumHeight:   deck.btnH

        radius: 0
        color: fbMouse.containsMouse ? "#252525" : (fbActive ? fbActiveColor : fbInactiveColor)

        // Bottom accent line
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left:   parent.left
            anchors.right:  parent.right
            height: 1
            color:  fb.fbActive ? fb.fbActiveTxtColor : "#333333"
            opacity: fb.fbActive ? 1.0 : (fbMouse.containsMouse ? 0.5 : 0.0)
        }

        signal clicked()
        signal rightClicked()
        signal btnPressed()
        signal btnReleased()

        Text {
            anchors.centerIn:    parent
            text:                fb.btnText
            color:               fb.fbActive ? fb.fbActiveTxtColor : fb.fbInactiveTxtColor
            font.pixelSize:      window.spViewport(9)
            font.bold:           fb.fbActive
            font.letterSpacing:  0.4
            font.family:         "monospace"
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
        color: "#111111"

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Track-info header ──────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: deck.headerCellHeight * 2
                color: "#0a0a0a"

                RowLayout {
                    anchors.fill: parent
                    spacing: 0

                    // Deck accent strip
                    Rectangle {
                        Layout.preferredWidth: 4; Layout.fillHeight: true
                        color: deck.accent
                        opacity: deck._hasTrack ? 1.0 : 0.3
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

                    Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#1c1c1c" }

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
                                color: deck._hasTrack ? "#ebebeb" : "#555"
                                font.pixelSize: window.spViewport(10)
                                font.bold: true
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "#1e1e1e" }

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
                                color: deck._hasTrack ? "#9a9a9a" : "#555"
                                font.pixelSize: window.spViewport(9)
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                    // Meta badges 2×2
                    GridLayout {
                        id: metaBadges
                        columns: 2; rowSpacing: 0; columnSpacing: 0
                        Layout.preferredWidth: 164; Layout.fillHeight: true

                        Rectangle {
                            id: bpmBadge
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#0a180a"

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "BPM"; color: "#3a6a3a"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: deck._trackBpm !== "" ? deck._trackBpm : "--"; color: deck._trackBpm !== "" ? deck.accentGrn : "#4a604a"; font.pixelSize: window.spViewport(11); font.bold: true; font.family: "monospace" }
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
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
                        }

                        Rectangle {
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#0a0a1a"

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "LIVE"; color: "#3a3a6a"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: deck._currentBpm !== "" ? deck._currentBpm : "--"
                                    color: {
                                        if (!deck._showLiveBpmIndicator()) return "#444464"
                                        if (!deck.engine) return deck.accentBlu
                                        return deck.engine.tempoPercent > 0 ? "#ffaa00" : deck.accentBlu
                                    }
                                    font.pixelSize: window.spViewport(11); font.bold: true; font.family: "monospace"
                                }
                            }
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
                            Rectangle { anchors.left: parent.left; height: parent.height; width: 1; color: "#1c1c1c" }
                        }

                        Rectangle {
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#0a0a1a"

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "KEY"; color: "#3a3a6a"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: deck._trackKey !== "" ? deck._trackKey : "--"; color: deck._trackKey !== "" ? deck.accentBlu : "#444464"; font.pixelSize: window.spViewport(11); font.bold: true; font.family: "monospace" }
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#111111"

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "LEN"; color: "#444"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: deck._trackDuration !== "" ? deck._trackDuration : "--:--"; color: deck._trackDuration !== "" ? "#b0b0b0" : "#555"; font.pixelSize: window.spViewport(11); font.bold: true; font.family: "monospace" }
                            }
                            Rectangle { anchors.left: parent.left; height: parent.height; width: 1; color: "#1c1c1c" }
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

            // ── Overview waveform ─────────────────────────────────────────
            OverallWaveform {
                engine: deck.engine
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                stripeColor: "#0d0d0d"
            }

            SegmentBar {
                Layout.fillWidth: true
                Layout.preferredHeight: 4
                segments: deck.engine ? deck.engine.currentSegments : []
                totalTrackDuration: deck.engine ? deck.engine.trackDurationSec : 0
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

            // ── Transport / loop controls ─────────────────────────────────
            ColumnLayout {
                id: deckControlsCol
                Layout.fillWidth: true
                spacing: 0

                property real unit: Math.max(26, window.spViewport(28))

                // Row 1 — Transport
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: deck.btnH
                    color: "#181818"   // intentionally slightly elevated from deck bg

                    RowLayout {
                        anchors.fill: parent
                        spacing: 1

                        // PLAY — primary action, brighter inactive state
                        FlatBtn {
                            btnText: "PLAY"
                            Layout.preferredWidth: deckControlsCol.unit * 1.6
                            fbActive: deck.engine ? deck.engine.isPlaying : false
                            fbActiveColor: "#0d2a0d"
                            fbActiveTxtColor: deck.accentGrn
                            fbInactiveColor: "#202020"
                            fbInactiveTxtColor: "#aaa"
                            onClicked: {
                                if (!deck.engine) return
                                if (performancePads.consumeHotCueHoldPlayLatch()) return
                                deck.engine.togglePlay()
                            }
                        }

                        // Thin accent divider after PLAY
                        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: deck.btnH; color: "#282828" }

                        // CUE
                        FlatBtn {
                            btnText: "CUE"
                            Layout.preferredWidth: deckControlsCol.unit
                            onBtnPressed:  { if (deck.engine) deck.engine.cueButtonPress() }
                            onBtnReleased: { if (deck.engine) deck.engine.cueButtonRelease() }
                        }

                        // REV
                        FlatBtn {
                            btnText: "REV"
                            Layout.preferredWidth: deckControlsCol.unit
                            fbActive: deck.engine ? deck.engine.isReverse : false
                            fbActiveColor: "#2a1200"; fbActiveTxtColor: "#ff6600"
                            onClicked: { if (deck.engine) deck.engine.setReverse(!deck.engine.isReverse) }
                        }

                        // SYNC
                        FlatBtn {
                            btnText: "SYNC"
                            Layout.preferredWidth: deckControlsCol.unit * 1.2
                            fbActive: deck.engine ? deck.engine.syncEnabled : false
                            fbActiveColor: deck.engine && deck.engine.syncMaster ? "#2a2000" : "#0a2a0a"
                            fbActiveTxtColor: deck.engine && deck.engine.syncMaster ? "#ffd24d" : deck.accentGrn
                            onClicked: {
                                if (!deck.engine) return
                                if (deck.engine.syncEnabled) deck.engine.reSync()
                                else deck.engine.setSyncEnabled(true)
                            }
                            onRightClicked: { if (deck.engine) deck.engine.setSyncEnabled(false) }
                        }

                        // LINK
                        FlatBtn {
                            btnText: "LINK"
                            Layout.preferredWidth: deckControlsCol.unit * 1.1
                            fbActive: deck.linkMode
                            fbActiveColor: "#0a2a14"; fbActiveTxtColor: "#3de87a"
                            fbInactiveTxtColor: deck.linkAvailable ? "#777" : "#444"
                            onClicked: {
                                if (!deck.linkAvailable) { deck._setLinkMode(false); return }
                                deck._setLinkMode(!deck.linkMode)
                                if (deck.linkMode) deck._publishDeckToAbletonLink()
                            }
                        }

                        // Group divider
                        Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: deck.btnH; color: "#0d0d0d" }

                        // Q
                        FlatBtn {
                            btnText: "Q"
                            Layout.preferredWidth: deckControlsCol.unit * 0.85
                            fbActive: deck.engine ? deck.engine.quantizeEnabled : false
                            fbActiveColor: deck.deckName === "A" ? "#2a1e00" : "#002233"
                            fbActiveTxtColor: deck.accent
                            onClicked: { if (deck.engine) deck.engine.quantizeEnabled = !deck.engine.quantizeEnabled }
                        }

                        // KL
                        FlatBtn {
                            btnText: "KL"
                            Layout.preferredWidth: deckControlsCol.unit * 0.85
                            fbActive: deck.engine ? deck.engine.keylock : false
                            fbActiveColor: deck.deckName === "A" ? "#2a1e00" : "#002233"
                            fbActiveTxtColor: deck.accent
                            onClicked: { if (deck.engine) deck.engine.keylock = !deck.engine.keylock }
                        }

                        // SLIP
                        FlatBtn {
                            btnText: "SLIP"
                            Layout.preferredWidth: deckControlsCol.unit
                            fbActive: deck.engine ? deck.engine.slipActive : false
                            fbActiveColor: deck.deckName === "A" ? "#2a1e00" : "#002233"
                            fbActiveTxtColor: deck.accent
                            onClicked: { if (deck.engine) deck.engine.setSlip(!deck.engine.slipActive) }
                        }

                        Item { Layout.fillWidth: true }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: "#0e0e0e" }

                // Row 2 — Loop
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: deck.btnH
                    color: "#111111"

                    RowLayout {
                        anchors.fill: parent
                        spacing: 1

                        // L IN
                        FlatBtn {
                            btnText: "L IN"
                            Layout.preferredWidth: deckControlsCol.unit * 1.1
                            Layout.minimumWidth: 36
                            fbInactiveColor: "#1a1a1a"
                            onClicked: { if (deck.engine) deck.engine.setLoopIn() }
                        }

                        // L OUT
                        FlatBtn {
                            btnText: "L OUT"
                            Layout.preferredWidth: deckControlsCol.unit * 1.2
                            Layout.minimumWidth: 40
                            fbInactiveColor: "#1a1a1a"
                            onClicked: { if (deck.engine) deck.engine.setLoopOut() }
                        }

                        // Group divider
                        Rectangle { Layout.preferredWidth: 4; Layout.preferredHeight: deck.btnH; color: "#0d0d0d" }

                        // <
                        FlatBtn {
                            btnText: "<"
                            Layout.preferredWidth: deckControlsCol.unit * 0.75
                            Layout.minimumWidth: 22
                            onClicked: { if (deck.engine) deck.engine.halveLoopLength() }
                        }

                        // Loop toggle
                        FlatBtn {
                            btnText: deck.loopLabel()
                            Layout.preferredWidth: deckControlsCol.unit * 2.0
                            Layout.minimumWidth: 54
                            fbActive: deck.engine ? deck.engine.loopActive : false
                            fbActiveColor: "#0a2a0a"; fbActiveTxtColor: deck.accentGrn
                            onClicked: { if (deck.engine) deck.engine.toggleLoop4Beats() }
                        }

                        // >
                        FlatBtn {
                            btnText: ">"
                            Layout.preferredWidth: deckControlsCol.unit * 0.75
                            Layout.minimumWidth: 22
                            onClicked: { if (deck.engine) deck.engine.doubleLoopLength() }
                        }

                        // 3/4
                        FlatBtn {
                            btnText: "3/4"
                            Layout.preferredWidth: deckControlsCol.unit * 0.95
                            Layout.minimumWidth: 30
                            fbActive: deck.engine ? (deck.engine.loopActive && Math.abs(deck.engine.loopLengthBeats - 0.75) < 0.06) : false
                            fbActiveColor: "#001a2a"; fbActiveTxtColor: deck.accentBlu
                            onClicked: { if (deck.engine) deck.engine.toggleLoopThreeQuarter() }
                        }

                        // LOOP — re-enable / deactivate last loop region
                        FlatBtn {
                            btnText: "LOOP"
                            Layout.preferredWidth: deckControlsCol.unit * 1.3
                            Layout.minimumWidth: 38
                            readonly property bool _hasLoop: deck.engine ? deck.engine.loopInPosition < deck.engine.loopOutPosition : false
                            opacity: _hasLoop ? 1.0 : 0.35
                            enabled: _hasLoop
                            fbActive: deck.engine ? deck.engine.loopActive : false
                            fbActiveColor: "#0a2a0a"; fbActiveTxtColor: deck.accentGrn
                            fbInactiveColor: "#0d160d"
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

            Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

            // ── Performance pads + tempo fader ────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                    PerformancePads {
                        id: performancePads
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deck.engine
                        accentColor: deck.accent
                    }

                Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#1c1c1c" }

                Rectangle {
                    id: tempoPanel
                    Layout.preferredWidth: 56
                    Layout.fillHeight: true
                    color: "#0a0a0a"

                    property real tempoRange: 8

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 3
                        spacing: 2

                        Rectangle {
                            id: tempoHeader
                            Layout.fillWidth: true
                            Layout.preferredHeight: 20
                            color: "transparent"

                            Row {
                                anchors.centerIn: parent; spacing: 3
                                Text { text: "TEMPO"; color: "#bbb"; font.pixelSize: window.spViewport(7); font.bold: true; font.letterSpacing: 0.6; anchors.verticalCenter: parent.verticalCenter }
                                Text { text: "▾"; color: deck.accent; font.pixelSize: window.spViewport(9); anchors.verticalCenter: parent.verticalCenter }
                            }
                            MouseArea {
                                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: tempoRangePopup.visible = !tempoRangePopup.visible
                            }
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1c1c1c" }
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
                                 : tempoSlider.value < 0 ? deck.accentBlu
                                 : "#555"
                            font.pixelSize: window.spViewport(10); font.bold: true; font.family: "monospace"
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
            color: "#181818"; border.color: deck.accent; border.width: 1; radius: 0

            MouseArea { anchors.fill: parent; onClicked: mouse => mouse.accepted = true }

            Column {
                id: rangeCol
                anchors { top: parent.top; left: parent.left; right: parent.right; margins: 5 }
                spacing: 1

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
                        width: rangeCol.width; height: 18; radius: 0
                        color: tempoPanel.tempoRange === modelData.value
                               ? (deck.deckName === "A" ? "#2a1800" : "#00182a") : "transparent"
                        Text {
                            anchors.centerIn: parent; text: modelData.label
                            color: tempoPanel.tempoRange === modelData.value ? deck.accent : "#888"
                            font.pixelSize: window.spViewport(9)
                            font.bold: tempoPanel.tempoRange === modelData.value
                            font.family: "monospace"
                        }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                tempoPanel.tempoRange = modelData.value
                                var clamped = Math.max(-modelData.value, Math.min(modelData.value, tempoSlider.value))
                                tempoSlider.enabled = false; tempoSlider.value = clamped; tempoSlider.enabled = true
                                if (deck.engine) deck.engine.setTempoPercent(clamped)
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
            color: "#181818"; border.color: "#3a6a3a"; border.width: 1

            MouseArea { anchors.fill: parent; onClicked: mouse => mouse.accepted = true }

            Column {
                anchors.fill: parent; anchors.margins: 6; spacing: 6
                Text { text: "MANUAL BPM"; color: deck.accentGrn; font.pixelSize: window.spViewport(9); font.bold: true; font.family: "monospace" }
                Controls.TextField {
                    id: manualBpmField
                    width: parent.width; height: 30
                    placeholderText: "e.g. 124.50"; color: "#111"; placeholderTextColor: "#666"
                    selectionColor: "#4caf50"; selectedTextColor: "#fff"
                    font.pixelSize: window.spViewport(11); font.bold: true; font.family: "monospace"
                    background: Rectangle { color: "#f5f5f5"; border.color: "#4dd98a"; border.width: 1; radius: 0 }
                    onAccepted: applyManualBpm()
                    function applyManualBpm() {
                        var v = Number(text.replace(",", "."))
                        if (!deck.engine || isNaN(v) || v <= 0) return
                        deck.engine.setManualBpm(v); deck._syncBpm(); deck._syncTempo()
                        manualBpmPopup.visible = false
                    }
                }
                Text { text: "Enter to apply"; color: "#666"; font.pixelSize: window.spViewport(8); font.family: "monospace" }
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
