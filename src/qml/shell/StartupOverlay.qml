import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DJSoftware

Item {
    id: root
    required property var appWindow
    required property var mainLayout
    required property var uncleanShutdownWarning
    readonly property var window: appWindow
    property bool welcomeActive: false

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
                root.welcomeActive = true
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
        id: welcomeScreen
        anchors.fill: parent
        z: 999
        visible: root.welcomeActive
        opacity: root.welcomeActive ? 1.0 : 0.0

        Behavior on opacity {
            NumberAnimation { duration: 280; easing.type: Easing.InOutQuad }
        }

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.78
            MouseArea { anchors.fill: parent }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width * 0.92, 620)
            height: welcomeContent.implicitHeight + 56
            color: "#18181a"
            border.color: UiTheme.separator
            radius: 8

            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 3
                radius: 3
                color: UiTheme.orange
            }

            ColumnLayout {
                id: welcomeContent
                anchors.fill: parent
                anchors.margins: 40
                spacing: 16

                Text {
                    Layout.fillWidth: true
                    text: "BrockDJ"
                    color: UiTheme.orange
                    font.pixelSize: window.sp(28)
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8

                    Repeater {
                        model: ["PRE-ALPHA", "DEVELOPER BUILD"]
                        Rectangle {
                            required property string modelData
                            implicitWidth: badgeText.implicitWidth + 16
                            implicitHeight: 22
                            color: UiTheme.surfaceInset
                            border.color: UiTheme.borderStrong
                            radius: 3

                            Text {
                                id: badgeText
                                anchors.centerIn: parent
                                text: modelData
                                color: UiTheme.textSecondary
                                font.pixelSize: window.sp(10)
                                font.bold: true
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: "BrockDJ is being built as a fast, performance-grade DJ platform for live venues, club sets and professional hardware."
                    color: UiTheme.textSecondary
                    font.pixelSize: window.sp(13)
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: warningText.implicitHeight + 28
                    color: "#120c00"
                    border.color: "#3d2500"
                    radius: 5

                    Text {
                        id: warningText
                        anchors.fill: parent
                        anchors.margins: 14
                        text: "Ultra-early state: APIs are unstable and crashes or audio glitches are possible. Do not use this build where reliability matters."
                        color: "#b87828"
                        font.pixelSize: window.sp(12)
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: "Source and updates: github.com/TimoRams/multiplatform-dj-software"
                    color: UiTheme.textLabel
                    font.pixelSize: window.sp(11)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap

                    TapHandler {
                        onTapped: Qt.openUrlExternally(
                            "https://github.com/TimoRams/multiplatform-dj-software")
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    CheckBox {
                        id: dontShowAgainCheckBox
                        checked: true
                        text: "Don't show again on startup"
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "Let's Go"
                        onClicked: {
                            if (typeof appConfig !== "undefined" && appConfig)
                                appConfig.completeFirstRun(dontShowAgainCheckBox.checked)
                            root.welcomeActive = false
                        }
                    }
                }
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
