import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    color: UiTheme.bg0
    // The expanded tray deliberately paints below this fixed-height item as an
    // overlay.  Keeping it unclipped prevents the workspace from being moved.
    clip: false

    // Height is fully controlled by parent layout — no implicitHeight here.
    // The first row remains fixed while the pull-down quick-access tray opens.
    readonly property int collapsedHeight: UiMetrics.toolbarHeight

    // ── Sizing helpers ───────────────────────────────────────────────────────
    readonly property int btnH:    Math.max(1, root.collapsedHeight)
    readonly property int padH:    Math.max(7, Math.round(btnH * 0.25))   // inner horizontal pad
    readonly property int sepW:    1                                        // divider width

    // VU meter sizing
    readonly property int vuW:     Math.max(72, Math.round(btnH * 2.6))
    readonly property int vuSegH:  Math.max(3,  Math.round(btnH * 0.12))

    // Beat dot sizing
    readonly property int dotSz:   Math.max(5,  Math.round(btnH * 0.18))

    // Master dial
    readonly property int dialSz:  Math.max(15, Math.round(btnH * 0.54))

    // Deck colors
    readonly property color clrA:  UiTheme.deckA
    readonly property color clrB:  UiTheme.deckB

    // Accent
    readonly property color accentBlue: UiTheme.masterBlue

    // Typography — fixed (no window-height scaling; bar has fixed px height)
    function sp(px) { return px }

    // ── State ────────────────────────────────────────────────────────────────
    property string currentTime: "00:00"
    property real   totalLatencyMs: 0.0
    property var    latencyRows: []
    property var    audioPerfStats: ({})
    property int    beatUiTick: 0

    // ── Timers ───────────────────────────────────────────────────────────────
    Connections {
        target: (typeof controlClock !== "undefined") ? controlClock : null
        property int latencyDivider: 0
        function onHousekeepingTick() {
            var d = new Date()
            root.currentTime = d.getHours().toString().padStart(2,"0") + ":"
                             + d.getMinutes().toString().padStart(2,"0")
        }
        function onStatisticsTick() {
            if ((++latencyDivider % 3) === 0)
                root.refreshLatencyInfo()
        }
        function onLinkTick() { root.beatUiTick++ }
    }

    Component.onCompleted: {
        var d = new Date()
        root.currentTime = d.getHours().toString().padStart(2,"0") + ":"
                         + d.getMinutes().toString().padStart(2,"0")
        root.refreshLatencyInfo()
    }

    // ── Functions ────────────────────────────────────────────────────────────
    function refreshLatencyInfo() {
        if (!deckA || !deckA.latencyBreakdown) return
        var rows = deckA.latencyBreakdown()
        if (!rows || rows.length === 0) return
        latencyRows = rows
        audioPerfStats = deckA.audioPerformanceStats ? deckA.audioPerformanceStats() : ({})
        var sum = 0.0
        for (var i = 0; i < rows.length; ++i) {
            if (rows[i].countInTotal === false) continue
            var ms = Number(rows[i].ms)
            if (!isNaN(ms) && isFinite(ms)) sum += ms
        }
        totalLatencyMs = sum
    }

    function deckBeatInfo(engine) {
        var _tick = root.beatUiTick
        if (!engine || !engine.trackData || !engine.trackData.isBpmAnalyzed)
            return { valid: false, beatInBar: 0, barNumber: 0 }
        var bpm = Number(engine.trackData.bpm)
        if (!isFinite(bpm) || bpm <= 0)
            return { valid: false, beatInBar: 0, barNumber: 0 }
        var pos = engine.getPlayheadPositionAtomic()
        if (pos === undefined || isNaN(pos))
            return { valid: false, beatInBar: 0, barNumber: 0 }
        var sr = Number(engine.trackData.sampleRate)
        if (!isFinite(sr) || sr <= 0) sr = 44100.0
        var first = Number(engine.trackData.firstBeatSample) / sr
        var beatDur = 60.0 / bpm
        var beats = (pos - first) / beatDur
        var beatFloor = Math.floor(beats)
        return {
            valid: true,
            beatInBar: (((beatFloor % 4) + 4) % 4) + 1,
            barNumber: Math.floor(beatFloor / 4) + 1
        }
    }

    // ── Settings window ──────────────────────────────────────────────────────
    SettingsWindow { id: settingsWin }

    // ── Latency popup ────────────────────────────────────────────────────────
    Popup {
        id: latencyPopup
        parent: Overlay.overlay
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0
        background: Rectangle { color: "#0e0e0e"; border.color: "#222"; border.width: 1 }

        contentItem: Column {
            spacing: 0

            Rectangle {
                width: latencyPopup.width; height: 28
                color: "#161616"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    text: "LATENCY BREAKDOWN"
                    color: "#999"; font.pixelSize: root.sp(9); font.bold: true; font.letterSpacing: 0.6
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    text: root.totalLatencyMs.toFixed(1) + " ms"
                    color: "#9ecbff"; font.pixelSize: root.sp(9); font.family: "monospace"; font.bold: true
                }
            }

            Repeater {
                model: root.latencyRows
                Rectangle {
                    required property var modelData
                    required property int index
                    width: latencyPopup.width; height: 26
                    color: index % 2 === 0 ? "#121212" : "#151515"
                    Row {
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                        Text {
                            width: 170; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: modelData.countInTotal === false ? "#555" : "#aaa"
                            font.pixelSize: root.sp(8); elide: Text.ElideRight
                        }
                        Text {
                            width: 54; anchors.verticalCenter: parent.verticalCenter
                            text: Number(modelData.ms).toFixed(1) + " ms"
                            color: "#efefef"; font.pixelSize: root.sp(8); font.family: "monospace"
                            horizontalAlignment: Text.AlignRight
                        }
                        Text {
                            width: 66; anchors.verticalCenter: parent.verticalCenter
                            text: Number(modelData.samples).toString() + " smp"
                            color: "#666"; font.pixelSize: root.sp(8); font.family: "monospace"
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }

            Rectangle {
                width: latencyPopup.width; height: 28
                color: "#161616"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    text: "CALLBACK PROFILE"
                    color: "#999"; font.pixelSize: root.sp(9); font.bold: true; font.letterSpacing: 0.6
                }
            }

            Rectangle {
                width: latencyPopup.width; height: 28
                color: "#121212"
                Row {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                    Text {
                        width: 82; anchors.verticalCenter: parent.verticalCenter
                        text: "AVG"
                        color: "#777"; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: ((Number(root.audioPerfStats.callbackAverageUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                        color: "#efefef"; font.pixelSize: root.sp(8); font.family: "monospace"
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        width: 54; anchors.verticalCenter: parent.verticalCenter
                        text: "WORST"
                        color: "#777"; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: ((Number(root.audioPerfStats.callbackWorstUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                        color: "#efefef"; font.pixelSize: root.sp(8); font.family: "monospace"
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            Rectangle {
                width: latencyPopup.width; height: 28
                color: "#151515"
                Row {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                    Text {
                        width: 82; anchors.verticalCenter: parent.verticalCenter
                        text: "BUDGET"
                        color: "#777"; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: ((Number(root.audioPerfStats.callbackBudgetUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                        color: "#efefef"; font.pixelSize: root.sp(8); font.family: "monospace"
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        width: 54; anchors.verticalCenter: parent.verticalCenter
                        text: "XRUNS"
                        color: "#777"; font.pixelSize: root.sp(8); font.bold: true
                    }
                    Text {
                        width: 62; anchors.verticalCenter: parent.verticalCenter
                        text: Number(root.audioPerfStats.callbackOverruns) ? Number(root.audioPerfStats.callbackOverruns).toString() : "0"
                        color: Number(root.audioPerfStats.callbackOverruns) > 0 ? "#ff5f52" : "#efefef"
                        font.pixelSize: root.sp(8); font.family: "monospace"
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            Repeater {
                model: root.audioPerfStats.fxProfiles ? root.audioPerfStats.fxProfiles : []
                Rectangle {
                    required property var modelData
                    width: latencyPopup.width; height: 24
                    color: "#121212"
                    Row {
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                        Text {
                            width: 136; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: "#777"; font.pixelSize: root.sp(8); elide: Text.ElideRight
                        }
                        Text {
                            width: 62; anchors.verticalCenter: parent.verticalCenter
                            text: ((Number(modelData.averageUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                            color: "#efefef"; font.pixelSize: root.sp(8); font.family: "monospace"
                            horizontalAlignment: Text.AlignRight
                        }
                        Text {
                            width: 46; anchors.verticalCenter: parent.verticalCenter
                            text: "max"
                            color: "#555"; font.pixelSize: root.sp(8); font.bold: true
                        }
                        Text {
                            width: 62; anchors.verticalCenter: parent.verticalCenter
                            text: ((Number(modelData.worstUsec) || 0.0) / 1000.0).toFixed(3) + " ms"
                            color: "#efefef"; font.pixelSize: root.sp(8); font.family: "monospace"
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }
        }
    }

    // ── View menu popup ──────────────────────────────────────────────────────
    Popup {
        id: viewMenuPopup
        parent: Overlay.overlay
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0
        width: 210
        background: Rectangle { color: "#0e0e0e"; border.color: "#222"; border.width: 1 }

        contentItem: Column {
            spacing: 0

            Rectangle {
                width: viewMenuPopup.width; height: 28
                color: "#161616"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    text: "VIEW TOGGLES"
                    color: "#999"; font.pixelSize: root.sp(9); font.bold: true; font.letterSpacing: 0.6
                }
            }

            Rectangle {
                id: vt_waveforms
                width: viewMenuPopup.width; height: 26
                readonly property bool on: root.Window.window ? root.Window.window.showWaveforms : true
                color: vt_wfMouse.containsMouse ? "#191919" : "#131313"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: vt_wfPill.left; anchors.rightMargin: 8
                    text: "Scrolling Waveforms"
                    color: vt_waveforms.on ? "#c0c0c0" : "#484848"
                    font.pixelSize: root.sp(9); elide: Text.ElideRight
                }
                Rectangle {
                    id: vt_wfPill
                    width: 24; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    color: vt_waveforms.on ? "#1e7bd4" : "#252525"
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#e0e0e0"; y: 2
                        x: vt_waveforms.on ? parent.width - 10 : 2
                        Behavior on x { NumberAnimation { duration: 80 } }
                    }
                }
                MouseArea {
                    id: vt_wfMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.showWaveforms = !root.Window.window.showWaveforms
                }
            }

            Rectangle {
                id: vt_developmentControls
                width: viewMenuPopup.width; height: 26
                readonly property bool on: root.Window.window ? root.Window.window.showDevelopmentControls : true
                color: vt_devControlsMouse.containsMouse ? "#191919" : "#131313"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: vt_devControlsPill.left; anchors.rightMargin: 8
                    text: "Development Controls"
                    color: vt_developmentControls.on ? "#c0c0c0" : "#484848"
                    font.pixelSize: root.sp(9); elide: Text.ElideRight
                }
                Rectangle {
                    id: vt_devControlsPill
                    width: 24; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    color: vt_developmentControls.on ? "#1e7bd4" : "#252525"
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#e0e0e0"; y: 2
                        x: vt_developmentControls.on ? parent.width - 10 : 2
                        Behavior on x { NumberAnimation { duration: 80 } }
                    }
                }
                MouseArea {
                    id: vt_devControlsMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.showDevelopmentControls = !root.Window.window.showDevelopmentControls
                }
            }

            Rectangle {
                id: vt_deckMode
                width: viewMenuPopup.width
                height: 30
                color: vt_deckModeMouse.containsMouse ? "#191919" : "#131313"
                readonly property bool fourDeck: root.Window.window ? root.Window.window.fourDeckMode : false

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    text: "Deck Layout"
                    color: "#c0c0c0"
                    font.pixelSize: root.sp(9)
                }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    spacing: 2

                    Rectangle {
                        width: 26
                        height: 16
                        radius: 2
                        color: !vt_deckMode.fourDeck ? "#1e7bd4" : "#242424"
                        Text {
                            anchors.centerIn: parent
                            text: "2"
                            color: !vt_deckMode.fourDeck ? "#ffffff" : "#555"
                            font.pixelSize: root.sp(8)
                            font.bold: true
                            font.family: "monospace"
                        }
                    }

                    Rectangle {
                        width: 26
                        height: 16
                        radius: 2
                        color: vt_deckMode.fourDeck ? "#1e7bd4" : "#242424"
                        Text {
                            anchors.centerIn: parent
                            text: "4"
                            color: vt_deckMode.fourDeck ? "#ffffff" : "#555"
                            font.pixelSize: root.sp(8)
                            font.bold: true
                            font.family: "monospace"
                        }
                    }
                }

                MouseArea {
                    id: vt_deckModeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.fourDeckMode = !root.Window.window.fourDeckMode
                }
            }

            Rectangle {
                id: vt_deckA
                width: viewMenuPopup.width; height: 26
                readonly property bool on: root.Window.window ? root.Window.window.showDeckA : true
                color: vt_deckAMouse.containsMouse ? "#191919" : "#131313"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: vt_deckAPill.left; anchors.rightMargin: 8
                    text: "Deck A"
                    color: vt_deckA.on ? "#c0c0c0" : "#484848"
                    font.pixelSize: root.sp(9); elide: Text.ElideRight
                }
                Rectangle {
                    id: vt_deckAPill
                    width: 24; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    color: vt_deckA.on ? "#1e7bd4" : "#252525"
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#e0e0e0"; y: 2
                        x: vt_deckA.on ? parent.width - 10 : 2
                        Behavior on x { NumberAnimation { duration: 80 } }
                    }
                }
                MouseArea {
                    id: vt_deckAMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.showDeckA = !root.Window.window.showDeckA
                }
            }

            Rectangle {
                id: vt_deckB
                width: viewMenuPopup.width; height: 26
                readonly property bool on: root.Window.window ? root.Window.window.showDeckB : true
                color: vt_deckBMouse.containsMouse ? "#191919" : "#131313"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: vt_deckBPill.left; anchors.rightMargin: 8
                    text: "Deck B"
                    color: vt_deckB.on ? "#c0c0c0" : "#484848"
                    font.pixelSize: root.sp(9); elide: Text.ElideRight
                }
                Rectangle {
                    id: vt_deckBPill
                    width: 24; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    color: vt_deckB.on ? "#1e7bd4" : "#252525"
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#e0e0e0"; y: 2
                        x: vt_deckB.on ? parent.width - 10 : 2
                        Behavior on x { NumberAnimation { duration: 80 } }
                    }
                }
                MouseArea {
                    id: vt_deckBMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.showDeckB = !root.Window.window.showDeckB
                }
            }

            Rectangle {
                id: vt_mixer
                visible: false
                width: viewMenuPopup.width; height: 0
                readonly property bool on: root.Window.window ? root.Window.window.showMixer : true
                color: vt_mixerMouse.containsMouse ? "#191919" : "#131313"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: vt_mixerPill.left; anchors.rightMargin: 8
                    text: "Mixer"
                    color: vt_mixer.on ? "#c0c0c0" : "#484848"
                    font.pixelSize: root.sp(9); elide: Text.ElideRight
                }
                Rectangle {
                    id: vt_mixerPill
                    width: 24; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    color: vt_mixer.on ? "#1e7bd4" : "#252525"
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#e0e0e0"; y: 2
                        x: vt_mixer.on ? parent.width - 10 : 2
                        Behavior on x { NumberAnimation { duration: 80 } }
                    }
                }
                MouseArea {
                    id: vt_mixerMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.showMixer = !root.Window.window.showMixer
                }
            }

            Rectangle {
                id: vt_fxBar
                visible: false
                width: viewMenuPopup.width; height: 0
                readonly property bool on: root.Window.window ? root.Window.window.showFxBar : true
                color: vt_fxMouse.containsMouse ? "#191919" : "#131313"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: vt_fxPill.left; anchors.rightMargin: 8
                    text: "FX Bar"
                    color: vt_fxBar.on ? "#c0c0c0" : "#484848"
                    font.pixelSize: root.sp(9); elide: Text.ElideRight
                }
                Rectangle {
                    id: vt_fxPill
                    width: 24; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    color: vt_fxBar.on ? "#1e7bd4" : "#252525"
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#e0e0e0"; y: 2
                        x: vt_fxBar.on ? parent.width - 10 : 2
                        Behavior on x { NumberAnimation { duration: 80 } }
                    }
                }
                MouseArea {
                    id: vt_fxMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.showFxBar = !root.Window.window.showFxBar
                }
            }

            Rectangle {
                id: vt_crossfader
                visible: false
                width: viewMenuPopup.width; height: 0
                readonly property bool on: root.Window.window ? root.Window.window.showCrossfader : true
                color: vt_cfMouse.containsMouse ? "#191919" : "#131313"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: vt_cfPill.left; anchors.rightMargin: 8
                    text: "Crossfader"
                    color: vt_crossfader.on ? "#c0c0c0" : "#484848"
                    font.pixelSize: root.sp(9); elide: Text.ElideRight
                }
                Rectangle {
                    id: vt_cfPill
                    width: 24; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    color: vt_crossfader.on ? "#1e7bd4" : "#252525"
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#e0e0e0"; y: 2
                        x: vt_crossfader.on ? parent.width - 10 : 2
                        Behavior on x { NumberAnimation { duration: 80 } }
                    }
                }
                MouseArea {
                    id: vt_cfMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.showCrossfader = !root.Window.window.showCrossfader
                }
            }

            Rectangle {
                id: vt_library
                width: viewMenuPopup.width; height: 26
                readonly property bool on: root.Window.window ? root.Window.window.showLibrary : true
                color: vt_libMouse.containsMouse ? "#191919" : "#131313"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.right: vt_libPill.left; anchors.rightMargin: 8
                    text: "Library"
                    color: vt_library.on ? "#c0c0c0" : "#484848"
                    font.pixelSize: root.sp(9); elide: Text.ElideRight
                }
                Rectangle {
                    id: vt_libPill
                    width: 24; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 12
                    color: vt_library.on ? "#1e7bd4" : "#252525"
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#e0e0e0"; y: 2
                        x: vt_library.on ? parent.width - 10 : 2
                        Behavior on x { NumberAnimation { duration: 80 } }
                    }
                }
                MouseArea {
                    id: vt_libMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.Window.window) root.Window.window.showLibrary = !root.Window.window.showLibrary
                }
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // MAIN ROW
    // ════════════════════════════════════════════════════════════════════════
    RowLayout {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.collapsedHeight
        z: 10
        spacing: 0

        // ── Branding ─────────────────────────────────────────────────────────
        Item {
            Layout.preferredWidth: brandRow.implicitWidth + root.padH * 2
            Layout.fillHeight: true

            Row {
                id: brandRow
                anchors.centerIn: parent
                spacing: 6

                // Engine-DJ-style left color strip
                Rectangle {
                    width: 2
                    height: parent.height * 0.6
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.accentBlue
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1
                    Text {
                        text: "BROCKDJ"
                        color: "#e8e8e8"
                        font.pixelSize: root.sp(10)
                        font.bold: true
                        font.letterSpacing: 1.4
                    }
                    Text {
                        text: "ramsbrock.net"
                        color: "#333333"
                        font.pixelSize: root.sp(6)
                        font.letterSpacing: 0.3
                    }
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Primary navigation ───────────────────────────────────────────────
        Rectangle {
            id: libraryButton
            Layout.preferredWidth: 78
            Layout.fillHeight: true
            readonly property bool active: root.Window.window
                                           ? (root.Window.window.allInOneMode
                                              ? root.Window.window.libraryPanelActive
                                              : root.Window.window.showLibrary)
                                           : false
            color: libraryMouse.pressed ? "#203446"
                 : active ? "#162b3b"
                 : (libraryMouse.containsMouse ? "#1a2025" : "#121212")
            Row {
                anchors.centerIn: parent
                spacing: 5
                Text { text: "▤"; color: libraryButton.active ? "#a9d4ff" : "#788692"; font.pixelSize: root.sp(15); anchors.verticalCenter: parent.verticalCenter }
                Text { text: "LIBRARY"; color: libraryButton.active ? "#dceeff" : "#aab2b8"; font.pixelSize: root.sp(8); font.bold: true; font.letterSpacing: 0.5; anchors.verticalCenter: parent.verticalCenter }
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 2; visible: libraryButton.active; color: root.accentBlue }
            MouseArea { id: libraryMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: if (root.Window.window) root.Window.window.toggleAllInOneLibrary() }
        }

        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        Rectangle {
            id: searchButton
            Layout.preferredWidth: 74
            Layout.fillHeight: true
            color: searchMouse.pressed ? "#202428" : (searchMouse.containsMouse ? "#1a1e21" : "#121212")
            Row {
                anchors.centerIn: parent
                spacing: 4
                Text { text: "⌕"; color: "#7d8992"; font.pixelSize: root.sp(17); anchors.verticalCenter: parent.verticalCenter }
                Text { text: "SEARCH"; color: "#9aa3a9"; font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.3; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea { id: searchMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor }
        }

        // This spacer keeps the navigation left-aligned and moves all mixer and
        // system controls into a single group on the right.
        Item { Layout.fillWidth: true }

        // ── Anti-Clip ────────────────────────────────────────────────────────
        Rectangle {
            id: antiClipBlock
            Layout.preferredWidth: Math.max(44, Math.round(root.btnH * 1.45))
            Layout.fillHeight: true
            property bool on: false
            property real gr: deckA ? deckA.gainReduction : 1.0
            color: !on       ? "#131313"
                 : gr < 0.5  ? "#3d0000"
                 : gr < 0.7  ? "#6b1010"
                 : gr < 0.99 ? "#8a4a00"
                 : "#0a2e0a"

            Column {
                anchors.centerIn: parent
                spacing: 2

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "A-CLP"
                    color: !antiClipBlock.on ? "#3a3a3a"
                         : antiClipBlock.gr < 0.99 ? "#ffffff" : "#88cc88"
                    font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.3
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 16; height: 2
                    color: !antiClipBlock.on ? "#2a2a2a"
                         : antiClipBlock.gr < 0.5  ? "#ff3333"
                         : antiClipBlock.gr < 0.7  ? "#ff7733"
                         : antiClipBlock.gr < 0.99 ? "#ffaa00"
                         : "#44aa44"
                }
            }

            MouseArea {
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    antiClipBlock.on = !antiClipBlock.on
                    if (deckA) deckA.setAntiClip(antiClipBlock.on)
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Master volume ─────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: root.dialSz + root.padH * 2 + 12
            Layout.fillHeight: true
            color: "#131313"

            Row {
                anchors.centerIn: parent
                spacing: 4

                Text {
                    text: "MST"
                    color: "#404040"
                    font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.3
                    anchors.verticalCenter: parent.verticalCenter
                }

                Dial {
                    id: masterVolDial
                    width: root.dialSz; height: root.dialSz
                    from: 0.0; to: 1.0; value: 0.8
                    anchors.verticalCenter: parent.verticalCenter

                    background: Rectangle {
                        x: masterVolDial.width  / 2 - width  / 2
                        y: masterVolDial.height / 2 - height / 2
                        width: masterVolDial.width; height: masterVolDial.height
                        radius: width / 2; color: "transparent"

                        Canvas {
                            id: mstArc
                            anchors.fill: parent; antialiasing: true
                            onPaint: {
                                var ctx = getContext("2d"); ctx.reset()
                                var cx = width / 2; var cy = height / 2
                                var r = Math.min(width, height) * 0.44
                                var norm = Math.max(0, Math.min(1,
                                    (masterVolDial.value - masterVolDial.from)
                                    / (masterVolDial.to - masterVolDial.from)))
                                ctx.lineWidth = Math.max(1.5, width * 0.07)
                                ctx.lineCap   = "butt"
                                // track
                                ctx.strokeStyle = "#222"
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 120 * Math.PI/180, (120+300) * Math.PI/180)
                                ctx.stroke()
                                // fill
                                ctx.strokeStyle = "#1e7bd4"
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 120 * Math.PI/180, (120 + norm*300) * Math.PI/180)
                                ctx.stroke()
                            }
                            Connections {
                                target: masterVolDial
                                function onValueChanged() { mstArc.requestPaint() }
                            }
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width * 0.72; height: parent.height * 0.72
                            radius: width / 2; color: "#1c1c1c"
                        }
                    }

                    handle: Item {
                        id: mstHandle
                        x: masterVolDial.background.x + masterVolDial.background.width  / 2 - width  / 2
                        y: masterVolDial.background.y + masterVolDial.background.height / 2 - height / 2
                        width:  masterVolDial.width  * 0.72
                        height: masterVolDial.height * 0.72
                        Rectangle {
                            width: 1.5; height: parent.height * 0.42; color: "#c0c0c0"
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top; anchors.topMargin: -1
                        }
                        transform: Rotation {
                            angle: masterVolDial.angle
                            origin.x: mstHandle.width  / 2
                            origin.y: mstHandle.height / 2
                        }
                    }

                    onValueChanged: if (deckA) deckA.setMasterVolume(value)
                    TapHandler {
                        onDoubleTapped: {
                            masterVolDial.enabled = false
                            masterVolDial.value   = 0.8
                            masterVolDial.enabled = true
                        }
                    }
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Headphone cue ────────────────────────────────────────────────────
        Rectangle {
            id: headphoneCueBlock
            Layout.preferredWidth: root.dialSz + root.padH * 2 + 42
            Layout.fillHeight: true
            color: "#121212"

            property bool syncingCueMix: false
            readonly property bool masterCueOn: deckA ? deckA.masterCueEnabled : false

            Connections {
                target: deckA
                function onHeadphoneMixChanged() {
                    headphoneCueBlock.syncingCueMix = true
                    cueMixDial.value = deckA ? deckA.headphoneMix : 0.0
                    headphoneCueBlock.syncingCueMix = false
                }
            }

            Row {
                anchors.centerIn: parent
                spacing: 4

                Rectangle {
                    width: 32
                    height: Math.max(16, root.btnH * 0.48)
                    radius: 3
                    color: headphoneCueBlock.masterCueOn ? "#0c1e2f" : "#171717"
                    border.color: headphoneCueBlock.masterCueOn ? "#1e7bd4" : "#2a2a2a"

                    Text {
                        anchors.centerIn: parent
                        text: "MC"
                        color: headphoneCueBlock.masterCueOn ? "#7ab8f5" : "#555"
                        font.pixelSize: root.sp(7)
                        font.bold: true
                        font.letterSpacing: 0.5
                    }

                    HoverHandler { id: masterCueHover; cursorShape: Qt.PointingHandCursor }
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: "#ffffff"
                        opacity: masterCueHover.hovered && !headphoneCueBlock.masterCueOn ? 0.04 : 0.0
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (deckA) deckA.setMasterCueEnabled(!deckA.masterCueEnabled)
                    }
                }

                Dial {
                    id: cueMixDial
                    width: root.dialSz
                    height: root.dialSz
                    from: 0.0
                    to: 1.0
                    value: deckA ? deckA.headphoneMix : 0.0
                    anchors.verticalCenter: parent.verticalCenter

                    background: Rectangle {
                        x: cueMixDial.width / 2 - width / 2
                        y: cueMixDial.height / 2 - height / 2
                        width: cueMixDial.width
                        height: cueMixDial.height
                        radius: width / 2
                        color: "transparent"

                        Canvas {
                            id: cueMixArc
                            anchors.fill: parent
                            antialiasing: true
                            onPaint: {
                                var ctx = getContext("2d"); ctx.reset()
                                var cx = width / 2; var cy = height / 2
                                var r = Math.min(width, height) * 0.44
                                var norm = Math.max(0, Math.min(1, cueMixDial.value))
                                ctx.lineWidth = Math.max(1.5, width * 0.07)
                                ctx.lineCap = "butt"
                                ctx.strokeStyle = "#222"
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 120 * Math.PI / 180, 420 * Math.PI / 180)
                                ctx.stroke()
                                ctx.strokeStyle = "#7ab8f5"
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 120 * Math.PI / 180, (120 + norm * 300) * Math.PI / 180)
                                ctx.stroke()
                            }
                            Connections {
                                target: cueMixDial
                                function onValueChanged() { cueMixArc.requestPaint() }
                            }
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width * 0.72
                            height: parent.height * 0.72
                            radius: width / 2
                            color: "#1c1c1c"
                        }
                    }

                    handle: Item {
                        id: cueMixHandle
                        x: cueMixDial.background.x + cueMixDial.background.width / 2 - width / 2
                        y: cueMixDial.background.y + cueMixDial.background.height / 2 - height / 2
                        width: cueMixDial.width * 0.72
                        height: cueMixDial.height * 0.72
                        Rectangle {
                            width: 1.5
                            height: parent.height * 0.42
                            color: "#c0c0c0"
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: -1
                        }
                        transform: Rotation {
                            angle: cueMixDial.angle
                            origin.x: cueMixHandle.width / 2
                            origin.y: cueMixHandle.height / 2
                        }
                    }

                    onValueChanged: {
                        if (!headphoneCueBlock.syncingCueMix && deckA)
                            deckA.setHeadphoneMix(value)
                    }
                    TapHandler {
                        onDoubleTapped: cueMixDial.value = 0.5
                    }
                }

                Text {
                    text: "MST"
                    color: headphoneCueBlock.masterCueOn ? "#7ab8f5" : "#3f3f3f"
                    font.pixelSize: root.sp(7)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── System monitor — LAT + CPU + RAM ─────────────────────────────────
        Rectangle {
            id: monitorBlock
            Layout.preferredWidth: 106   // fixed — prevents layout jitter as values change
            Layout.fillHeight: true
            color: latMouse.containsMouse ? "#161616" : "#121212"
            Behavior on color { ColorAnimation { duration: 100 } }

            readonly property color latClr: root.totalLatencyMs >= 35.0 ? "#ff5f52"
                                           : root.totalLatencyMs >= 20.0 ? "#ffcc44"
                                           : "#5bb8f5"
            readonly property real cpuVal: sysMonitor ? sysMonitor.cpuUsage : 0
            readonly property real ramVal: sysMonitor ? sysMonitor.ramUsage : 0

            function metricColor(v) {
                return v > 0.8 ? "#cc4444" : v > 0.5 ? "#cc8800" : "#484848"
            }
            function barColor(v, hue) {
                return v > 0.8 ? "#cc3333" : v > 0.5 ? "#bb7700" : hue
            }

            Column {
                anchors.centerIn: parent
                spacing: 2

                // ── Row 1: Latency ────────────────────────────────────────────
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 3

                    Text {
                        text: "LAT"; color: "#383838"
                        font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.5
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        width: 44
                        text: root.totalLatencyMs > 0 ? root.totalLatencyMs.toFixed(1) + " ms" : "—  ms"
                        color: monitorBlock.latClr
                        font.pixelSize: root.sp(8); font.bold: true; font.family: "monospace"
                        horizontalAlignment: Text.AlignRight
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: latencyPopup.opened ? "▴" : "▾"
                        color: "#363636"; font.pixelSize: root.sp(7)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                // ── Row 2: CPU | RAM side-by-side with mini bars ──────────────
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 2

                    Text {
                        text: "C"; color: "#2e2e2e"
                        font.pixelSize: root.sp(7); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: 18; height: 3; radius: 1; color: "#1c1c1c"
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle {
                            width: Math.max(0, Math.round(parent.width * monitorBlock.cpuVal))
                            height: parent.height; radius: 1
                            color: monitorBlock.barColor(monitorBlock.cpuVal, "#2a5a38")
                            Behavior on width { NumberAnimation { duration: 350; easing.type: Easing.OutQuad } }
                        }
                    }
                    Text {
                        width: 18; text: Math.round(monitorBlock.cpuVal * 100) + "%"
                        color: monitorBlock.metricColor(monitorBlock.cpuVal)
                        font.pixelSize: root.sp(7); font.family: "monospace"
                        horizontalAlignment: Text.AlignRight
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Rectangle { width: 1; height: 7; color: "#252525"; anchors.verticalCenter: parent.verticalCenter }

                    Text {
                        text: "R"; color: "#2e2e2e"
                        font.pixelSize: root.sp(7); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: 18; height: 3; radius: 1; color: "#1c1c1c"
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle {
                            width: Math.max(0, Math.round(parent.width * monitorBlock.ramVal))
                            height: parent.height; radius: 1
                            color: monitorBlock.barColor(monitorBlock.ramVal, "#1a3a5a")
                            Behavior on width { NumberAnimation { duration: 350; easing.type: Easing.OutQuad } }
                        }
                    }
                    Text {
                        width: 18; text: Math.round(monitorBlock.ramVal * 100) + "%"
                        color: monitorBlock.metricColor(monitorBlock.ramVal)
                        font.pixelSize: root.sp(7); font.family: "monospace"
                        horizontalAlignment: Text.AlignRight
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            MouseArea {
                id: latMouse
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root.refreshLatencyInfo()
                    if (latencyPopup.opened) { latencyPopup.close(); return }
                    var p = monitorBlock.mapToItem(latencyPopup.parent, 0, monitorBlock.height + 2)
                    latencyPopup.width = 340
                    latencyPopup.x = p.x; latencyPopup.y = p.y
                    latencyPopup.open()
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Ableton Link ─────────────────────────────────────────────────────
        Rectangle {
            id: linkBlock
            Layout.preferredWidth: 48
            Layout.fillHeight: true
            color: linkMouse.pressed ? "#162016" : ((linkManager && linkManager.enabled) ? "#0d1a12" : "#121212")

            readonly property bool on: linkManager && linkManager.enabled
            readonly property int beatIndex: linkManager ? (((Math.floor(linkManager.beat) % 4) + 4) % 4) : 0

            Column {
                anchors.centerIn: parent
                spacing: 1

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 3
                    Rectangle {
                        width: 4; height: 4; radius: 2
                        anchors.verticalCenter: parent.verticalCenter
                        color: linkBlock.on ? "#3de87a" : "#2a2a2a"
                    }
                    Text {
                        text: "LINK"
                        color: linkBlock.on ? "#3de87a" : "#454545"
                        font.pixelSize: root.sp(6)
                        font.bold: true
                        font.letterSpacing: 0.3
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: linkManager ? linkManager.bpm.toFixed(1) : "120.0"
                    color: linkBlock.on ? "#e8f5e8" : "#555555"
                    font.pixelSize: root.sp(10)
                    font.family: "monospace"
                    font.bold: true
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 2
                    Repeater {
                        model: 4
                        Rectangle {
                            required property int index
                            width: 7
                            height: 2
                            radius: 1
                            color: linkBlock.on && index === linkBlock.beatIndex ? "#3de87a" : "#242424"
                        }
                    }
                }
            }

            MouseArea {
                id: linkMouse
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: if (linkManager) linkManager.enabled = !linkManager.enabled
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── View toggles ──────────────────────────────────────────────────────
        Rectangle {
            id: viewMenuBtn
            Layout.preferredWidth: root.btnH
            Layout.fillHeight: true
            color: viewBtnMouse.pressed ? "#1e1e1e" : (viewBtnMouse.containsMouse ? "#181818" : "#121212")

            Column {
                anchors.centerIn: parent
                spacing: 3
                Repeater {
                    model: 3
                    Rectangle { width: 10; height: 1; color: "#555555" }
                }
            }

            MouseArea {
                id: viewBtnMouse
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (viewMenuPopup.opened) {
                        viewMenuPopup.close()
                    } else {
                        var p = viewMenuBtn.mapToItem(viewMenuPopup.parent, 0, viewMenuBtn.height + 2)
                        viewMenuPopup.x = p.x - viewMenuPopup.width + viewMenuBtn.width
                        viewMenuPopup.y = p.y
                        viewMenuPopup.open()
                    }
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Clock ─────────────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 52
            Layout.fillHeight: true
            color: "#121212"

            Text {
                id: clockText
                anchors.centerIn: parent
                text: root.currentTime
                color: "#888888"
                font.pixelSize: root.sp(12); font.family: "monospace"; font.bold: true
            }
        }

    }

    // ── Pull-down quick access ──────────────────────────────────────────────
    // The same tray is available in desktop and AIO mode.  It gives touch
    // users generously sized shortcuts without permanently taking deck space.
    Rectangle {
        id: quickAccessTray
        anchors.left: parent.left
        anchors.right: parent.right
        // Start hidden behind the fixed header, then slide down from it.
        // This gives the interaction the same visual direction as a mobile
        // notification shade without changing the workspace geometry.
        y: root.collapsedHeight - height
           + height * (root.Window.window ? root.Window.window.topBarPullProgress : 0.0)
        height: UiMetrics.toolbarPullExtra
        color: "#101214"
        opacity: Math.min(1.0, (root.Window.window ? root.Window.window.topBarPullProgress : 0.0) * 3.0)
        visible: opacity > 0.01
        z: 5
        clip: true

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: "#252a2e"
        }

        RowLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - UiMetrics.space6 * 2, 680)
            height: Math.max(0, Math.min(parent.height - UiMetrics.space3 * 2, UiMetrics.px(56)))
            spacing: UiMetrics.space3

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 5
                color: quickModeMouse.pressed ? "#1f3345" : "#18232d"
                Text {
                    anchors.centerIn: parent
                    text: root.Window.window && root.Window.window.allInOneMode ? "AIO MODE" : "DESKTOP MODE"
                    color: "#a9d4ff"; font.pixelSize: root.sp(10); font.bold: true
                }
                MouseArea {
                    id: quickModeMouse; anchors.fill: parent
                    onClicked: if (root.Window.window) {
                                   root.Window.window.setAllInOneMode(!root.Window.window.allInOneMode)
                                   root.Window.window.closeTopBarPullDown()
                               }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 5
                color: quickLibraryMouse.pressed ? "#1f3345" : "#181b1e"
                Text {
                    anchors.centerIn: parent
                    text: "LIBRARY"
                    color: "#d5dce2"; font.pixelSize: root.sp(10); font.bold: true
                }
                MouseArea {
                    id: quickLibraryMouse; anchors.fill: parent
                    onClicked: if (root.Window.window) {
                                   root.Window.window.toggleAllInOneLibrary()
                                   root.Window.window.closeTopBarPullDown()
                               }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 5
                color: quickPerformanceMouse.pressed ? "#1f3345" : "#181b1e"
                Text {
                    anchors.centerIn: parent
                    text: "PERFORMANCE"
                    color: "#d5dce2"; font.pixelSize: root.sp(10); font.bold: true
                }
                MouseArea {
                    id: quickPerformanceMouse; anchors.fill: parent
                    onClicked: if (root.Window.window) {
                                   root.Window.window.activeMainTab = "performance"
                                   root.Window.window.closeTopBarPullDown()
                               }
                }
            }

            Rectangle {
                id: quickSettingsBlock
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 5
                readonly property bool active: root.Window.window ? root.Window.window.settingsPanelActive : false
                color: quickSettingsMouse.pressed ? "#1f3345" : (active ? "#1a3042" : "#181b1e")
                Text {
                    anchors.centerIn: parent
                    text: "SETTINGS"
                    color: quickSettingsBlock.active ? "#a9d4ff" : "#d5dce2"
                    font.pixelSize: root.sp(10); font.bold: true
                }
                MouseArea {
                    id: quickSettingsMouse; anchors.fill: parent
                    onClicked: {
                        if (!root.Window.window)
                            return
                        if (root.Window.window.toggleAllInOneSettings
                                && root.Window.window.toggleAllInOneSettings()) {
                            root.Window.window.closeTopBarPullDown()
                            return
                        }
                        settingsWin.show()
                        settingsWin.raise()
                        settingsWin.requestActivate()
                        root.Window.window.closeTopBarPullDown()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 5
                color: quickRecMouse.pressed ? "#3a1717" : "#211718"
                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Rectangle { width: 7; height: 7; radius: 4; color: "#c84848"; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "REC"; color: "#e2a4a4"; font.pixelSize: root.sp(10); font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea {
                    id: quickRecMouse
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    // Recording control is kept as the existing placeholder.
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 5
                color: quickFullscreenMouse.pressed ? "#1f3345" : "#181b1e"
                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Text { text: "⛶"; color: "#a9bbc8"; font.pixelSize: root.sp(15); anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "FULLSCREEN"; color: "#d5dce2"; font.pixelSize: root.sp(9); font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea {
                    id: quickFullscreenMouse
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (!root.Window.window)
                            return
                        if (root.Window.window.visibility === Window.FullScreen)
                            root.Window.window.showNormal()
                        else
                            root.Window.window.showFullScreen()
                        root.Window.window.closeTopBarPullDown()
                    }
                }
            }
        }
    }

    // A familiar Android-style handle: drag it down to reveal the tray, or
    // tap it to toggle.  It sits above the content, so it is reachable in both
    // AIO and desktop mode.
    Item {
        id: pullHandle
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: 112
        height: 16
        z: 20

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 54
            height: 4
            radius: 2
            color: pullHandleMouse.pressed ? "#b5c9d9" : "#71808b"
        }

        MouseArea {
            id: pullHandleMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.OpenHandCursor
            preventStealing: true
            property real pressY: 0
            property real pressProgress: 0

            onPressed: function(mouse) {
                pressY = mouse.y
                pressProgress = root.Window.window ? root.Window.window.topBarPullProgress : 0
                cursorShape = Qt.ClosedHandCursor
            }
            onPositionChanged: function(mouse) {
                if (!pressed || !root.Window.window)
                    return
                var next = pressProgress + (mouse.y - pressY) / UiMetrics.toolbarPullExtra
                root.Window.window.topBarPullProgress = Math.max(0.0, Math.min(1.0, next))
            }
            onReleased: function(mouse) {
                if (!root.Window.window)
                    return
                var moved = Math.abs(mouse.y - pressY)
                if (moved < 4)
                    root.Window.window.toggleTopBarPullDown()
                else if (root.Window.window.topBarPullProgress >= 0.35)
                    root.Window.window.openTopBarPullDown()
                else
                    root.Window.window.closeTopBarPullDown()
                cursorShape = Qt.OpenHandCursor
            }
            onCanceled: cursorShape = Qt.OpenHandCursor
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // CENTER OVERLAY — VU meter + beat indicators
    // (rendered on top of the RowLayout, horizontally centered)
    // ════════════════════════════════════════════════════════════════════════
    Rectangle {
        id: centerMeter
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: Math.round((root.collapsedHeight - height) / 2) - 5
        z: 11
        width: 136
        height: 22
        radius: 3
        color: "#090909"
        border.width: 1
        border.color: clipNow ? "#a82020" : "#1d1d1d"

        property real levelL: Math.max(deckA ? deckA.vuLevelL : 0, deckB ? deckB.vuLevelL : 0)
        property real levelR: Math.max(deckA ? deckA.vuLevelR : 0, deckB ? deckB.vuLevelR : 0)
        property bool clipNow: (deckA && deckA.clipDetected) || (deckB && deckB.clipDetected)
        readonly property int segs: 24

        function toDb(peak) {
            if (peak <= 0.0001) return -36.0
            return Math.max(-36.0, Math.min(12.0, 20.0 * Math.log10(peak)))
        }
        function toSeg(db) { return Math.floor((db + 36.0) * (segs / 48.0)) }
        function segColor(i) {
            if (i >= 22) return "#ff3b30"
            if (i >= 19) return "#ff8c2a"
            if (i >= 14) return "#d8a21a"
            return "#35c46f"
        }

        Column {
            anchors.centerIn: parent
            spacing: 2

            Row {
                spacing: 2
                Repeater {
                    model: centerMeter.segs
                    Rectangle {
                        required property int index
                        width: 4
                        height: 3
                        radius: 1
                        readonly property int litTo: centerMeter.toSeg(centerMeter.toDb(centerMeter.levelL))
                        color: index <= litTo ? centerMeter.segColor(index) : "#1b1b1b"
                    }
                }
            }

            Row {
                spacing: 2
                Repeater {
                    model: centerMeter.segs
                    Rectangle {
                        required property int index
                        width: 4
                        height: 3
                        radius: 1
                        readonly property int litTo: centerMeter.toSeg(centerMeter.toDb(centerMeter.levelR))
                        color: index <= litTo ? centerMeter.segColor(index) : "#1b1b1b"
                    }
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 3
                Repeater {
                    model: 8
                    Rectangle {
                        required property int index
                        width: 7
                        height: 2
                        radius: 1
                        readonly property bool deckABeat: index < 4
                        readonly property int beatIndex: index % 4
                        readonly property var inf: deckABeat ? root.deckBeatInfo(deckA) : root.deckBeatInfo(deckB)
                        color: !inf.valid ? "#222222"
                             : (beatIndex + 1) === inf.beatInBar ? (deckABeat ? root.clrA : root.clrB)
                             : (deckABeat ? "#382500" : "#002436")
                    }
                }
            }
        }
    }
}
