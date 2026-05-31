import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Window
import DJSoftware

Rectangle {
    id: root
    color: "#0e0e0e"
    clip: true

    // Height is fully controlled by parent layout — no implicitHeight here.

    // ── Sizing helpers ───────────────────────────────────────────────────────
    readonly property int btnH:    Math.max(1, root.height)
    readonly property int padH:    Math.max(14, Math.round(btnH * 0.42))  // inner horizontal pad
    readonly property int sepW:    1                                        // divider width

    // VU meter sizing
    readonly property int vuW:     Math.max(88, Math.round(btnH * 3.2))
    readonly property int vuSegH:  Math.max(4,  Math.round(btnH * 0.14))

    // Beat dot sizing
    readonly property int dotSz:   Math.max(5,  Math.round(btnH * 0.18))

    // Master dial
    readonly property int dialSz:  Math.max(18, Math.round(btnH * 0.60))

    // Deck colors
    readonly property color clrA:  "#ff9900"
    readonly property color clrB:  "#00bfff"

    // Accent
    readonly property color accentBlue: "#1e7bd4"

    // Typography — fixed (no window-height scaling; bar has fixed px height)
    function sp(px) { return px }

    // ── State ────────────────────────────────────────────────────────────────
    property string currentTime: "00:00"
    property real   totalLatencyMs: 0.0
    property var    latencyRows: []
    property var    audioPerfStats: ({})
    property int    beatUiTick: 0

    // ── Timers ───────────────────────────────────────────────────────────────
    Timer {
        interval: 1000; running: true; repeat: true
        onTriggered: {
            var d = new Date()
            root.currentTime = d.getHours().toString().padStart(2,"0") + ":"
                             + d.getMinutes().toString().padStart(2,"0")
        }
    }
    Timer {
        interval: 350; running: true; repeat: true
        onTriggered: root.refreshLatencyInfo()
    }
    Timer {
        interval: 50; running: true; repeat: true
        onTriggered: root.beatUiTick++
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
            beatInBar: ((beatFloor % 4) + 4) % 4,
            barNumber: Math.max(1, Math.floor(beatFloor / 4) + 1)
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
                width: viewMenuPopup.width; height: 26
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
                width: viewMenuPopup.width; height: 26
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
                width: viewMenuPopup.width; height: 26
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
        anchors.fill: parent
        spacing: 0

        // ── Branding ─────────────────────────────────────────────────────────
        Item {
            Layout.preferredWidth: brandRow.implicitWidth + root.padH * 2
            Layout.fillHeight: true

            Row {
                id: brandRow
                anchors.centerIn: parent
                spacing: 8

                // Engine-DJ-style left color strip
                Rectangle {
                    width: 3
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
                        font.pixelSize: root.sp(11)
                        font.bold: true
                        font.letterSpacing: 2.0
                    }
                    Text {
                        text: "ramsbrock.net"
                        color: "#333333"
                        font.pixelSize: root.sp(7)
                        font.letterSpacing: 0.5
                    }
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── LINK block ───────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: linkRow.implicitWidth + root.padH * 2
            Layout.fillHeight: true
            color: (linkManager && linkManager.enabled) ? "#0e1f15" : "transparent"

            MouseArea {
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: if (linkManager) linkManager.enabled = !linkManager.enabled
            }

            Row {
                id: linkRow
                anchors.centerIn: parent
                spacing: 10

                // LINK label + status dot
                Row {
                    spacing: 5
                    anchors.verticalCenter: parent.verticalCenter

                    Rectangle {
                        width: 6; height: 6; radius: 3
                        anchors.verticalCenter: parent.verticalCenter
                        color: (linkManager && linkManager.enabled) ? "#3de87a" : "#2a2a2a"
                    }
                    Text {
                        text: "LINK"
                        color: (linkManager && linkManager.enabled) ? "#3de87a" : "#484848"
                        font.pixelSize: root.sp(9); font.bold: true; font.letterSpacing: 0.8
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: linkManager ? linkManager.numPeers.toString() : "0"
                        color: (linkManager && linkManager.numPeers > 0) ? "#3de87a" : "#333"
                        font.pixelSize: root.sp(9); font.family: "monospace"; font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                // BPM readout
                Text {
                    opacity: (linkManager && linkManager.enabled) ? 1.0 : 0.25
                    text: linkManager ? linkManager.bpm.toFixed(1) : "120.0"
                    color: "#cccccc"
                    font.pixelSize: root.sp(12); font.family: "monospace"; font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }

                // Beat dots
                Row {
                    spacing: 3
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: (linkManager && linkManager.enabled) ? 1.0 : 0.15

                    Repeater {
                        model: 4
                        Rectangle {
                            required property int index
                            width: root.dotSz; height: root.dotSz
                            color: {
                                if (!linkManager || !linkManager.enabled) return "#1e1e1e"
                                var bi = ((Math.floor(linkManager.beat) % 4) + 4) % 4
                                return index === bi ? "#3de87a" : "#1e1e1e"
                            }
                        }
                    }
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Spacer ───────────────────────────────────────────────────────────
        Item { Layout.fillWidth: true }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Anti-Clip ────────────────────────────────────────────────────────
        Rectangle {
            id: antiClipBlock
            Layout.preferredWidth: Math.max(54, Math.round(root.btnH * 1.6))
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
                spacing: 3

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "A-CLIP"
                    color: !antiClipBlock.on ? "#3a3a3a"
                         : antiClipBlock.gr < 0.99 ? "#ffffff" : "#88cc88"
                    font.pixelSize: root.sp(8); font.bold: true; font.letterSpacing: 0.5
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 20; height: 2
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
            Layout.preferredWidth: root.dialSz + root.padH * 2 + 18
            Layout.fillHeight: true
            color: "#131313"

            Row {
                anchors.centerIn: parent
                spacing: 6

                Text {
                    text: "MST"
                    color: "#404040"
                    font.pixelSize: root.sp(8); font.bold: true; font.letterSpacing: 0.5
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
            Layout.preferredWidth: root.dialSz + root.padH * 2 + 58
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
                spacing: 7

                Rectangle {
                    width: 42
                    height: Math.max(20, root.btnH * 0.48)
                    radius: 3
                    color: headphoneCueBlock.masterCueOn ? "#0c1e2f" : "#171717"
                    border.color: headphoneCueBlock.masterCueOn ? "#1e7bd4" : "#2a2a2a"

                    Text {
                        anchors.centerIn: parent
                        text: "M CUE"
                        color: headphoneCueBlock.masterCueOn ? "#7ab8f5" : "#555"
                        font.pixelSize: root.sp(8)
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

                Text {
                    text: "CUE"
                    color: "#3f3f3f"
                    font.pixelSize: root.sp(8)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
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
                    font.pixelSize: root.sp(8)
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
            Layout.preferredWidth: 128   // fixed — prevents layout jitter as values change
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
                spacing: 3   // 2 rows × sp(7–9) + 3px gap fits in 34px

                // ── Row 1: Latency ────────────────────────────────────────────
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 4

                    Text {
                        text: "LAT"; color: "#383838"
                        font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.5
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        width: 52
                        text: root.totalLatencyMs > 0 ? root.totalLatencyMs.toFixed(1) + " ms" : "—  ms"
                        color: monitorBlock.latClr
                        font.pixelSize: root.sp(9); font.bold: true; font.family: "monospace"
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
                    spacing: 3

                    Text {
                        text: "C"; color: "#2e2e2e"
                        font.pixelSize: root.sp(7); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: 26; height: 3; radius: 1; color: "#1c1c1c"
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle {
                            width: Math.max(0, Math.round(parent.width * monitorBlock.cpuVal))
                            height: parent.height; radius: 1
                            color: monitorBlock.barColor(monitorBlock.cpuVal, "#2a5a38")
                            Behavior on width { NumberAnimation { duration: 350; easing.type: Easing.OutQuad } }
                        }
                    }
                    Text {
                        width: 22; text: Math.round(monitorBlock.cpuVal * 100) + "%"
                        color: monitorBlock.metricColor(monitorBlock.cpuVal)
                        font.pixelSize: root.sp(7); font.family: "monospace"
                        horizontalAlignment: Text.AlignRight
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Rectangle { width: 1; height: 8; color: "#252525"; anchors.verticalCenter: parent.verticalCenter }

                    Text {
                        text: "R"; color: "#2e2e2e"
                        font.pixelSize: root.sp(7); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: 26; height: 3; radius: 1; color: "#1c1c1c"
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle {
                            width: Math.max(0, Math.round(parent.width * monitorBlock.ramVal))
                            height: parent.height; radius: 1
                            color: monitorBlock.barColor(monitorBlock.ramVal, "#1a3a5a")
                            Behavior on width { NumberAnimation { duration: 350; easing.type: Easing.OutQuad } }
                        }
                    }
                    Text {
                        width: 22; text: Math.round(monitorBlock.ramVal * 100) + "%"
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

        // ── Clock ─────────────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 58   // fixed — HH:MM never changes character count
            Layout.fillHeight: true
            color: "#121212"

            Text {
                id: clockText
                anchors.centerIn: parent
                text: root.currentTime
                color: "#888888"
                font.pixelSize: root.sp(13); font.family: "monospace"; font.bold: true
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── REC button ────────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: root.btnH
            Layout.fillHeight: true
            color: recMouse.pressed ? "#2a0a0a" : "#131313"

            Column {
                anchors.centerIn: parent
                spacing: 3
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 7; height: 7; radius: 4
                    color: "#993333"
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "REC"; color: "#3a3a3a"
                    font.pixelSize: root.sp(7); font.bold: true; font.letterSpacing: 0.4
                }
            }
            MouseArea { id: recMouse; anchors.fill: parent; cursorShape: Qt.PointingHandCursor }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Fullscreen ────────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: root.btnH
            Layout.fillHeight: true
            color: fsMouse.pressed ? "#1e1e1e" : (fsMouse.containsMouse ? "#181818" : "#121212")

            Text {
                anchors.centerIn: parent
                text: "⛶"; color: "#555555"
                font.pixelSize: root.sp(14)
            }
            MouseArea {
                id: fsMouse
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.Window.window.visibility === Window.FullScreen)
                        root.Window.window.showNormal()
                    else
                        root.Window.window.showFullScreen()
                }
            }
        }

        // ── Settings ──────────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: root.btnH
            Layout.fillHeight: true
            color: stgMouse.pressed ? "#1e1e1e" : (stgMouse.containsMouse ? "#181818" : "#121212")

            Text {
                anchors.centerIn: parent
                text: "⚙"; color: "#555555"
                font.pixelSize: root.sp(14)
            }
            MouseArea {
                id: stgMouse
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: { settingsWin.show(); settingsWin.raise(); settingsWin.requestActivate() }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── Deck count selector ───────────────────────────────────────────────
        Rectangle {
            id: deckModeBlock
            Layout.preferredWidth: deckModeRow.implicitWidth + root.padH * 2
            Layout.fillHeight: true
            color: "#121212"

            readonly property bool fourDeck: root.Window.window ? root.Window.window.fourDeckMode : false

            Row {
                id: deckModeRow
                anchors.centerIn: parent
                spacing: 2

                Rectangle {
                    width: 24; height: Math.max(14, Math.round(root.btnH * 0.55))
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 2
                    color: !deckModeBlock.fourDeck ? "#1e7bd4" : "#1c1c1c"
                    Text {
                        anchors.centerIn: parent
                        text: "2"; color: !deckModeBlock.fourDeck ? "#ffffff" : "#555"
                        font.pixelSize: root.sp(9); font.bold: true; font.family: "monospace"
                    }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: if (root.Window.window) root.Window.window.fourDeckMode = false
                    }
                }

                Rectangle {
                    width: 24; height: Math.max(14, Math.round(root.btnH * 0.55))
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 2
                    color: deckModeBlock.fourDeck ? "#1e7bd4" : "#1c1c1c"
                    Text {
                        anchors.centerIn: parent
                        text: "4"; color: deckModeBlock.fourDeck ? "#ffffff" : "#555"
                        font.pixelSize: root.sp(9); font.bold: true; font.family: "monospace"
                    }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: if (root.Window.window) root.Window.window.fourDeckMode = true
                    }
                }
            }
        }

        // ── Separator ────────────────────────────────────────────────────────
        Rectangle { width: root.sepW; Layout.fillHeight: true; color: "#1c1c1c" }

        // ── View toggles ──────────────────────────────────────────────────────
        Rectangle {
            id: viewMenuBtn
            Layout.preferredWidth: root.btnH + 4
            Layout.fillHeight: true
            color: viewBtnMouse.pressed ? "#1e1e1e" : (viewBtnMouse.containsMouse ? "#181818" : "#121212")

            Column {
                anchors.centerIn: parent
                spacing: 3
                Repeater {
                    model: 3
                    Rectangle { width: 11; height: 1; color: "#555555" }
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
    }

    // ════════════════════════════════════════════════════════════════════════
    // CENTER OVERLAY — VU meter + beat indicators
    // (rendered on top of the RowLayout, horizontally centered)
    // ════════════════════════════════════════════════════════════════════════
    Row {
        anchors.centerIn: parent
        spacing: 6

        // ── Beat indicators (Deck A / B) ──────────────────────────────────
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            // Deck A
            Row {
                spacing: 5
                Text {
                    text: "A"; color: root.clrA
                    font.pixelSize: root.sp(8); font.bold: true
                    width: 8; horizontalAlignment: Text.AlignHCenter
                    anchors.verticalCenter: parent.verticalCenter
                }
                Row {
                    spacing: 3
                    Repeater {
                        model: 4
                        Rectangle {
                            required property int index
                            readonly property var inf: root.deckBeatInfo(deckA)
                            width: root.dotSz; height: root.dotSz
                            color: {
                                if (!inf.valid) return index === 0 ? "#3a2800" : "#1a1a1a"
                                if (index === inf.beatInBar) return "#ffb347"
                                return index === 0 ? "#4a3400" : "#1a1a1a"
                            }
                        }
                    }
                }
                Text {
                    readonly property var inf: root.deckBeatInfo(deckA)
                    text: inf.valid ? ("B" + inf.barNumber) : "B—"
                    color: "#484848"; font.pixelSize: root.sp(7); font.family: "monospace"
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Deck B
            Row {
                spacing: 5
                Text {
                    text: "B"; color: root.clrB
                    font.pixelSize: root.sp(8); font.bold: true
                    width: 8; horizontalAlignment: Text.AlignHCenter
                    anchors.verticalCenter: parent.verticalCenter
                }
                Row {
                    spacing: 3
                    Repeater {
                        model: 4
                        Rectangle {
                            required property int index
                            readonly property var inf: root.deckBeatInfo(deckB)
                            width: root.dotSz; height: root.dotSz
                            color: {
                                if (!inf.valid) return index === 0 ? "#001a2e" : "#1a1a1a"
                                if (index === inf.beatInBar) return "#44ccff"
                                return index === 0 ? "#001e36" : "#1a1a1a"
                            }
                        }
                    }
                }
                Text {
                    readonly property var inf: root.deckBeatInfo(deckB)
                    text: inf.valid ? ("B" + inf.barNumber) : "B—"
                    color: "#484848"; font.pixelSize: root.sp(7); font.family: "monospace"
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // ── VU meters ────────────────────────────────────────────────────────
        Column {
            id: vuCol
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            // ── Signal properties ──────────────────────────────────────────
            property real levelL: Math.max(deckA ? deckA.vuLevelL : 0, deckB ? deckB.vuLevelL : 0)
            property real levelR: Math.max(deckA ? deckA.vuLevelR : 0, deckB ? deckB.vuLevelR : 0)
            property bool clipNow: (deckA && deckA.clipDetected) || (deckB && deckB.clipDetected)

            readonly property int segs: 40
            property real peakL: -36.0
            property real peakR: -36.0

            function toDb(peak) {
                if (peak <= 0.0001) return -36.0
                return Math.max(-36.0, Math.min(12.0, 20.0 * Math.log10(peak)))
            }
            function toSeg(db) { return Math.floor((db + 36.0) * (segs / 48.0)) }
            function segColor(i) {
                if (i >= 37) return "#ff2424"
                if (i >= 33) return "#ff7722"
                if (i >= 22) return "#ffaa00"
                return "#2cb84e"
            }

            onLevelLChanged: {
                var db = toDb(levelL)
                if (db > peakL) { peakL = db; decL.restart() }
            }
            onLevelRChanged: {
                var db = toDb(levelR)
                if (db > peakR) { peakR = db; decR.restart() }
            }

            Timer { id: decL; interval: 280; onTriggered: animL.start() }
            Timer { id: decR; interval: 280; onTriggered: animR.start() }
            NumberAnimation { id: animL; target: vuCol; property: "peakL"; to: -36.0; duration: 900; easing.type: Easing.InQuad }
            NumberAnimation { id: animR; target: vuCol; property: "peakR"; to: -36.0; duration: 900; easing.type: Easing.InQuad }

            // L bar
            Row {
                spacing: 1
                Repeater {
                    model: vuCol.segs
                    Rectangle {
                        required property int index
                        width: Math.floor(root.vuW / vuCol.segs)
                        height: root.vuSegH
                        property int litTo: vuCol.toSeg(vuCol.toDb(vuCol.levelL))
                        property int pkSeg: vuCol.toSeg(vuCol.peakL)
                        color: index === pkSeg     ? "#ffffff"
                             : index <= litTo      ? vuCol.segColor(index)
                             : "#161616"
                    }
                }
            }

            // R bar
            Row {
                spacing: 1
                Repeater {
                    model: vuCol.segs
                    Rectangle {
                        required property int index
                        width: Math.floor(root.vuW / vuCol.segs)
                        height: root.vuSegH
                        property int litTo: vuCol.toSeg(vuCol.toDb(vuCol.levelR))
                        property int pkSeg: vuCol.toSeg(vuCol.peakR)
                        color: index === pkSeg     ? "#ffffff"
                             : index <= litTo      ? vuCol.segColor(index)
                             : "#161616"
                    }
                }
            }
        }

        // ── CLIP indicator ───────────────────────────────────────────────────
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            property real peakDb: Math.max(vuCol.toDb(vuCol.levelL), vuCol.toDb(vuCol.levelR))
            property bool hardClip: vuCol.clipNow && peakDb >= 3.0
            property bool softClip: vuCol.clipNow && peakDb < 3.0

            Text {
                text: "CLIP"
                color: parent.hardClip ? "#ff3333"
                     : parent.softClip ? "#ff6655"
                     : "#252525"
                font.pixelSize: root.sp(9); font.bold: true; font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
                width: 32
            }
            Text {
                text: parent.peakDb.toFixed(1)
                color: parent.hardClip ? "#ff5555" : "#303030"
                font.pixelSize: root.sp(7); font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
                width: 32
            }
        }
    }
}
