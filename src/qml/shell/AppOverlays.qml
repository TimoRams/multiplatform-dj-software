import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DJSoftware

Item {
    id: root
    required property var appWindow
    required property var mainLayout
    z: 1000

    StartupOverlay {
        anchors.fill: parent
        appWindow: root.appWindow
        mainLayout: root.mainLayout
        uncleanShutdownWarning: statusOverlay.uncleanShutdownWarning
    }

    StatusOverlay {
        id: statusOverlay
        anchors.fill: parent
        appWindow: root.appWindow
    }

    ExitOverlay {
        anchors.fill: parent
        appWindow: root.appWindow
        mainLayout: root.mainLayout
    }

    component StartupOverlay: Item {
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

            function finishLoading() {
                if (!window.startupReady)
                    return
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
                if (window.startupReady) {
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

            var visualStage = Math.max(0, Math.min(startupStages.length - 1, readyStage))

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

    component StatusOverlay: Item {
        id: root
        required property var appWindow
        readonly property var window: appWindow
        property alias uncleanShutdownWarning: uncleanShutdownWarning

    Rectangle {
        anchors.top: parent.top
        anchors.topMargin: window.uncleanShutdownWarningVisible ? 82 : 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width * 0.92, 640)
        height: audioStartupWarningText.implicitHeight + 24
        radius: 6
        color: "#1a1200"
        border.color: "#7a4800"
        z: 1000
        visible: window.startupReady && window.startupAudioError.length > 0

        Text {
            id: audioStartupWarningText
            anchors.fill: parent
            anchors.margins: 12
            text: "Audio is unavailable: " + window.startupAudioError
                  + ". Open Audio Settings to choose an output device."
            color: "#e0bd75"
            font.pixelSize: window.sp(12)
            wrapMode: Text.WordWrap
        }
    }

    // ── Previous unsafe shutdown notification ───────────────────────────────
    Rectangle {
        id: uncleanShutdownWarning
        anchors.top: parent.top
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width * 0.92, 640)
        height: unsafeShutdownRow.implicitHeight + 22
        radius: 6
        color: "#1d1508"
        border.color: "#8a5a14"
        z: 1001
        visible: window.uncleanShutdownWarningVisible
        opacity: visible ? 1.0 : 0.0

        property string visibleMessage: ""

        Behavior on opacity {
            NumberAnimation { duration: 180; easing.type: Easing.InOutQuad }
        }

        Row {
            id: unsafeShutdownRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 10

            Rectangle {
                width: 22
                height: 22
                radius: 11
                color: "#3a2505"
                border.color: "#a76a16"
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    anchors.centerIn: parent
                    text: "!"
                    color: "#ffb347"
                    font.pixelSize: 14
                    font.bold: true
                }
            }

            Text {
                width: parent.width - 64
                text: uncleanShutdownWarning.visibleMessage
                color: "#dfc08a"
                font.pixelSize: window.sp(12)
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                width: 22
                height: 22
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: unsafeShutdownDismissHover.hovered ? "#3a2610" : "#24180a"
                border.color: "#6a4518"
                HoverHandler { id: unsafeShutdownDismissHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: window.uncleanShutdownWarningVisible = false }

                Text {
                    anchors.centerIn: parent
                    text: "x"
                    color: "#b78b4a"
                    font.pixelSize: 11
                    font.bold: true
                }
            }
        }
    }

    // ── Audio device fallback notification ───────────────────────────────────
    Rectangle {
        id: audioFallbackToast
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width * 0.9, 600)
        height: toastCol.implicitHeight + 20
        radius: 6
        color: "#1a1200"
        border.color: "#7a4800"
        z: 998
        visible: opacity > 0
        opacity: 0.0

        property string message: ""

        Connections {
            target: typeof deckA !== "undefined" && deckA ? deckA : null
            function onAudioDeviceFallbackChanged() {
                var msg = deckA ? deckA.audioDeviceFallbackMessage : ""
                if (msg) {
                    audioFallbackToast.message = msg
                    audioFallbackToast.opacity = 1.0
                    audioFallbackDismissTimer.restart()
                } else {
                    audioFallbackToast.opacity = 0.0
                }
            }
        }

        Timer {
            id: audioFallbackDismissTimer
            interval: 12000
            repeat: false
            onTriggered: audioFallbackToast.opacity = 0.0
        }

        Behavior on opacity {
            NumberAnimation { duration: 300; easing.type: Easing.InOutQuad }
        }

        Row {
            id: toastCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 10

            Text {
                text: "⚠"
                color: "#ff9900"
                font.pixelSize: 14
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                width: parent.width - 60
                text: audioFallbackToast.message
                color: "#ccaa66"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                width: 22; height: 22
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: dismissH.hovered ? "#2a0000" : "#1a0000"
                border.color: "#442222"
                HoverHandler { id: dismissH; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: audioFallbackToast.opacity = 0.0 }
                Text { anchors.centerIn: parent; text: "✕"; color: "#885555"; font.pixelSize: 10 }
            }
        }
    }
    }

    component ExitOverlay: Rectangle {
        id: exitOverlay
        required property var appWindow
        required property Item mainLayout
        readonly property var window: appWindow
        anchors.fill: parent
        z: 1000
        visible: window.exitPromptVisible
        color: "transparent"
        focus: visible

        PerformanceBackdrop {
            objectName: "exitBackdrop"
            anchors.fill: parent
            sourceItem: requested ? exitOverlay.mainLayout : null
            sourceOrigin: Qt.point(0, 0)
            requested: exitOverlay.visible && !window.exitShutdownInProgress
            maximumCaptureSize: Qt.size(512, 512)
            live: false
            tint: Qt.rgba(0.06, 0.06, 0.06, 0.65)
        }

        MouseArea {
            anchors.fill: parent
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width * 0.82, 460)
            spacing: 14

            Text {
                width: parent.width
                text: window.exitShutdownInProgress
                      ? "Closing..."
                      : "Are you sure you want to quit?"
                color: "#e5e5e5"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: window.sp(20)
                font.bold: true
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                visible: !window.exitShutdownInProgress

                Rectangle {
                    width: 118
                    height: 38
                    radius: 0
                    color: yesArea.pressed ? "#3b3b3b" : "#353535"
                    border.width: 1
                    border.color: UiTheme.separator

                    Text {
                        anchors.centerIn: parent
                        text: "OK"
                        color: "#f0f0f0"
                        font.pixelSize: window.sp(13)
                        font.bold: true
                    }

                    MouseArea {
                        id: yesArea
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: window.confirmAppClose()
                    }
                }

                Rectangle {
                    width: 118
                    height: 38
                    radius: 0
                    color: noArea.pressed ? "#3b3b3b" : "#353535"
                    border.width: 1
                    border.color: UiTheme.separator

                    Text {
                        anchors.centerIn: parent
                        text: "Cancel"
                        color: "#f0f0f0"
                        font.pixelSize: window.sp(13)
                        font.bold: true
                    }

                    MouseArea {
                        id: noArea
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: window.cancelAppClosePrompt()
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 6
                color: "#1f1f1f"
                border.width: 1
                border.color: UiTheme.separator
                visible: window.exitShutdownInProgress

                Rectangle {
                    width: parent.width * window.exitProgress
                    height: parent.height
                    color: "#6f6f6f"
                }
            }

            Text {
                width: parent.width
                visible: window.exitShutdownInProgress
                text: "Saving database and settings..."
                color: "#cfcfcf"
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: window.sp(11)
            }

            Text {
                width: parent.width
                visible: !window.exitShutdownInProgress
                text: (typeof libraryDb !== "undefined" && libraryDb !== null)
                      ? (libraryDb.mirroredDatabaseStatus ? libraryDb.mirroredDatabaseStatus : "DB A: unknown | DB B: unknown")
                      : "DB A: unknown | DB B: unknown"
                color: "#b8b8b8"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: window.sp(11)
            }

            CheckBox {
                id: manualBackupCheck
                visible: !window.exitShutdownInProgress
                checked: false
                padding: 0
                spacing: 8
                text: "Save manual database backup"
                indicator: Rectangle {
                    implicitWidth: 16
                    implicitHeight: 16
                    radius: 2
                    border.width: 1
                    border.color: parent.checked ? "#9a9a9a" : "#5a5a5a"
                    color: parent.checked ? "#4c4c4c" : "#232323"

                    Rectangle {
                        anchors.centerIn: parent
                        width: 8
                        height: 8
                        radius: 1
                        color: parent.visible && parent.parent.checked ? "#e5e5e5" : "transparent"
                        visible: parent.parent.checked
                    }
                }
                contentItem: Text {
                    text: manualBackupCheck.text
                    color: "#f0f0f0"
                    font.pixelSize: window.sp(12)
                    font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: manualBackupCheck.indicator.width + manualBackupCheck.spacing
                }
                onCheckedChanged: {
                    if (window.exitShutdownInProgress)
                        return
                    window.exitManualBackupRequested = checked
                }
            }

            Text {
                width: parent.width
                visible: !window.exitShutdownInProgress
                text: "When enabled, a separate backup file is saved on exit, only updated through this option."
                color: "#a8a8a8"
                font.pixelSize: window.sp(10)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }
}
