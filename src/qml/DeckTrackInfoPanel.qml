import QtQuick
import QtQuick.Layouts
import DJSoftware

Item {
    id: root

    property string deckName: "A"
    property var engine: null
    property bool onAir: false
    property bool showRemainingTime: true

    readonly property color panelColor: "#23272C"
    readonly property color panelRaised: "#292E34"
    readonly property color outline: "#565D65"
    readonly property color primaryText: "#F3F1D5"
    readonly property color secondaryText: "#D7D9CB"
    readonly property color deckBlue: "#159ED7"
    readonly property color deckBlueDark: "#0878B5"
    readonly property color warningRed: "#F04A2E"
    readonly property color functionOrange: "#F1A12B"
    readonly property color rulerText: "#E4E3D7"
    readonly property real scale: Math.max(0.60, Math.min(width / 720, height / 280))
    readonly property real titleHeight: 60 * scale
    readonly property real infoHeight: 98 * scale
    readonly property real overviewHeight: 60 * scale
    readonly property real rulerHeight: 25 * scale
    readonly property real duration: engine && engine.trackDurationSec > 0 ? engine.trackDurationSec : 0
    readonly property bool hasTrack: engine && engine.hasTrack
    // This is deliberately a concrete UI value, refreshed from the visual
    // transport clock.  The atomic audio playhead is only updated at audio
    // callback boundaries, whereas the visual clock extrapolates smoothly.
    property real displayedPlaybackSeconds: 0
    readonly property real playheadNormalized: duration > 0
                                                   ? Math.max(0, Math.min(1, displayedPlaybackSeconds / duration))
                                                   : 0
    readonly property real displayedTimeSeconds: showRemainingTime
                                                 ? Math.max(0, duration - displayedPlaybackSeconds)
                                                 : displayedPlaybackSeconds
    readonly property int displayedWholeSeconds: Math.floor(displayedTimeSeconds)
    readonly property int displayedMilliseconds: Math.floor((displayedTimeSeconds - displayedWholeSeconds) * 1000 + 0.00001)
    readonly property string displayedTimeMain: !hasTrack || duration <= 0 ? "--:--"
                                               : (showRemainingTime ? "-" : "")
                                                 + Math.floor(displayedWholeSeconds / 60).toString().padStart(2, "0")
                                                 + ":" + (displayedWholeSeconds % 60).toString().padStart(2, "0")
    readonly property string displayedTimeMillis: !hasTrack || duration <= 0 ? ".---"
                                                : "." + Math.min(999, displayedMilliseconds).toString().padStart(3, "0")

    function valuePx(reference, minimum) {
        return Math.max(minimum, Math.round(reference * scale))
    }

    function playbackSeconds() {
        if (!engine || duration <= 0)
            return 0
        var visualPosition = engine.getVisualPositionQml()
        if (isFinite(visualPosition))
            return Math.max(0, Math.min(duration, visualPosition))

        var progress = Number(engine.progress)
        if (isFinite(progress))
            return Math.max(0, Math.min(duration, progress * duration))

        return Math.max(0, Math.min(duration, engine.getPlayheadPositionAtomic()))
    }

    function refreshDisplayedPlaybackPosition() {
        displayedPlaybackSeconds = playbackSeconds()
    }

    function timeParts() {
        if (!hasTrack || duration <= 0)
            return { main: "--:--", millis: ".---", negative: false }
        return {
            main: displayedTimeMain,
            millis: displayedTimeMillis,
            negative: showRemainingTime
        }
    }

    function formattedTrackNumber() {
        if (!engine || !engine.trackNumber)
            return "--"
        var digits = engine.trackNumber.toString().match(/^\d+/)
        if (!digits || digits.length === 0)
            return "--"
        return digits[0].padStart(2, "0")
    }

    function tempoRangeLabel() {
        if (!engine)
            return "±8"
        var range = Number(engine.tempoRangePercent)
        return range >= 100 ? "WIDE" : "±" + Math.round(range)
    }

    function tempoText() {
        if (!engine)
            return "0.00%"
        var tempo = Number(engine.tempoPercent)
        if (!isFinite(tempo)) tempo = 0
        return tempo.toFixed(2) + "%"
    }

    function bpmText() {
        if (!engine || !hasTrack || !isFinite(Number(engine.currentBpm)) || Number(engine.currentBpm) <= 0)
            return "---.-"
        return Number(engine.currentBpm).toFixed(1)
    }

    function rulerMarks() {
        if (!hasTrack || duration <= 0)
            return []
        var interval = duration <= 180 ? 30 : (duration <= 600 ? 60 : (duration <= 1800 ? 120 : 300))
        var marks = []
        for (var second = interval; second < duration; second += interval) {
            var remaining = Math.max(0, Math.round(duration - second))
            var minutes = Math.floor(remaining / 60)
            var secs = remaining % 60
            marks.push({ position: second / duration,
                         label: "-" + minutes.toString() + ":" + secs.toString().padStart(2, "0") })
        }
        return marks
    }

    readonly property var timeMarks: rulerMarks()

    Timer {
        interval: 33
        repeat: true
        running: root.visible && root.engine !== null && root.hasTrack
        triggeredOnStart: true
        onTriggered: root.refreshDisplayedPlaybackPosition()
    }

    onEngineChanged: refreshDisplayedPlaybackPosition()

    Connections {
        target: root.engine
        function onProgressChanged() { root.refreshDisplayedPlaybackPosition() }
        function onTrackMetadataChanged() { root.refreshDisplayedPlaybackPosition() }
        function onPlayingChanged() { root.refreshDisplayedPlaybackPosition() }
    }

    Rectangle {
        anchors.fill: parent
        color: root.panelColor
        border.color: root.outline
        border.width: 1
        radius: 0
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: root.titleHeight
            Layout.minimumHeight: root.titleHeight
            Layout.maximumHeight: root.titleHeight

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: root.valuePx(68, 42)
                gradient: Gradient {
                    GradientStop { position: 0; color: root.deckBlue }
                    GradientStop { position: 1; color: root.deckBlueDark }
                }

                Text {
                    anchors.top: parent.top
                    anchors.topMargin: root.valuePx(7, 4)
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "DECK"
                    color: root.primaryText
                    font.pixelSize: root.valuePx(16, 10)
                    font.weight: Font.Medium
                    font.letterSpacing: 0.5
                }
                Text {
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: root.valuePx(4, 2)
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.deckName
                    color: root.primaryText
                    font.pixelSize: root.valuePx(33, 19)
                    font.weight: Font.Medium
                    font.family: "monospace"
                }
            }

            Text {
                id: titleText
                anchors.left: parent.left
                anchors.leftMargin: root.valuePx(88, 55)
                anchors.right: onAirBadge.visible ? onAirBadge.left : parent.right
                anchors.rightMargin: root.valuePx(16, 9)
                anchors.verticalCenter: parent.verticalCenter
                text: "♪  " + (root.hasTrack ? root.engine.trackTitle : "No Track Loaded")
                color: root.hasTrack ? root.primaryText : "#9B9E98"
                font.pixelSize: root.valuePx(27, 16)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                maximumLineCount: 1
                verticalAlignment: Text.AlignVCenter
            }

            Rectangle {
                id: onAirBadge
                visible: root.onAir
                anchors.top: parent.top
                anchors.right: parent.right
                width: root.valuePx(92, 62)
                height: root.valuePx(30, 20)
                color: root.warningRed
                radius: 0
                Text {
                    anchors.centerIn: parent
                    text: "ON AIR"
                    color: root.primaryText
                    font.pixelSize: root.valuePx(14, 9)
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.4
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: root.infoHeight
            Layout.minimumHeight: root.infoHeight
            Layout.maximumHeight: root.infoHeight

            Item {
                id: trackSection
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: parent.width * 0.15
                anchors.leftMargin: root.valuePx(12, 7)

                Text {
                    anchors.top: parent.top
                    anchors.topMargin: root.valuePx(9, 5)
                    text: "TRACK"
                    color: root.secondaryText
                    font.pixelSize: root.valuePx(16, 10)
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.6
                }
                Text {
                    anchors.top: parent.top
                    anchors.topMargin: root.valuePx(29, 18)
                    text: root.formattedTrackNumber()
                    color: root.hasTrack ? root.primaryText : "#858984"
                    font.pixelSize: root.valuePx(42, 24)
                    font.family: "monospace"
                    font.weight: Font.Medium
                }
                Text {
                    visible: root.engine && root.engine.quantizeEnabled
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: root.valuePx(7, 4)
                    text: "QUANTIZE"
                    color: root.functionOrange
                    font.pixelSize: root.valuePx(15, 9)
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.5
                }
            }

            Item {
                id: timeSection
                anchors.left: trackSection.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: parent.width * 0.36
                clip: true

                Text {
                    anchors.top: parent.top
                    anchors.topMargin: root.valuePx(9, 5)
                    text: root.showRemainingTime ? "REMAIN" : "TIME"
                    color: root.secondaryText
                    font.pixelSize: root.valuePx(16, 10)
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.6
                }
                Item {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: root.valuePx(3, 2)
                    width: parent.width
                    height: root.valuePx(70, 42)
                    Text {
                        id: timeMain
                        anchors.left: parent.left
                        anchors.right: timeMillis.left
                        anchors.rightMargin: root.valuePx(2, 1)
                        anchors.baseline: timeMillis.baseline
                        text: root.displayedTimeMain
                        color: root.primaryText
                        font.pixelSize: Math.max(root.valuePx(24, 14), Math.min(root.valuePx(72, 40), width / 3.7))
                        font.family: "monospace"
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        id: timeMillis
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        text: root.displayedTimeMillis
                        color: root.secondaryText
                        font.pixelSize: Math.max(root.valuePx(16, 10), Math.min(root.valuePx(31, 18), parent.width / 7))
                        font.family: "monospace"
                        font.weight: Font.Medium
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.showRemainingTime = !root.showRemainingTime
                }
            }

            Item {
                id: tempoSection
                anchors.left: timeSection.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: parent.width * 0.21
                clip: true

                Text {
                    anchors.top: parent.top
                    anchors.topMargin: root.valuePx(9, 5)
                    text: "TEMPO"
                    color: root.secondaryText
                    font.pixelSize: root.valuePx(16, 10)
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.6
                }
                Rectangle {
                    anchors.top: parent.top
                    anchors.topMargin: root.valuePx(4, 2)
                    anchors.right: parent.right
                    width: root.valuePx(73, 46)
                    height: root.valuePx(35, 22)
                    color: root.warningRed
                    radius: 0
                    Text {
                        anchors.centerIn: parent
                        text: root.tempoRangeLabel()
                        color: "#3C211A"
                        font.pixelSize: root.valuePx(16, 10)
                        font.weight: Font.DemiBold
                    }
                }
                Text {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: root.valuePx(8, 4)
                    text: root.tempoText()
                    color: root.primaryText
                    font.pixelSize: Math.max(root.valuePx(20, 12), Math.min(root.valuePx(49, 28), parent.width / 3.8))
                    font.family: "monospace"
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideLeft
                }
            }

            Rectangle {
                id: bpmSection
                anchors.left: tempoSection.right
                anchors.right: parent.right
                anchors.rightMargin: root.valuePx(8, 4)
                anchors.top: parent.top
                anchors.topMargin: root.valuePx(4, 2)
                anchors.bottom: parent.bottom
                anchors.bottomMargin: root.valuePx(5, 3)
                color: "transparent"
                border.width: 1
                border.color: root.engine && root.engine.syncMaster ? root.functionOrange : root.secondaryText
                radius: 0
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: root.valuePx(8, 4)
                    anchors.top: parent.top
                    anchors.topMargin: root.valuePx(5, 3)
                    text: "BPM"
                    color: root.secondaryText
                    font.pixelSize: root.valuePx(16, 10)
                    font.weight: Font.DemiBold
                }
                Rectangle {
                    visible: root.engine && root.engine.syncMaster && bpmSection.width >= root.valuePx(86, 56)
                    anchors.right: parent.right
                    anchors.top: parent.top
                    width: root.valuePx(72, 43)
                    height: root.valuePx(28, 18)
                    color: root.functionOrange
                    Text {
                        anchors.centerIn: parent
                        text: "MASTER"
                        color: "#382813"
                        font.pixelSize: root.valuePx(12, 8)
                        font.weight: Font.DemiBold
                    }
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: root.valuePx(8, 4)
                    anchors.right: parent.right
                    anchors.rightMargin: root.valuePx(6, 3)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: root.valuePx(4, 2)
                    text: root.bpmText()
                    color: root.engine && root.engine.syncMaster ? root.functionOrange : root.primaryText
                    font.pixelSize: Math.max(root.valuePx(20, 12), Math.min(root.valuePx(57, 32), width / 3.4))
                    font.family: "monospace"
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }
        }

        Item { Layout.fillWidth: true; Layout.fillHeight: true }

        Item {
            id: overviewArea
            Layout.fillWidth: true
            Layout.preferredHeight: root.overviewHeight
            Layout.minimumHeight: root.overviewHeight
            Layout.maximumHeight: root.overviewHeight
            Layout.leftMargin: root.valuePx(18, 9)
            Layout.rightMargin: root.valuePx(18, 9)
            clip: true

            RgbWaveformItem {
                anchors.fill: parent
                engine: root.engine
                rectified: true
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Math.max(2, root.valuePx(2, 2))
                color: root.rulerText
            }
            Rectangle {
                visible: root.hasTrack
                width: Math.max(3, root.valuePx(3, 3))
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                x: Math.round(root.playheadNormalized * parent.width - width / 2)
                color: root.warningRed
            }
            Canvas {
                visible: root.hasTrack && root.engine && root.engine.mainCueSec > 0 && root.duration > 0
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: root.valuePx(16, 10)
                height: root.valuePx(12, 8)
                x: root.engine && root.duration > 0
                   ? Math.round(Math.max(0, Math.min(parent.width - width,
                                                      root.engine.mainCueSec / root.duration * parent.width - width / 2)))
                   : 0
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.fillStyle = root.functionOrange
                    ctx.beginPath()
                    ctx.moveTo(0, height)
                    ctx.lineTo(width * 0.5, 0)
                    ctx.lineTo(width, height)
                    ctx.closePath()
                    ctx.fill()
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: root.rulerHeight
            Layout.minimumHeight: root.rulerHeight
            Layout.maximumHeight: root.rulerHeight
            Layout.leftMargin: root.valuePx(18, 9)
            Layout.rightMargin: root.valuePx(18, 9)
            Repeater {
                model: root.timeMarks
                Item {
                    required property var modelData
                    x: Math.round(modelData.position * parent.width)
                    width: root.valuePx(56, 34)
                    height: parent.height
                    Rectangle {
                        width: 1
                        height: root.valuePx(10, 6)
                        color: root.rulerText
                    }
                    Text {
                        anchors.top: parent.top
                        anchors.topMargin: root.valuePx(10, 6)
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData.label
                        color: root.rulerText
                        font.pixelSize: root.valuePx(13, 8)
                        font.family: "monospace"
                    }
                }
            }
        }
    }
}
