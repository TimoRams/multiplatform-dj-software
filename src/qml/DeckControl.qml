import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

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
    readonly property int headerCellHeight: 24
    property double _linkSuppressPublishUntilMs: 0
    property double _linkFollowBlockedUntilMs: 0
    property double _lastLinkPublishMs: 0
    property double _lastLinkPublishBpm: 0

    // ── Theme ────────────────────────────────────────────────────────────
    readonly property color accent:    deckName === "A" ? "#ff9900" : "#00ccff"
    readonly property color accentGrn: "#4dd98a"
    readonly property color accentBlu: "#5bb6ff"
    readonly property int   btnH:      26

    // ── Reusable flat button component ───────────────────────────────────
    component FlatBtn: Rectangle {
        id: fb
        required property string btnText
        property bool   fbActive:           false
        property color  fbActiveColor:      "#1a2e1a"
        property color  fbActiveTxtColor:   deck.accentGrn
        property color  fbInactiveColor:    "#1e1e1e"
        property color  fbInactiveTxtColor: "#888"

        Layout.preferredHeight: deck.btnH
        Layout.minimumHeight:   deck.btnH
        Layout.maximumHeight:   deck.btnH

        radius: 2
        color: hovH.hovered ? "#2c2c2c" : (fbActive ? fbActiveColor : fbInactiveColor)

        HoverHandler { id: hovH }

        Text {
            anchors.centerIn: parent
            text: fb.btnText
            color: fb.fbActive ? fb.fbActiveTxtColor : fb.fbInactiveTxtColor
            font.pixelSize: window.spViewport(9)
            font.bold: fb.fbActive
            font.letterSpacing: 0.4
            font.family: "monospace"
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        signal clicked()
        signal btnPressed()
        signal btnReleased()

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: fb.clicked()
            onPressed: fb.btnPressed()
            onReleased: fb.btnReleased()
        }
    }

    // ── Tempo slider component ───────────────────────────────────────────
    component DeckSlider: Slider {
        id: control
        property bool centerFill: false

        implicitWidth:  orientation === Qt.Vertical ? 22 : 150
        implicitHeight: orientation === Qt.Vertical ? 150 : 22

        background: Rectangle {
            x: control.orientation === Qt.Horizontal ? control.leftPadding : control.width / 2 - 2
            y: control.orientation === Qt.Horizontal ? control.height / 2 - 2 : control.topPadding
            width:  control.orientation === Qt.Horizontal ? control.availableWidth  : 4
            height: control.orientation === Qt.Horizontal ? 4 : control.availableHeight
            radius: 1
            color: "#181818"

            Rectangle {
                visible: control.orientation === Qt.Vertical && !control.centerFill
                x: 1
                y: parent.height - 1 - Math.max(0, (1.0 - control.visualPosition) * (parent.height - 2))
                width: parent.width - 2
                height: Math.max(0, (1.0 - control.visualPosition) * (parent.height - 2))
                radius: 0
                color: control.pressed ? "#5a5a5a" : "#3e3e3e"
            }

            Rectangle {
                visible: control.orientation === Qt.Vertical && control.centerFill
                x: 1
                width: parent.width - 2
                radius: 0
                color: control.pressed ? "#5a5a5a" : "#3e3e3e"

                readonly property real midPy: parent.height / 2
                readonly property real posPy: 1 + control.visualPosition * (parent.height - 2)

                y: Math.min(midPy, posPy)
                height: Math.max(0, Math.abs(posPy - midPy))
            }
        }

        handle: Rectangle {
            implicitWidth:  control.orientation === Qt.Vertical ? 26 : 18
            implicitHeight: control.orientation === Qt.Vertical ? 10 : 22
            x: control.orientation === Qt.Horizontal
               ? control.leftPadding + control.visualPosition * (control.availableWidth - width)
               : control.width / 2 - width / 2
            y: control.orientation === Qt.Horizontal
               ? control.height / 2 - height / 2
               : control.topPadding + control.visualPosition * (control.availableHeight - height)
            radius: 2
            color: control.pressed ? "#e8e8e8" : "#c8c8c8"
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
            var expectedId = "deck" + deck.deckName + "_play"
            if (id === expectedId) {
                if (value > 0.5 && !deck.engine.isPlaying)
                    deck.engine.togglePlay()
                else if (value <= 0.5 && deck.engine.isPlaying)
                    deck.engine.togglePlay()
            }
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
        color: "#131313"

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Track-info header ──────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: deck.headerCellHeight * 2
                color: "#0f0f0f"

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
                        Layout.preferredWidth: deck.headerCellHeight * 2
                        Layout.minimumWidth:   deck.headerCellHeight * 2
                        Layout.maximumWidth:   deck.headerCellHeight * 2
                        Layout.fillHeight: true
                        color: "#0d0d0d"

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
                            color: "#0d1a0d"

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
                            color: "#0d0d1c"

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
                            color: "#0d0d1c"

                            Row { anchors.fill: parent; anchors.leftMargin: 5; spacing: 4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "KEY"; color: "#3a3a6a"; font.pixelSize: window.spViewport(7); font.bold: true; font.family: "monospace" }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: deck._trackKey !== "" ? deck._trackKey : "--"; color: deck._trackKey !== "" ? deck.accentBlu : "#444464"; font.pixelSize: window.spViewport(11); font.bold: true; font.family: "monospace" }
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: 82; Layout.preferredHeight: deck.headerCellHeight
                            color: "#151515"

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
                Layout.preferredHeight: 34
                stripeColor: "#0d0d0d"
            }

            SegmentBar {
                Layout.fillWidth: true
                Layout.preferredHeight: 6
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
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    // PLAY
                    FlatBtn {
                        btnText: "PLAY"
                        Layout.preferredWidth: deckControlsCol.unit * 1.5
                        fbActive: deck.engine ? deck.engine.isPlaying : false
                        fbActiveColor: "#0d280d"; fbActiveTxtColor: deck.accentGrn
                        fbInactiveTxtColor: "#666"
                        onClicked: { if (deck.engine) deck.engine.togglePlay() }
                    }

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
                        onClicked: { if (deck.engine) deck.engine.setSyncEnabled(!deck.engine.syncEnabled) }
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

                    // Group separator
                    Rectangle { Layout.preferredWidth: 3; Layout.preferredHeight: deck.btnH; color: "#0a0a0a" }

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
                        fbActive: false
                        fbActiveColor: deck.deckName === "A" ? "#2a1e00" : "#002233"
                        fbActiveTxtColor: deck.accent
                    }

                    Item { Layout.fillWidth: true }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: "#161616" }

                // Row 2 — Loop
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    // L IN
                    FlatBtn {
                        btnText: "L IN"
                        Layout.preferredWidth: deckControlsCol.unit * 1.1
                        Layout.minimumWidth: 36
                        onClicked: { if (deck.engine) deck.engine.setLoopIn() }
                    }

                    // L OUT
                    FlatBtn {
                        btnText: "L OUT"
                        Layout.preferredWidth: deckControlsCol.unit * 1.2
                        Layout.minimumWidth: 40
                        onClicked: { if (deck.engine) deck.engine.setLoopOut() }
                    }

                    // Group separator
                    Rectangle { Layout.preferredWidth: 3; Layout.preferredHeight: deck.btnH; color: "#0a0a0a" }

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

                    Item { Layout.fillWidth: true }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#1c1c1c" }

            // ── Performance pads + tempo fader ────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                PerformancePads {
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
                    color: "#111111"

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
                            TapHandler {
                                onDoubleTapped: {
                                    tempoSlider.enabled = false; tempoSlider.value = 0; tempoSlider.enabled = true
                                    if (deck.engine) deck.engine.setTempoPercent(0)
                                }
                            }
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
                TextField {
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
