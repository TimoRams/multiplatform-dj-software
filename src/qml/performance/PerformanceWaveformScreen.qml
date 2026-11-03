import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DJSoftware

Item {
    id: root
    clip: true
    property var deckAEngine: null
    property var deckBEngine: null
    property var fx: null
    property real waveformZoom: 0.22
    property string selectedDeck: "A"
    property string leftPanel: "deck" // closed | deck | grid
    property bool rightPanelOpen: false
    property bool panelStateReady: false
    Component.onCompleted: {
        if (typeof settingsManager !== "undefined" && settingsManager)
            rightPanelOpen = settingsManager.getUiState("performanceFxPanelOpen", "0") === "1"
        panelStateReady = true
    }
    onRightPanelOpenChanged: {
        if (panelStateReady && typeof settingsManager !== "undefined" && settingsManager)
            settingsManager.setUiState("performanceFxPanelOpen", rightPanelOpen ? "1" : "0")
    }

    readonly property real panelWidth: UiTheme.px(wideLayout ? 232 : 208)
    readonly property real handleWidth: UiTheme.touchTarget
    readonly property bool wideLayout: UiTheme.layoutProfile(width,
        Window.window ? Window.window.height : height, UiTheme.scale) === "wide"
    readonly property bool dockPanels: wideLayout && width - 2 * panelWidth - 2 * handleWidth >= UiTheme.px(900)
    property real leftPanelReveal: leftPanel === "closed" ? 0.0 : 1.0
    property real rightPanelReveal: rightPanelOpen ? 1.0 : 0.0
    readonly property var selectedEngine: selectedDeck === "A" ? deckAEngine : deckBEngine
    readonly property real renderDpr: {
        var value = Screen.devicePixelRatio
        return isFinite(value) && value > 0 ? value : 1.0
    }
    readonly property real separatorHeight:
        Math.max(1.0 / renderDpr, Math.round(2.0 * renderDpr) / renderDpr)
    readonly property real deckAHeight:
        Math.floor(Math.max(0, height - separatorHeight) * renderDpr * 0.5) / renderDpr
    readonly property real deckBY: deckAHeight + separatorHeight

    function toggleDeckPanel() { leftPanel = leftPanel === "deck" ? "closed" : "deck" }
    function openGrid() { leftPanel = "grid" }
    function closeLeftPanel() { leftPanel = "closed" }

    component PerformanceText: Text {
        font.family: UiTheme.uiFontFamily
        renderType: Text.QtRendering
    }

    component PerformanceScroll: Flickable {
        clip: true
        contentWidth: width
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
    }

    component PerformanceActionButton: Button {
        required property string label
        required property real rowHeight
        required property color accent
        property bool active: false
        text: label
        accentColor: accent
        emphasized: active
        labelSize: 12
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(UiTheme.touchTarget, rowHeight)
        Layout.minimumHeight: Math.max(UiTheme.touchTarget, rowHeight)
        ToolTip.visible: hovered
        ToolTip.delay: 600
        ToolTip.text: label
    }

    component PerformanceLevelDial: Item {
        id: dial
        property real amount: 0
        property color accent: UiTheme.blue
        property bool engaged: false
        signal edited(real amount)
        onAmountChanged: if (dialArc) dialArc.requestPaint()
        onEngagedChanged: if (dialArc) dialArc.requestPaint()
        onAccentChanged: if (dialArc) dialArc.requestPaint()
        Canvas {
            id: dialArc
            anchors.fill: parent
            antialiasing: true
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const cx = width / 2, cy = height / 2
                const radius = Math.max(1, Math.min(width, height) / 2 - 5)
                const start = Math.PI * 0.75, span = Math.PI * 1.5
                const end = start + span * Math.max(0, Math.min(1, dial.amount))
                ctx.lineWidth = 3; ctx.lineCap = "round"
                ctx.strokeStyle = UiTheme.borderStrong
                ctx.beginPath(); ctx.arc(cx, cy, radius, start, start + span); ctx.stroke()
                ctx.strokeStyle = dial.engaged ? dial.accent : UiTheme.textSecondary
                ctx.beginPath(); ctx.arc(cx, cy, radius, start, end); ctx.stroke()
                ctx.fillStyle = ctx.strokeStyle
                ctx.beginPath(); ctx.arc(cx + Math.cos(end) * radius, cy + Math.sin(end) * radius, 4, 0, Math.PI * 2); ctx.fill()
            }
        }
        Column {
            anchors.centerIn: parent
            spacing: 3
            PerformanceText {
                anchors.horizontalCenter: parent.horizontalCenter
                text: Math.round(dial.amount * 100) + "%"
                color: UiTheme.textPrimary
                font.pixelSize: Math.max(12, Math.min(32, dial.width * 0.23))
                font.weight: Font.Medium
            }
            PerformanceText {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: dial.width >= 96
                text: "LEVEL / DEPTH"; color: UiTheme.textLabel
                font.pixelSize: 9; font.letterSpacing: 0.5
            }
        }
        MouseArea {
            objectName: "fxLevelDialInput"
            anchors.fill: parent
            cursorShape: Qt.SizeVerCursor
            preventStealing: true
            property real startY: 0
            property real startAmount: 0
            onPressed: (mouse) => { startY = mouse.y; startAmount = dial.amount }
            onPositionChanged: (mouse) => {
                if (pressed) dial.edited(Math.max(0, Math.min(1, startAmount + (startY - mouse.y) / 150)))
            }
            onDoubleClicked: dial.edited(0)
            onWheel: (wheel) => {
                dial.edited(Math.max(0, Math.min(1, dial.amount + wheel.angleDelta.y / 120 * 0.025)))
                wheel.accepted = true
            }
        }
    }

    component PerformanceBeatgridPanel: Rectangle {
        id: root

        property var engine: null
        property string deckName: "A"
        signal closeRequested()
        implicitHeight: gridContent.implicitHeight + 2

        readonly property int headerHeight: 32
        readonly property int rowHeight: 32
        readonly property int panelMargin: 10
        readonly property color accentColor: UiTheme.deckColor(deckName)

        color: UiTheme.inspectorSurface
        border.color: UiTheme.borderSubtle
        border.width: 1
        radius: 0



        PerformanceScroll {
            id: gridScroll
            objectName: "gridScroll"
            anchors.fill: parent
            anchors.margins: 1
            contentHeight: gridContent.height
        }
        ColumnLayout {
            id: gridContent
            parent: gridScroll.contentItem
            width: gridScroll.width
            height: implicitHeight
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: root.headerHeight
                Layout.minimumHeight: root.headerHeight
                color: Qt.rgba(1, 1, 1, 0.06)
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 2
                    color: root.accentColor
                }
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "‹"
                    color: UiTheme.textSecondary; font.pixelSize: 24
                }
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 36
                    anchors.right: parent.right; anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "BEATGRID  ·  DECK " + root.deckName
                    color: UiTheme.textPrimary; font.pixelSize: 12; font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }
                MouseArea { anchors.fill: parent; onClicked: root.closeRequested() }
            }

            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: root.rowHeight
                Layout.minimumHeight: root.rowHeight
                Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin
                PerformanceText { text: "TEMPO"; color: UiTheme.textLabel; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.5 }
                Item { Layout.fillWidth: true }
                PerformanceText {
                    Layout.maximumWidth: Math.max(56, parent.width - 82)
                    text: root.engine && root.engine.trackData && root.engine.trackData.isBpmAnalyzed
                          ? root.engine.trackData.bpm.toFixed(2) + " BPM" : "BPM —"
                    color: UiTheme.textPrimary; font.pixelSize: 18
                    elide: Text.ElideRight; horizontalAlignment: Text.AlignRight
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: UiTheme.borderSubtle }

            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: root.panelMargin; Layout.rightMargin: root.panelMargin
                Layout.topMargin: root.panelMargin
                columns: 2
                columnSpacing: 4; rowSpacing: 4
                PerformanceActionButton {
                    rowHeight: root.rowHeight
                    accent: root.accentColor; label: "÷2 BPM"; onClicked: if (root.engine) root.engine.halveBpm() }
                PerformanceActionButton {
                    rowHeight: root.rowHeight
                    accent: root.accentColor; label: "×2 BPM"; onClicked: if (root.engine) root.engine.doubleBpm() }
                PerformanceActionButton {
                        objectName: "gridNudgeMinusBeat"
                        rowHeight: root.rowHeight
                        accent: root.accentColor; label: "− 1 BEAT"; onClicked: if (root.engine) root.engine.nudgeBeatgridBeats(-1) }
                PerformanceActionButton {
                    rowHeight: root.rowHeight
                    accent: root.accentColor; label: "+ 1 BEAT"; onClicked: if (root.engine) root.engine.nudgeBeatgridBeats(1) }
                PerformanceActionButton {
                    rowHeight: root.rowHeight
                    accent: root.accentColor; label: "− 10 ms"; onClicked: if (root.engine) root.engine.nudgeBeatgridMs(-10) }
                PerformanceActionButton {
                    rowHeight: root.rowHeight
                    accent: root.accentColor; label: "+ 10 ms"; onClicked: if (root.engine) root.engine.nudgeBeatgridMs(10) }
                PerformanceActionButton {
                    rowHeight: root.rowHeight
                    accent: root.accentColor; label: "SET DOWNBEAT"; onClicked: if (root.engine) root.engine.setDownbeatAtCurrentPosition() }
                PerformanceActionButton {
                    objectName: "gridLockButton"
                    rowHeight: root.rowHeight
                    accent: root.accentColor
                    label: root.engine && root.engine.beatgridLocked ? "GRID LOCKED" : "LOCK GRID"
                    active: root.engine && root.engine.beatgridLocked
                    onClicked: if (root.engine) root.engine.beatgridLocked = !root.engine.beatgridLocked
                }
            }

        }
    }

    component PerformanceDeckQuickPanel: Rectangle {
        id: root

        property var engine: null
        property string deckName: "A"
        property bool selected: false
        signal selectedRequested()
        signal gridRequested()
        implicitHeight: deckContent.implicitHeight + 2 * verticalPadding + 2

        readonly property int horizontalPadding: 5
        readonly property int verticalPadding: 4
        readonly property real contentScale: Math.min(1, Math.max(1, deckScroll.height - 2 * verticalPadding)
                                                        / Math.max(1, deckContent.implicitHeight))
        readonly property int rowSpacing: 2
        readonly property int bpmTenths: engine && engine.currentBpm > 0
                                        ? Math.round(engine.currentBpm * 10) : 0
        readonly property color panelText: UiTheme.textPrimary
        readonly property color mutedText: UiTheme.textSecondary
        readonly property color lineColor: UiTheme.borderSubtle
        readonly property color rowColor: UiTheme.inspectorInset
        readonly property color accentColor: UiTheme.deckColor(deckName)
        readonly property color playingColor: UiTheme.play
        readonly property bool externalSourceTrack: engine && engine.hasTrack
                                                    && engine.readOnlyExternalTrack
        readonly property color sourceColor: externalSourceTrack ? UiTheme.blue : UiTheme.textSecondary

        // Ejecting mid-playback would cut the output, so the button only arms once
        // the deck is stopped — paused, or sitting at the end of the track.
        readonly property bool canEject: engine && engine.hasTrack && !engine.isPlaying

        color: UiTheme.inspectorSurface
        border.color: lineColor
        border.width: 1
        radius: 0

        function loadedSourceLabel() {
            if (!engine || !engine.hasTrack)
                return "NO SOURCE"
            if (!engine.readOnlyExternalTrack)
                return "LOCAL"

            var sourceId = engine.externalSourceId || ""
            if (typeof deviceLibraryManager !== "undefined" && deviceLibraryManager) {
                var devices = deviceLibraryManager.devices
                for (var index = 0; index < devices.length; ++index) {
                    var device = devices[index]
                    if (String(device.id) === String(sourceId))
                        return "USB " + (index + 1)
                               + (device.name ? " · " + device.name : "")
                }
            }
            return "USB"
        }

        PerformanceScroll {
            id: deckScroll
            objectName: "deckScroll"
            anchors.fill: parent
            anchors.margins: 1
            contentHeight: height
            interactive: false
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
        }
        ColumnLayout {
            id: deckContent
            parent: deckScroll.contentItem
            x: root.horizontalPadding
            y: root.verticalPadding
            transformOrigin: Item.TopLeft
            scale: root.contentScale
            width: Math.max(0, deckScroll.width - 2 * root.horizontalPadding) / scale
            height: Math.max(1, deckScroll.height - 2 * root.verticalPadding) / scale
            spacing: root.rowSpacing

            // ── Deck header ─────────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                Layout.minimumHeight: 24
                color: "transparent"

                Rectangle {
                    id: deckChip
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 7
                    width: 3; height: 18
                    opacity: root.selected ? 1.0 : 0.4
                    color: root.accentColor
                }

                PerformanceText {
                    anchors.left: deckChip.right
                    anchors.leftMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: "DECK " + root.deckName
                    color: root.panelText
                    font.pixelSize: 12; font.weight: Font.DemiBold; font.letterSpacing: 0.8
                }

                PerformanceText {
                    id: deckState
                    anchors.right: ejectButton.left
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.engine && root.engine.isPlaying ? "PLAYING" : "READY"
                    color: root.engine && root.engine.isPlaying ? root.playingColor : root.mutedText
                    font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 0.6
                }

                // Eject sits directly beside the state caption, where the player is
                // reporting that it is not currently playing anything.
                Item {
                    id: ejectButton
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24; height: 24
                    opacity: root.canEject ? 1.0 : 0.28

                    Canvas {
                        id: ejectGlyph
                        anchors.centerIn: parent
                        width: 13; height: 13
                        property color glyphColor: ejectArea.containsMouse && root.canEject
                                                   ? UiTheme.blue : root.panelText
                        onGlyphColorChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.reset()
                            ctx.fillStyle = glyphColor
                            ctx.beginPath()
                            ctx.moveTo(width * 0.5, 0)
                            ctx.lineTo(width, height * 0.62)
                            ctx.lineTo(0, height * 0.62)
                            ctx.closePath()
                            ctx.fill()
                            ctx.fillRect(0, height * 0.79, width, height * 0.21)
                        }
                    }

                    MouseArea {
                        id: ejectArea
                        objectName: "deckEjectArea"
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: root.canEject
                        onClicked: if (root.engine) root.engine.ejectTrack()
                    }
                }

                MouseArea {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.right: ejectButton.left
                    onClicked: root.selectedRequested()
                }
            }

            Rectangle {
                id: trackDisplay
                objectName: "sidebarTrackDisplay"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: 42
                Layout.minimumHeight: 42
                color: "transparent"
                Column {
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: coverShell.visible ? 6 : 1
                    Item {
                        id: coverShell
                        width: parent.width
                        height: Math.min(112, Math.max(0, trackDisplay.height - 68))
                        visible: trackDisplay.height >= 124
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: parent.height; height: width; radius: 0
                            color: root.rowColor
                            border.color: root.lineColor
                            Rectangle {
                                anchors.centerIn: parent
                                width: parent.width * 0.6; height: width; radius: width / 2
                                color: "transparent"; border.color: UiTheme.borderStrong
                                Rectangle {
                                    anchors.centerIn: parent; width: 6; height: 6; radius: width / 2
                                    color: root.mutedText
                                }
                            }
                            Image {
                                anchors.fill: parent
                                source: root.visible && coverShell.visible && root.engine
                                        ? (root.engine.coverArtUrl || "") : ""
                                sourceSize: Qt.size(256, 256)
                                asynchronous: true; cache: true
                                fillMode: Image.PreserveAspectFit
                            }
                        }
                    }
                    PerformanceText {
                        width: parent.width
                        objectName: "deckTrackTitle"
                        text: root.engine && root.engine.hasTrack
                              ? (root.engine.trackTitle || "Untitled track") : "No track loaded"
                        color: root.panelText
                        font.pixelSize: 13; font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                        maximumLineCount: trackDisplay.height >= 124 ? 2 : 1
                        elide: Text.ElideRight
                    }
                    PerformanceText {
                        width: parent.width
                        objectName: "deckTrackArtist"
                        text: root.engine && root.engine.hasTrack
                              ? (root.engine.trackArtist || "Unknown artist") : "Load a track to begin"
                        color: root.mutedText
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                    PerformanceText {
                        width: parent.width
                        text: root.loadedSourceLabel()
                        color: UiTheme.textLabel
                        font.pixelSize: 8; font.letterSpacing: 0.5
                        elide: Text.ElideRight
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.selectedRequested()
                }
            }

            // ── BPM ─────────────────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: Math.min(52, Math.max(32, root.height * 0.13))
                Layout.minimumHeight: 32
                color: root.rowColor
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: -7
                    text: "BPM"; color: UiTheme.textLabel
                    font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.6
                }
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 7
                    width: 65
                    text: "KEY  " + (root.engine && root.engine.trackKey ? root.engine.trackKey : "—")
                    color: root.mutedText; font.pixelSize: 10
                    elide: Text.ElideRight
                }
                // Split so the decimal stays small, the way a player prints a tempo.
                PerformanceText {
                    id: bpmDecimals
                    objectName: "bpmDecimals"
                    anchors.right: parent.right; anchors.rightMargin: 9
                    anchors.baseline: bpmWhole.baseline
                    text: root.bpmTenths > 0 ? "." + (root.bpmTenths % 10) : ".-"
                    color: root.panelText; font.pixelSize: 12
                }
                PerformanceText {
                    id: bpmWhole
                    objectName: "bpmWhole"
                    anchors.right: bpmDecimals.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.bpmTenths > 0 ? Math.floor(root.bpmTenths / 10).toString() : "---"
                    color: root.panelText; font.pixelSize: 22; font.weight: Font.Medium
                }
                MouseArea { anchors.fill: parent; onClicked: root.selectedRequested() }
            }

            Rectangle {
                objectName: "bpmSeparator"
                Layout.fillWidth: true
                Layout.minimumHeight: 1
                Layout.preferredHeight: 1
                Layout.maximumHeight: 1
                color: root.lineColor
            }

            // ── BEAT JUMP ───────────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 28
                Layout.minimumHeight: 28
                color: root.rowColor
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: jumpMinus.left; anchors.rightMargin: 6
                    text: "JUMP"; color: UiTheme.textLabel; lineHeight: 0.95
                    font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 0.6
                    elide: Text.ElideRight
                }
                Rectangle {
                    id: jumpMinus
                    objectName: "beatJumpMinus"
                    anchors.right: jumpValue.left; anchors.rightMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24; height: 24
                    color: UiTheme.surface; border.color: root.lineColor; border.width: 1
                    PerformanceText { anchors.centerIn: parent; text: "−"; color: root.panelText; font.pixelSize: 13 }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (root.engine)
                            root.engine.beatJumpBeats = Math.max(0.5, root.engine.beatJumpBeats / 2)
                    }
                }
                PerformanceText {
                    id: jumpValue
                    anchors.right: jumpPlus.left; anchors.rightMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30
                    height: 24
                    text: root.engine ? root.engine.beatJumpBeats.toString() : "4"; color: root.panelText
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    MouseArea { anchors.fill: parent; onClicked: if (root.engine) root.engine.beatJump(root.engine.beatJumpBeats) }
                }
                Rectangle {
                    id: jumpPlus
                    anchors.right: parent.right; anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24; height: 24
                    color: UiTheme.surface; border.color: root.lineColor; border.width: 1
                    PerformanceText { anchors.centerIn: parent; text: "+"; color: root.panelText; font.pixelSize: 13 }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (root.engine)
                            root.engine.beatJumpBeats = Math.min(64, root.engine.beatJumpBeats * 2)
                    }
                }
            }

            // ── Quantize ────────────────────────────────────────────────────────
            Rectangle {
                objectName: "quantizeRow"
                Layout.fillWidth: true; Layout.preferredHeight: 26
                Layout.minimumHeight: 26
                color: root.rowColor
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: "QUANTIZE"
                    color: root.engine && root.engine.quantizeEnabled ? root.accentColor : root.mutedText
                    font.pixelSize: 12; font.weight: Font.DemiBold
                }
                PerformanceText {
                    anchors.right: parent.right; anchors.rightMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.engine && root.engine.quantizeEnabled ? "ON" : "OFF"
                    color: root.engine && root.engine.quantizeEnabled ? root.panelText : root.mutedText
                    font.pixelSize: 12; font.weight: Font.DemiBold
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: if (root.engine) root.engine.quantizeEnabled = !root.engine.quantizeEnabled
                }
            }
        }
    }

    component PerformanceBeatFxPanel: Rectangle {
        id: root
        property var fx: null
        signal closeRequested()
        implicitHeight: fxContent.implicitHeight + 2 * verticalPadding + 2

        readonly property int horizontalPadding: 5
        readonly property int verticalPadding: 4
        readonly property real contentScale: Math.min(1, Math.max(1, fxScroll.height - 2 * verticalPadding)
                                                        / Math.max(1, fxContent.implicitHeight))
        readonly property int headerHeight: 26
        readonly property color panelText: UiTheme.textPrimary
        readonly property color mutedText: UiTheme.textSecondary
        readonly property color lineColor: UiTheme.borderSubtle
        readonly property color rowColor: UiTheme.inspectorInset
        readonly property color activeColor: UiTheme.blue
        readonly property color accentColor: UiTheme.blue

        readonly property bool expandedMixControl: height >= 400
        readonly property bool percentParameter: fx && fx.effectType1 === "Reverb"
        readonly property bool timingRelevant: fx && ["Echo", "Low Cut Echo", "MT Delay", "Multi-Tap Delay",
            "Spiral", "Flanger", "Phaser", "Trans", "Tremolo", "Enigma Jet", "Roll", "Slip Roll",
            "Roll Out", "Mobius Saw", "Mobius Tri"].indexOf(fx.effectType1) >= 0
        readonly property var beatDivisions: [
            { label: "1/16", value: 0.0625 }, { label: "1/8", value: 0.125 },
            { label: "1/4", value: 0.25 }, { label: "1/2", value: 0.5 },
            { label: "1", value: 1.0 }, { label: "2", value: 2.0 }, { label: "4", value: 4.0 }
        ]
        readonly property var divisions: percentParameter
            ? fx.reverbSizeSteps.map(function(value) {
                return { label: Math.round(value * 100) + "%", value: value }
            }) : beatDivisions
        readonly property real currentDiv: fx
            ? (percentParameter ? fx.primaryParam1 : fx.beatDiv1) : 0.25
        readonly property int currentDivIndex: {
            var index = 0
            var nearest = Number.MAX_VALUE
            for (var i = 0; i < divisions.length; ++i) {
                var distance = Math.abs(currentDiv - divisions[i].value)
                if (distance < nearest) { nearest = distance; index = i }
            }
            return index
        }
        readonly property real effectMs: {
            var bpm = fx && fx.displayBpm1 > 0 ? fx.displayBpm1 : 0
            return bpm > 0 ? divisions[currentDivIndex].value * 60000 / bpm : 0
        }
        readonly property bool routedA: fx ? fx.deck1A : false
        readonly property bool routedB: fx ? fx.deck1B : false
        // The unit's own engage flag, not a guess derived from the mix amount.
        // Reading it off wetDry1 meant a mix of zero — the resting position of the
        // hardware LEVEL knob — looked identical to "off", so pressing ON appeared
        // to do nothing.
        readonly property bool effectOn: fx && fx.effectType1 !== "---" && fx.enabled1

        color: UiTheme.inspectorSurface
        border.color: lineColor
        border.width: 1
        radius: 0

        readonly property var effectOptions: ["Echo", "Reverb", "Flanger", "Roll", "Phaser", "---"]

        function selectNextEffect() {
            if (!fx) return
            var index = effectOptions.indexOf(fx.effectType1)
            fx.setEffectType(1, effectOptions[(index + 1) % effectOptions.length])
        }

        // ON needs a selected effect: raising the mix on a slot still set to "---"
        // is silent, so the button would look dead. The mix amount itself is left
        // alone — switching back on restores whatever the user had dialled in.
        function toggleEffect() {
            if (!fx) return
            if (effectOn) {
                fx.setUnitEnabled(1, false)
                return
            }
            if (fx.effectType1 === "---")
                fx.setEffectType(1, effectOptions[0])
            fx.setUnitEnabled(1, true)
        }

        function setDivisionIndex(index) {
            if (!fx) return
            index = Math.max(0, Math.min(divisions.length - 1, index))
            if (percentParameter)
                fx.setPrimaryParam(1, divisions[index].value)
            else
                fx.setBeatDivision(1, divisions[index].value)
        }

        // Dragging the mix is a request to hear the effect, so it engages the unit
        // explicitly. The amount itself never engages anything on its own — a
        // hardware knob streams its resting position and would switch FX on unasked.
        function setMix(amount) {
            if (!fx) return
            amount = Math.max(0, Math.min(1, amount))
            fx.setWetDry(1, amount)
            if (amount > 0.001 && !fx.enabled1) {
                if (fx.effectType1 === "---")
                    fx.setEffectType(1, effectOptions[0])
                fx.setUnitEnabled(1, true)
            }
        }

        PerformanceScroll {
            id: fxScroll
            objectName: "fxScroll"
            anchors.fill: parent
            anchors.margins: 1
            contentHeight: height
            interactive: false
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
        }
        ColumnLayout {
            id: fxContent
            parent: fxScroll.contentItem
            x: root.horizontalPadding
            y: root.verticalPadding
            transformOrigin: Item.TopLeft
            scale: root.contentScale
            width: Math.max(0, fxScroll.width - 2 * root.horizontalPadding) / scale
            height: Math.max(1, fxScroll.height - 2 * root.verticalPadding) / scale
            spacing: 2

            // ── Section header ──────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: root.headerHeight
                Layout.minimumHeight: root.headerHeight
                color: "transparent"
                PerformanceText {
                    anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: "BEAT FX"; color: root.panelText
                    font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 1.0
                }
                PerformanceText {
                    anchors.right: parent.right; anchors.rightMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: "×"; color: root.mutedText; font.pixelSize: 16
                }
                MouseArea {
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    width: 24; height: 24
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closeRequested()
                }
            }

            // ── Selected effect ─────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 42
                Layout.minimumHeight: 42
                color: root.rowColor
                radius: 0
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.top: parent.top; anchors.topMargin: 3
                    text: "EFFECT"
                    color: UiTheme.textLabel; font.pixelSize: 9; font.letterSpacing: 1
                }
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: parent.right; anchors.rightMargin: 26
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                    text: root.fx && root.fx.effectType1 !== "---" ? root.fx.effectType1 : "Select effect"
                    color: root.panelText
                    font.pixelSize: 17; font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                PerformanceText {
                    anchors.right: parent.right; anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "›"; color: root.mutedText; font.pixelSize: 22
                }
                MouseArea { anchors.fill: parent; onClicked: root.selectNextEffect() }
            }

            // ── Tempo the effect is locked to ───────────────────────────────────
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 32
                Layout.minimumHeight: 32
                objectName: "fxTempoSyncRow"
                opacity: root.timingRelevant ? 1.0 : 0.5
                color: root.rowColor

                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: "TEMPO"; color: UiTheme.textLabel
                    font.pixelSize: 9; font.letterSpacing: 0.5
                }
                PerformanceText {
                    id: fxBpmDecimals
                    anchors.right: bpmBadge.left; anchors.rightMargin: 6
                    anchors.baseline: fxBpmWhole.baseline
                    text: root.fx && root.fx.displayBpm1 > 0
                          ? "." + (Math.round(root.fx.displayBpm1 * 10) % 10) : ".-"
                    color: root.panelText; font.pixelSize: 13
                }
                PerformanceText {
                    id: fxBpmWhole
                    anchors.right: fxBpmDecimals.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.fx && root.fx.displayBpm1 > 0
                          ? Math.floor(root.fx.displayBpm1).toString() : "---"
                    color: root.panelText; font.pixelSize: 20; font.weight: Font.Medium
                }

                // AUTO/MAN over the BPM caption, the way a player labels the tempo
                // source right next to the number it is following.
                Column {
                    id: bpmBadge
                    anchors.right: parent.right; anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1
                    Rectangle {
                        width: 34; height: 14
                        color: root.fx && root.fx.syncEnabled1 ? Qt.rgba(root.activeColor.r, root.activeColor.g, root.activeColor.b, 0.16) : UiTheme.surface
                        border.color: root.fx && root.fx.syncEnabled1 ? root.activeColor : root.lineColor
                        border.width: 1
                        PerformanceText {
                            anchors.centerIn: parent
                            text: root.fx && root.fx.syncEnabled1 ? "AUTO" : "MAN"
                            color: root.fx && root.fx.syncEnabled1 ? root.activeColor : root.mutedText
                            font.pixelSize: 9; font.weight: Font.DemiBold
                        }
                    }
                    PerformanceText {
                        width: 34; horizontalAlignment: Text.AlignHCenter
                        text: "BPM"; color: UiTheme.textLabel
                        font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 0.4
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: if (root.fx) root.fx.setSyncEnabled(1, !root.fx.syncEnabled1)
                }
            }

            // ── Beat division: previous / current / next, all directly selectable ─
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 26
                Layout.minimumHeight: 26
                Layout.maximumHeight: 26
                spacing: 4
                Layout.leftMargin: 0
                Layout.rightMargin: 0
                Layout.topMargin: 0
                Layout.bottomMargin: 0
                Repeater {
                    model: 3
                    Rectangle {
                        required property int index
                        readonly property int divIndex: root.currentDivIndex + index - 1
                        readonly property bool current: index === 1
                        readonly property bool available: divIndex >= 0 && divIndex < root.divisions.length
                        Layout.fillWidth: true; Layout.fillHeight: true
                        color: current ? Qt.rgba(root.activeColor.r, root.activeColor.g, root.activeColor.b, 0.16) : root.rowColor
                        border.color: current ? root.activeColor : root.lineColor
                        border.width: 1
                        PerformanceText {
                            anchors.centerIn: parent
                            text: available ? root.divisions[divIndex].label : ""
                            color: current ? root.activeColor : root.mutedText
                            font.pixelSize: current ? 14 : 12
                            font.weight: current ? Font.DemiBold : Font.Normal
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: available && !current
                            onClicked: root.setDivisionIndex(divIndex)
                        }
                    }
                }
            }

            // ── Resulting effect time ───────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 26
                Layout.minimumHeight: 26
                color: root.rowColor
                PerformanceText {
                    anchors.left: parent.left; anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.percentParameter ? "ROOM SIZE" : "TIME"; color: UiTheme.textLabel
                    font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 0.6
                }
                PerformanceText {
                    id: timeUnit
                    anchors.right: parent.right; anchors.rightMargin: 9
                    anchors.baseline: timeValue.baseline
                    text: root.percentParameter ? "%" : "ms"; color: root.mutedText; font.pixelSize: 10
                }
                PerformanceText {
                    id: timeValue
                    objectName: "fxParameterValue"
                    anchors.right: timeUnit.left; anchors.rightMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.percentParameter ? Math.round(root.currentDiv * 100).toString()
                          : (root.effectMs > 0 ? Math.round(root.effectMs).toString() : "---")
                    color: root.panelText; font.pixelSize: 17
                }
            }

            Item {
                objectName: "fxLevelDisplay"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 0
                Layout.preferredHeight: 0
                PerformanceLevelDial {
                    objectName: "fxLevelDial"
                    anchors.centerIn: parent
                    width: Math.max(0, Math.min(parent.width - 20, parent.height - 12, 168))
                    height: width
                    visible: root.expandedMixControl && width >= 56
                    amount: root.fx ? root.fx.wetDry1 : 0
                    accent: root.activeColor
                    engaged: root.effectOn
                    onEdited: (amount) => root.setMix(amount)
                }
            }

            PerformanceText {
                Layout.fillWidth: true
                Layout.preferredHeight: 16
                Layout.minimumHeight: 16
                visible: !root.expandedMixControl
                text: "LEVEL / DEPTH"
                verticalAlignment: Text.AlignVCenter
                color: UiTheme.textLabel
                font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.8
            }

            Rectangle {
                id: mixBar
                objectName: "fxMixBar"
                visible: !root.expandedMixControl
                Layout.fillWidth: true; Layout.preferredHeight: 30
                Layout.minimumHeight: 30
                Layout.leftMargin: 0
                Layout.rightMargin: 0
                color: UiTheme.surfaceInset
                border.color: root.lineColor
                border.width: 1

                Rectangle {
                    anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                    anchors.margins: 1
                    width: Math.max(0, (parent.width - 2) * (root.fx ? root.fx.wetDry1 : 0))
                    color: root.effectOn ? Qt.rgba(root.activeColor.r, root.activeColor.g, root.activeColor.b, 0.32) : UiTheme.surfaceRaised
                }
                PerformanceText {
                    anchors.centerIn: parent
                    text: root.fx ? Math.round(root.fx.wetDry1 * 100) + " %" : "0 %"
                    color: root.panelText; font.pixelSize: 15; font.weight: Font.Medium
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    onPressed: (mouse) => root.setMix(mouse.x / width)
                    onPositionChanged: (mouse) => root.setMix(mouse.x / width)
                }
            }

            // ── Routing and engage ──────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 26
                Layout.minimumHeight: 26
                Layout.maximumHeight: 26
                spacing: 4
                Layout.leftMargin: 0
                Layout.rightMargin: 0
                Layout.topMargin: 0
                Layout.bottomMargin: 0
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: root.routedA ? Qt.rgba(UiTheme.deckA.r, UiTheme.deckA.g, UiTheme.deckA.b, 0.16) : root.rowColor
                    border.color: root.routedA ? UiTheme.deckA : root.lineColor
                    border.width: 1
                    PerformanceText {
                        anchors.centerIn: parent; text: "DECK A"
                        color: root.routedA ? UiTheme.deckA : root.mutedText
                        font.pixelSize: 11; font.weight: Font.DemiBold
                    }
                    MouseArea { anchors.fill: parent; onClicked: if (root.fx) root.fx.setDeckAssignment(1, 1, !root.routedA) }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: root.routedB ? Qt.rgba(UiTheme.deckB.r, UiTheme.deckB.g, UiTheme.deckB.b, 0.16) : root.rowColor
                    border.color: root.routedB ? UiTheme.deckB : root.lineColor
                    border.width: 1
                    PerformanceText {
                        anchors.centerIn: parent; text: "DECK B"
                        color: root.routedB ? UiTheme.deckB : root.mutedText
                        font.pixelSize: 11; font.weight: Font.DemiBold
                    }
                    MouseArea { anchors.fill: parent; onClicked: if (root.fx) root.fx.setDeckAssignment(1, 2, !root.routedB) }
                }
            }

            PerformanceActionButton {
                objectName: "fxQuantizeRow"
                opacity: root.timingRelevant ? 1.0 : 0.5
                Layout.leftMargin: 0
                Layout.rightMargin: 0
                Layout.bottomMargin: 2
                rowHeight: 28
                label: "FX QUANTIZE  " + (active ? "ON" : "OFF")
                accent: root.activeColor
                active: root.fx ? root.fx.quantizeEnabled1 : false
                enabled: root.fx !== null
                onClicked: if (root.fx) root.fx.setQuantizeEnabled(1, !root.fx.quantizeEnabled1)
                ToolTip.text: "Align effect starts and modulation to the beatgrid. Enables BPM sync."
                Accessible.role: Accessible.CheckBox
                Accessible.name: "FX Quantize"
                Accessible.checkable: true
                Accessible.checked: active
                Accessible.onPressAction: clicked()
            }

            Rectangle {
                objectName: "fxEngageRow"
                Layout.fillWidth: true; Layout.preferredHeight: 30
                Layout.minimumHeight: 30
                Layout.leftMargin: 0
                Layout.rightMargin: 0
                Layout.bottomMargin: 2
                color: root.effectOn ? Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.16) : root.rowColor
                border.color: root.effectOn ? root.accentColor : root.lineColor
                border.width: 1
                PerformanceText {
                    anchors.centerIn: parent
                    text: root.effectOn ? "EFFECT ON" : "ENABLE EFFECT"
                    color: root.effectOn ? root.accentColor : root.mutedText
                    font.pixelSize: 13; font.weight: Font.DemiBold; font.letterSpacing: 0.8
                }
                MouseArea { anchors.fill: parent; onClicked: root.toggleEffect() }
            }
        }
    }

    Behavior on leftPanelReveal {
        NumberAnimation { duration: UiTheme.motionPanel; easing.type: Easing.OutCubic }
    }
    Behavior on rightPanelReveal {
        NumberAnimation { duration: UiTheme.motionPanel; easing.type: Easing.OutCubic }
    }

    // Fill the handle gutters as well as the waveform viewport when docked.
    Rectangle {
        anchors.fill: parent
        color: UiTheme.displayBackground
    }

    // The capture source contains only waveforms: never the panels or their blur.
    Item {
        id: waveformBacking
        objectName: "waveformBacking"
        anchors.fill: parent
        anchors.leftMargin: root.dockPanels ? (root.panelWidth + root.handleWidth) * root.leftPanelReveal : 0
        anchors.rightMargin: root.dockPanels ? (root.panelWidth + root.handleWidth) * root.rightPanelReveal : 0
        Rectangle { anchors.fill: parent; color: UiTheme.displayBackground }

        EnlargedWaveform {
            x: 0
            y: 0
            width: waveformBacking.width
            height: root.deckAHeight
            deckName: "A"
            engine: root.deckAEngine
            backgroundColor: UiTheme.displayBackground
            waveformZoom: root.waveformZoom
            showBeatgridEditor: false
        }

        Rectangle {
            x: 0
            y: root.deckAHeight
            width: waveformBacking.width
            height: root.separatorHeight
            color: UiTheme.borderSubtle
        }

        EnlargedWaveform {
            x: 0
            y: root.deckBY
            width: waveformBacking.width
            height: Math.max(0, root.height - y)
            deckName: "B"
            engine: root.deckBEngine
            backgroundColor: UiTheme.displayBackground
            waveformZoom: root.waveformZoom
            showBeatgridEditor: false
        }
    }

    // Waveform context controls stay independent from both side panels.
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        spacing: 3
        z: 20
        Button {
            objectName: "waveformZoomOut"
            width: UiTheme.touchTarget; height: UiTheme.touchTarget
            text: "−"
            labelSize: 18
            Accessible.name: "Zoom out"
            onClicked: if (waveformZoomController) waveformZoomController.zoomOut()
        }
        Rectangle {
            width: UiTheme.px(56); height: UiTheme.touchTarget; radius: 0; color: UiTheme.surface; border.color: UiTheme.borderSubtle; border.width: 1

            // Where the current zoom sits in the whole range, so the level is
            // readable at a glance without parsing the number.
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 1
                height: 2
                color: UiTheme.surfaceInset
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: parent.width * (waveformZoomController
                                           ? waveformZoomController.zoomFraction : 0)
                    color: UiTheme.blue
                }
            }

            // A positioner ignores anchors on its children, so the two lines are
            // centred by filling the column width instead.
            Column {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: -1
                width: parent.width
                spacing: 0
                PerformanceText {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "ZOOM"; color: UiTheme.textLabel
                    font.pixelSize: 9; font.weight: Font.DemiBold
                }
                PerformanceText {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: waveformZoomController ? waveformZoomController.zoomLabel : "–"
                    color: UiTheme.textPrimary
                    font.pixelSize: 11; font.weight: Font.DemiBold
                }
            }
        }
        Button {
            objectName: "waveformZoomIn"
            width: UiTheme.touchTarget; height: UiTheme.touchTarget
            text: "+"
            labelSize: 18
            Accessible.name: "Zoom in"
            onClicked: if (waveformZoomController) waveformZoomController.zoomIn()
        }
        Rectangle {
            width: UiTheme.px(56); height: UiTheme.touchTarget; radius: 0
            readonly property color deckAccent: UiTheme.deckColor(root.selectedDeck)
            color: root.leftPanel === "grid" ? Qt.rgba(deckAccent.r, deckAccent.g, deckAccent.b, 0.16) : UiTheme.surface
            border.color: root.leftPanel === "grid" ? UiTheme.deckColor(root.selectedDeck) : UiTheme.borderSubtle
            border.width: 1
            PerformanceText {
                anchors.centerIn: parent; text: "GRID"
                color: root.leftPanel === "grid" ? UiTheme.deckColor(root.selectedDeck) : UiTheme.textPrimary
                font.pixelSize: 10; font.weight: Font.DemiBold
            }
            MouseArea { anchors.fill: parent; onClicked: root.openGrid() }
        }
    }

    Item {
        id: leftHost
        width: root.panelWidth
        anchors.top: parent.top; anchors.bottom: parent.bottom
        x: -width * (1.0 - root.leftPanelReveal)
        visible: root.leftPanelReveal > 0.001
        z: 30

        PerformanceBackdrop {
            objectName: "leftBackdrop"
            frameVisible: false
            anchors.fill: parent
            sourceItem: waveformBacking
            sourceOrigin: Qt.point(leftHost.x, leftHost.y)
            requested: root.leftPanel !== "closed" && !root.dockPanels
        }
        PerformanceBeatgridPanel {
            id: gridPanel
            objectName: "beatgridPanelInstance"
            width: parent.width; height: parent.height
            visible: root.leftPanel === "grid"
            engine: root.selectedEngine
            deckName: root.selectedDeck
            onCloseRequested: root.closeLeftPanel()
        }

        Item {
            anchors.fill: parent
            visible: root.leftPanel === "deck"
            Item {
                anchors.fill: parent
                PerformanceDeckQuickPanel {
                    id: quickPanelA
                    objectName: "deckQuickPanelA"
                    width: parent.width; height: root.deckAHeight
                    engine: root.deckAEngine; deckName: "A"; selected: root.selectedDeck === "A"
                    onSelectedRequested: root.selectedDeck = "A"
                    onGridRequested: root.openGrid()
                }
                Rectangle {
                    width: parent.width; y: root.deckAHeight
                    height: root.separatorHeight; color: UiTheme.borderSubtle
                }
                PerformanceDeckQuickPanel {
                    id: quickPanelB
                    objectName: "deckQuickPanelB"
                    width: parent.width; y: root.deckBY
                    height: Math.max(0, root.height - y)
                    engine: root.deckBEngine; deckName: "B"; selected: root.selectedDeck === "B"
                    onSelectedRequested: root.selectedDeck = "B"
                    onGridRequested: root.openGrid()
                }
            }
        }
    }

    Rectangle {
        id: leftHandle
        width: root.handleWidth; height: UiTheme.touchTarget
        anchors.left: root.leftPanel === "closed" ? parent.left : leftHost.right
        anchors.top: parent.top
        anchors.topMargin: 4
        color: UiTheme.panelRaised; border.color: UiTheme.borderSubtle; border.width: 1; radius: 0; z: 32
        PerformanceText { anchors.centerIn: parent; text: root.leftPanel === "closed" ? "›" : "‹"; color: UiTheme.textSecondary; font.pixelSize: 15 }
        Accessible.role: Accessible.Button
        Accessible.name: root.leftPanel === "closed" ? "Show deck information" : "Hide deck information"
        Accessible.onPressAction: root.toggleDeckPanel()
        MouseArea { anchors.fill: parent; onClicked: root.toggleDeckPanel() }
    }

    Item {
        id: rightHost
        width: root.panelWidth
        anchors.top: parent.top
        height: root.height
        x: root.width - width * root.rightPanelReveal
        visible: root.rightPanelReveal > 0.001
        z: 30
        PerformanceBackdrop {
            objectName: "rightBackdrop"
            frameVisible: false
            anchors.fill: parent
            sourceItem: waveformBacking
            sourceOrigin: Qt.point(rightHost.x, rightHost.y)
            requested: root.rightPanelOpen && !root.dockPanels
        }
        PerformanceBeatFxPanel {
            id: fxPanel
            objectName: "beatFxPanelInstance"
            anchors.fill: parent
            fx: root.fx
            onCloseRequested: root.rightPanelOpen = false
        }
    }

    Rectangle {
        id: rightHandle
        width: root.handleWidth; height: UiTheme.touchTarget
        anchors.right: root.rightPanelOpen ? rightHost.left : parent.right
        anchors.top: parent.top
        anchors.topMargin: 4
        color: UiTheme.panelRaised; border.color: UiTheme.borderSubtle; border.width: 1; radius: 0; z: 32
        PerformanceText { anchors.centerIn: parent; text: root.rightPanelOpen ? "›" : "‹"; color: UiTheme.textSecondary; font.pixelSize: 15 }
        Accessible.role: Accessible.Button
        Accessible.name: root.rightPanelOpen ? "Hide effects" : "Show effects"
        Accessible.onPressAction: root.rightPanelOpen = !root.rightPanelOpen
        MouseArea { anchors.fill: parent; onClicked: root.rightPanelOpen = !root.rightPanelOpen }
    }
}
