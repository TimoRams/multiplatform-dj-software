import QtQuick
import QtQuick.Controls
import DJSoftware

Item {
    id: root
    required property var appWindow
    required property var mainLayout
    required property var welcomeOverlay
    required property var uncleanShutdownWarning
    readonly property var window: appWindow

Timer {
    id: loadingTimer
    interval: 80
    running: true
    repeat: true

    property real startedAtMs: Date.now()
    readonly property int minStageMs: 1000
    readonly property int minTotalMs: 5200

    function elapsedMs() {
        return Date.now() - startedAtMs
    }

    function coreReady() {
        return typeof libraryDb !== "undefined" && libraryDb
            && typeof deckA !== "undefined" && deckA
            && typeof deckB !== "undefined" && deckB
            && typeof deckC !== "undefined" && deckC
            && typeof deckD !== "undefined" && deckD
            && typeof midiManager !== "undefined" && midiManager
    }

    function finishLoading() {
        loadingIndicator.running = false
        loadingIndicator.activeStage = loadingIndicator.startupStages.length - 1
        loadingIndicator.stageProgress = 1.0
        loadingIndicator.visible = false
        mainLayout.visible = true
        if (typeof appConfig !== "undefined" && appConfig && !appConfig.firstRunCompleted)
            welcomeOverlay.active = true
        if (typeof libraryDb !== "undefined" && libraryDb && libraryDb.recoveryWarningNeeded) {
            uncleanShutdownWarning.visibleMessage = libraryDb.recoveryWarningMessage
            window.uncleanShutdownWarningVisible = true
        }
    }

    onTriggered: {
        loadingIndicator.refreshStatus()
        if ((coreReady() && elapsedMs() >= minTotalMs) || elapsedMs() >= 9000) {
            stop()
            finishLoading()
        }
    }
}

Item {
    id: loadingIndicator
    property bool running: true
    property int activeStage: 0
    property real stageProgress: 0.04
    property string statusTitle: "Preparing application"
    property string statusDetail: "Loading QML interface"
    readonly property var startupStages: [
        {
            shortName: "UI",
            name: "Interface",
            title: "Starting interface",
            detail: "Preparing the low-latency control surface"
        },
        {
            shortName: "LIB",
            name: "Library",
            title: "Opening library",
            detail: "Connecting database, playlists and browser models"
        },
        {
            shortName: "AUDIO",
            name: "Audio Engine",
            title: "Starting audio engine",
            detail: "Creating DSP graph and device routing"
        },
        {
            shortName: "DECKS",
            name: "Decks",
            title: "Configuring decks",
            detail: "Preparing waveform services and deck state"
        },
        {
            shortName: "MIDI",
            name: "MIDI",
            title: "Connecting control layer",
            detail: "MIDI, mapping and runtime state are online"
        }
    ]
    anchors.fill: parent
    visible: true
    z: 1000

    function refreshStatus() {
        var elapsed = loadingTimer.elapsedMs()
        var readyStage = 0

        if (typeof libraryDb !== "undefined" && libraryDb) {
            readyStage = 1
        }
        if (typeof deckA !== "undefined" && deckA && typeof deckB !== "undefined" && deckB) {
            readyStage = 2
        }
        if (typeof deckC !== "undefined" && deckC && typeof deckD !== "undefined" && deckD) {
            readyStage = 3
        }
        if (typeof midiManager !== "undefined" && midiManager) {
            readyStage = 4
        }

        var visualStage = Math.min(readyStage, Math.floor(elapsed / loadingTimer.minStageMs))
        visualStage = Math.max(0, Math.min(startupStages.length - 1, visualStage))

        // Never regress — avoids the progress bar snapping back when parent width
        // collapses as the overlay hides (NumberAnimation on width).
        activeStage = Math.max(activeStage, visualStage)
        stageProgress = Math.max(stageProgress, Math.max(0.04, (activeStage + 1) / startupStages.length))
        statusTitle = startupStages[visualStage].title
        statusDetail = startupStages[visualStage].detail
    }

    Rectangle {
        anchors.fill: parent
        color: "#070707"
    }

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width * 0.72, 560)
        spacing: 20

        Text {
            text: "BROCK DJ"
            color: "#f2f2f2"
            font.pixelSize: window.sp(24)
            font.bold: true
            font.family: UiTheme.numericFontFamily
            font.letterSpacing: 1.8
            horizontalAlignment: Text.AlignHCenter
            width: parent.width
        }

        Column {
            width: parent.width
            spacing: 6

            Text {
                width: parent.width
                text: loadingIndicator.statusTitle
                color: "#f0f0f0"
                font.pixelSize: window.sp(15)
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }

            Text {
                width: parent.width
                text: loadingIndicator.statusDetail
                color: "#8f8f8f"
                font.pixelSize: window.sp(10)
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                visible: false
            }
        }

        Item {
            width: parent.width
            height: 112

            Text {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                text: loadingIndicator.statusDetail
                color: "#8f8f8f"
                font.pixelSize: window.sp(10)
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 28
                height: 2
                color: "#1c1c1c"

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Math.max(2, parent.width * loadingIndicator.stageProgress)
                    color: "#f2f2f2"

                    Behavior on width {
                        enabled: loadingIndicator.running && loadingIndicator.visible
                        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
                    }
                }
            }

            Row {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 62
                spacing: 0
                Repeater {
                    model: loadingIndicator.startupStages

                    Text {
                        required property int index
                        required property var modelData
                        width: parent.width / loadingIndicator.startupStages.length
                        text: modelData.name
                        color: loadingIndicator.activeStage === index ? "#f2f2f2"
                              : loadingIndicator.activeStage > index ? "#7a7a7a"
                              : "#444444"
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        font.pixelSize: window.sp(9)
                        font.bold: loadingIndicator.activeStage === index
                    }
                }
            }
        }

        Rectangle {
            width: parent.width
            height: 30
            color: "transparent"
            border.color: "#1b1b1b"
            border.width: 1

            Text {
                anchors.centerIn: parent
                text: loadingIndicator.activeStage >= 4
                      ? "Runtime ready"
                      : "Initializing low-latency deck environment"
                color: loadingIndicator.activeStage >= 4 ? "#4dd98a" : "#686868"
                font.pixelSize: window.sp(9)
                font.family: UiTheme.numericFontFamily
                font.letterSpacing: 0.6
            }
        }

        Rectangle {
            width: parent.width
            height: 1
            color: UiTheme.divider
        }

        Text {
            width: parent.width
            text: "Audio device setup may take a moment on first launch."
            color: "#5a5a5a"
            font.pixelSize: window.sp(9)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }
}
}
