import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Window {
    id: settingsWindow
    title: "Settings"
    width: 800
    height: 600
    minimumWidth: 600
    minimumHeight: 400
    visible: false
    color: "#1e1e19"
    flags: Qt.Dialog

    onVisibleChanged: {
        if (visible) {
            if (!audioSyncPending) {
                audioSyncPending = true
                audioSyncTimer.start()
            }
            audioDeviceListTimer.start()
        } else {
            audioSyncTimer.stop()
            audioDeviceListTimer.stop()
            audioSyncPending = false
        }
    }

    // ── Navigation categories ────────────────────────────────────────────────
    property int selectedCategory: 0
    property int pendingAudioSampleRate: 44100
    property int pendingAudioBufferSize: 128
    property string pendingAudioDeviceType: ""
    property bool audioUiSyncing: false
    property bool audioSyncPending: false
    property var outputChannelPairsCache: ({})

    property string pendingMasterOutputDevice: ""
    property int pendingMasterFirstChannel: 1

    property string pendingHeadphonesOutputDevice: ""
    property int pendingHeadphonesFirstChannel: -1

    property string pendingBoothOutputDevice: ""
    property int pendingBoothFirstChannel: -1

    property var audioDeviceTypeOptions: []
    property var audioOutputDeviceOptions: []
    property var masterChannelPairOptions: ["None", "1-2"]
    property var headphonesChannelPairOptions: ["None", "1-2"]
    property var boothChannelPairOptions: ["None", "1-2"]

    Timer {
        id: audioSyncTimer
        interval: 0
        repeat: false
        onTriggered: {
            settingsWindow.audioSyncPending = false
            settingsWindow.syncAudioSettings()
        }
    }

    // Backup refresh in case device enumeration (ALSA/JACK) wasn't complete at first open.
    Timer {
        id: audioDeviceListTimer
        interval: 500
        repeat: false
        onTriggered: {
            settingsWindow.refreshAudioDeviceLists()
        }
    }

    readonly property var categories: [
        { label: "Audio Setup",     icon: "♪" },
        { label: "MIDI Controller", icon: "⎘" },
        { label: "Library",         icon: "☰" },
        { label: "Legal",           icon: "§" },
    ]

    readonly property bool isJackDeviceSelected: String(pendingAudioDeviceType).toLowerCase().indexOf("jack") >= 0

    readonly property var sampleRateOptions: [
        { label: "44.1 kHz", value: 44100 },
        { label: "48 kHz", value: 48000 },
        { label: "88.2 kHz", value: 88200 },
        { label: "96 kHz", value: 96000 }
    ]

    readonly property var bufferSizeOptions: [
        { label: "64 samples", value: 64 },
        { label: "128 samples", value: 128 },
        { label: "256 samples", value: 256 },
        { label: "512 samples", value: 512 },
        { label: "1024 samples", value: 1024 }
    ]

    function indexForValue(options, value) {
        for (var i = 0; i < options.length; ++i) {
            if (options[i].value === value)
                return i
        }
        return 0
    }

    function indexForText(options, value) {
        for (var i = 0; i < options.length; ++i) {
            if (String(options[i]) === String(value))
                return i
        }
        return -1
    }

    function parseFirstChannel(pairText) {
        if (!pairText || String(pairText) === "None")
            return -1
        var parts = String(pairText).split("-")
        var first = parseInt(parts[0])
        return isNaN(first) ? -1 : first
    }

    function pairTextForFirstChannel(options, firstChannel) {
        if (firstChannel < 1)
            return "None"
        var desired = String(firstChannel) + "-" + String(firstChannel + 1)
        if (indexForText(options, desired) >= 0)
            return desired
        return options.length > 0 ? String(options[0]) : "None"
    }

    function outputPairCacheKey(outputDevice) {
        return String(pendingAudioDeviceType) + "|" + String(outputDevice)
    }

    function getOutputPairOptions(outputDevice) {
        var key = outputPairCacheKey(outputDevice)
        if (outputChannelPairsCache[key] !== undefined)
            return outputChannelPairsCache[key]

        var options = deckA.getAvailableOutputChannelPairs(pendingAudioDeviceType, outputDevice)
        if (!options || options.length === 0)
            options = ["None", "1-2"]

        outputChannelPairsCache[key] = options
        return options
    }

    function getRoleSelections(role) {
        if (role === "master")
            return { outputDevice: pendingMasterOutputDevice, firstChannel: pendingMasterFirstChannel }
        if (role === "headphones")
            return { outputDevice: pendingHeadphonesOutputDevice, firstChannel: pendingHeadphonesFirstChannel }
        return { outputDevice: pendingBoothOutputDevice, firstChannel: pendingBoothFirstChannel }
    }

    function setRoleSelections(role, outputDevice, firstChannel) {
        if (role === "master") {
            pendingMasterOutputDevice = outputDevice
            pendingMasterFirstChannel = firstChannel
            return
        }
        if (role === "headphones") {
            pendingHeadphonesOutputDevice = outputDevice
            pendingHeadphonesFirstChannel = firstChannel
            return
        }
        pendingBoothOutputDevice = outputDevice
        pendingBoothFirstChannel = firstChannel
    }

    function refreshRoleOutputAndPairs(role) {
        if (!deckA || !deckA.getAvailableAudioOutputDevices)
            return

        var selections = getRoleSelections(role)
        var outputOptions = audioOutputDeviceOptions
        if (!outputOptions || outputOptions.length === 0)
            outputOptions = ["None"]

        if (indexForText(outputOptions, selections.outputDevice) < 0)
            selections.outputDevice = outputOptions[0]

        setRoleSelections(role, selections.outputDevice, selections.firstChannel)

        audioUiSyncing = true
        if (role === "master") {
            masterOutputCombo.currentIndex = Math.max(0, indexForText(outputOptions, pendingMasterOutputDevice))
            refreshRoleChannelPairs("master")
            audioUiSyncing = false
            return
        }

        if (role === "headphones") {
            headphonesOutputCombo.currentIndex = Math.max(0, indexForText(outputOptions, pendingHeadphonesOutputDevice))
            refreshRoleChannelPairs("headphones")
            audioUiSyncing = false
            return
        }

        boothOutputCombo.currentIndex = Math.max(0, indexForText(outputOptions, pendingBoothOutputDevice))
        refreshRoleChannelPairs("booth")
        audioUiSyncing = false
    }

    function refreshRoleChannelPairs(role) {
        if (!deckA || !deckA.getAvailableOutputChannelPairs)
            return

        var selections = getRoleSelections(role)
        var pairOptions = getOutputPairOptions(selections.outputDevice)

        var pairText = pairTextForFirstChannel(pairOptions, selections.firstChannel)
        var firstChannel = parseFirstChannel(pairText)
        setRoleSelections(role, selections.outputDevice, firstChannel)

        audioUiSyncing = true
        if (role === "master") {
            masterChannelPairOptions = pairOptions
            masterChannelCombo.currentIndex = Math.max(0, indexForText(masterChannelPairOptions, pairText))
            audioUiSyncing = false
            return
        }
        if (role === "headphones") {
            headphonesChannelPairOptions = pairOptions
            headphonesChannelCombo.currentIndex = Math.max(0, indexForText(headphonesChannelPairOptions, pairText))
            audioUiSyncing = false
            return
        }
        boothChannelPairOptions = pairOptions
        boothChannelCombo.currentIndex = Math.max(0, indexForText(boothChannelPairOptions, pairText))
        audioUiSyncing = false
    }

    function refreshOutputsForPendingType() {
        if (!deckA || !deckA.getAvailableAudioOutputDevices)
            return

        outputChannelPairsCache = ({})

        audioOutputDeviceOptions = deckA.getAvailableAudioOutputDevices(pendingAudioDeviceType)
        if (!audioOutputDeviceOptions || audioOutputDeviceOptions.length === 0)
            audioOutputDeviceOptions = ["None"]

        audioUiSyncing = true
        refreshRoleOutputAndPairs("master")
        refreshRoleOutputAndPairs("headphones")
        refreshRoleOutputAndPairs("booth")
        audioUiSyncing = false
    }

    function refreshAudioDeviceLists() {
        if (!deckA || !deckA.getAvailableAudioDeviceTypes)
            return

        audioDeviceTypeOptions = deckA.getAvailableAudioDeviceTypes()
        if (!audioDeviceTypeOptions || audioDeviceTypeOptions.length === 0)
            audioDeviceTypeOptions = [""]
        if (!pendingAudioDeviceType || indexForText(audioDeviceTypeOptions, pendingAudioDeviceType) < 0) {
            var currentType = deckA.getCurrentAudioDeviceType ? deckA.getCurrentAudioDeviceType() : ""
            if (currentType && indexForText(audioDeviceTypeOptions, currentType) >= 0)
                pendingAudioDeviceType = currentType
            else
                pendingAudioDeviceType = audioDeviceTypeOptions[0]
        }

        audioUiSyncing = true
        audioDeviceTypeCombo.currentIndex = Math.max(0, indexForText(audioDeviceTypeOptions, pendingAudioDeviceType))
        audioUiSyncing = false

        refreshOutputsForPendingType()
    }

    function syncAudioSettings() {
        if (!settingsManager)
            return

        pendingAudioDeviceType = settingsManager.audioDeviceType
        if (!pendingAudioDeviceType && deckA && deckA.getCurrentAudioDeviceType)
            pendingAudioDeviceType = deckA.getCurrentAudioDeviceType()

        pendingMasterOutputDevice = settingsManager.audioMasterOutputDevice
        pendingMasterFirstChannel = settingsManager.audioMasterFirstChannel

        pendingHeadphonesOutputDevice = settingsManager.audioHeadphonesOutputDevice
        pendingHeadphonesFirstChannel = settingsManager.audioHeadphonesFirstChannel

        pendingBoothOutputDevice = settingsManager.audioBoothOutputDevice
        pendingBoothFirstChannel = settingsManager.audioBoothFirstChannel

        pendingAudioSampleRate = settingsManager.audioSampleRate
        pendingAudioBufferSize = settingsManager.audioBufferSize

        audioUiSyncing = true
        sampleRateCombo.currentIndex = indexForValue(sampleRateOptions, pendingAudioSampleRate)
        bufferSizeCombo.currentIndex = indexForValue(bufferSizeOptions, pendingAudioBufferSize)
        audioUiSyncing = false

        // Populate device type and output combos immediately using the pending values
        // we just set — no separate timer needed for the first open.
        refreshAudioDeviceLists()
    }

    function applyAudioSettings() {
        if (!settingsManager)
            return

        var deviceType = audioDeviceTypeOptions.length > 0 && audioDeviceTypeCombo.currentIndex >= 0
            ? audioDeviceTypeOptions[audioDeviceTypeCombo.currentIndex] : pendingAudioDeviceType

        var masterOutputDevice = audioOutputDeviceOptions.length > 0 && masterOutputCombo.currentIndex >= 0
            ? audioOutputDeviceOptions[masterOutputCombo.currentIndex] : pendingMasterOutputDevice
        var masterFirstChannel = masterChannelPairOptions.length > 0 && masterChannelCombo.currentIndex >= 0
            ? parseFirstChannel(masterChannelPairOptions[masterChannelCombo.currentIndex]) : pendingMasterFirstChannel

        var headphonesOutputDevice = audioOutputDeviceOptions.length > 0 && headphonesOutputCombo.currentIndex >= 0
            ? audioOutputDeviceOptions[headphonesOutputCombo.currentIndex] : pendingHeadphonesOutputDevice
        var headphonesFirstChannel = headphonesChannelPairOptions.length > 0 && headphonesChannelCombo.currentIndex >= 0
            ? parseFirstChannel(headphonesChannelPairOptions[headphonesChannelCombo.currentIndex]) : pendingHeadphonesFirstChannel

        var boothOutputDevice = audioOutputDeviceOptions.length > 0 && boothOutputCombo.currentIndex >= 0
            ? audioOutputDeviceOptions[boothOutputCombo.currentIndex] : pendingBoothOutputDevice
        var boothFirstChannel = boothChannelPairOptions.length > 0 && boothChannelCombo.currentIndex >= 0
            ? parseFirstChannel(boothChannelPairOptions[boothChannelCombo.currentIndex]) : pendingBoothFirstChannel

        var sampleRate = sampleRateOptions[sampleRateCombo.currentIndex].value
        var bufferSize = bufferSizeOptions[bufferSizeCombo.currentIndex].value

        pendingAudioDeviceType = deviceType
        pendingMasterOutputDevice = masterOutputDevice
        pendingMasterFirstChannel = masterFirstChannel
        pendingHeadphonesOutputDevice = headphonesOutputDevice
        pendingHeadphonesFirstChannel = headphonesFirstChannel
        pendingBoothOutputDevice = boothOutputDevice
        pendingBoothFirstChannel = boothFirstChannel

        pendingAudioSampleRate = sampleRate
        pendingAudioBufferSize = bufferSize

        settingsManager.audioDeviceType = deviceType
        settingsManager.audioMasterOutputDevice = masterOutputDevice
        settingsManager.audioMasterFirstChannel = masterFirstChannel
        settingsManager.audioHeadphonesOutputDevice = headphonesOutputDevice
        settingsManager.audioHeadphonesFirstChannel = headphonesFirstChannel
        settingsManager.audioBoothOutputDevice = boothOutputDevice
        settingsManager.audioBoothFirstChannel = boothFirstChannel
        settingsManager.audioSampleRate = sampleRate
        settingsManager.audioBufferSize = bufferSize

        var deckToApply = deckA && deckA.applyAudioDeviceSettings ? deckA
            : (deckB && deckB.applyAudioDeviceSettings ? deckB : null)

        if (deviceType && String(deviceType).toLowerCase().indexOf("jack") >= 0
            && deckToApply && deckToApply.isJackServerRunning && !deckToApply.isJackServerRunning()) {
            audioApplyStatus.text = deckToApply.jackServerStatus
                ? deckToApply.jackServerStatus()
                : "JACK server not running. Start PipeWire-JACK or jackd."
            audioApplyStatus.color = "#ffb86c"
            return
        }

        var applied = deckToApply
            ? deckToApply.applyAudioDeviceSettings(deviceType,
                                                   masterOutputDevice,
                                                   sampleRate,
                                                   bufferSize,
                                                   masterFirstChannel,
                                                   headphonesFirstChannel,
                                                   boothFirstChannel)
            : false

        // Both decks output to the same master channel pair.
        // Volume scaling (crossfader) provides the mix; JUCE adds both streams.
        if (applied && deckB && deckB.setOutputFirstChannel)
            deckB.setOutputFirstChannel(masterFirstChannel)

        if (applied) {
            if (deckToApply && deckToApply.getCurrentAudioSampleRate) {
                var actualSR = deckToApply.getCurrentAudioSampleRate()
                var actualBuf = deckToApply.getCurrentAudioBufferSize()
                if (actualSR > 0) {
                    pendingAudioSampleRate = actualSR
                    settingsManager.audioSampleRate = actualSR
                    audioUiSyncing = true
                    sampleRateCombo.currentIndex = indexForValue(sampleRateOptions, actualSR)
                    audioUiSyncing = false
                }
                if (actualBuf > 0) {
                    pendingAudioBufferSize = actualBuf
                    settingsManager.audioBufferSize = actualBuf
                    audioUiSyncing = true
                    bufferSizeCombo.currentIndex = indexForValue(bufferSizeOptions, actualBuf)
                    audioUiSyncing = false
                }
            }

            var note = "Applied: Sound API selected once, role devices and channels updated."
            if (isJackDeviceSelected) {
                var actualSRNote = (deckToApply && deckToApply.getCurrentAudioSampleRate)
                    ? deckToApply.getCurrentAudioSampleRate() : 0
                var actualBufNote = (deckToApply && deckToApply.getCurrentAudioBufferSize)
                    ? deckToApply.getCurrentAudioBufferSize() : 0
                note = "Applied (JACK). Buffer size and sample rate are set by the JACK server"
                    + (actualBufNote > 0 && actualSRNote > 0
                        ? " (" + actualBufNote + " samples @ " + (actualSRNote / 1000).toFixed(1) + " kHz)."
                        : ".")
            } else if ((headphonesOutputDevice && masterOutputDevice && headphonesOutputDevice !== masterOutputDevice)
                || (boothOutputDevice && masterOutputDevice && boothOutputDevice !== masterOutputDevice)) {
                note = "Applied: Pre-cue uses channel pairs on the master device. Separate devices are not yet supported."
            }
            audioApplyStatus.text = note
            audioApplyStatus.color = "#8fe388"
        } else {
            var errText = (deckToApply && deckToApply.lastAudioDeviceError)
                ? deckToApply.lastAudioDeviceError
                : "Saved, but the requested device, channels, or buffer could not be applied right now."
            audioApplyStatus.text = errText
            audioApplyStatus.color = "#ffb86c"
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── LEFT SIDEBAR ────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            color: "#141414"

            // Top: app / window title
            Rectangle {
                id: sidebarHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 48
                color: "#0f0f0f"

                Text {
                    anchors.centerIn: parent
                    text: "SETTINGS"
                    color: "#ff9900"
                    font.pixelSize: 13
                    font.bold: true
                    font.letterSpacing: 2
                }
            }

            // Separator
            Rectangle {
                anchors.top: sidebarHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: "#2a2a2a"
            }

            // Category list
            Column {
                anchors.top: sidebarHeader.bottom
                anchors.topMargin: 12
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 2

                Repeater {
                    model: settingsWindow.categories

                    delegate: Rectangle {
                        required property var modelData
                        required property int index

                        width: parent.width
                        height: 40
                        color: settingsWindow.selectedCategory === index
                               ? "#2a2a2a"
                               : containsMouse ? "#1e1e1e" : "transparent"
                        property bool containsMouse: false

                        // Active indicator bar (left edge)
                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            radius: 1
                            color: "#ff9900"
                            visible: settingsWindow.selectedCategory === index
                        }

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 18
                            spacing: 10

                            Text {
                                text: modelData.icon
                                color: settingsWindow.selectedCategory === index
                                       ? "#ff9900" : "#666"
                                font.pixelSize: 14
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Text {
                                text: modelData.label
                                color: settingsWindow.selectedCategory === index
                                       ? "#f0f0f0" : "#888"
                                font.pixelSize: 12
                                font.bold: settingsWindow.selectedCategory === index
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            hoverEnabled: true
                            onEntered: parent.containsMouse = true
                            onExited:  parent.containsMouse = false
                            onClicked: settingsWindow.selectedCategory = index
                        }
                    }
                }
            }

            // Bottom: version tag
            Button {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 34
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                height: 32
                text: "Settings-Ordner öffnen"

                background: Rectangle {
                    color: parent.down ? "#444" : "#333"
                    border.color: parent.hovered ? "#555" : "#3a3a3a"
                    radius: 4
                }

                contentItem: Text {
                    text: parent.text
                    color: "#fff"
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: {
                    if (midiManager)
                        midiManager.openSettingsDirectory()
                }
            }

            Text {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Ramsbrock DJ Engine"
                color: "#333"
                font.pixelSize: 10
                font.family: "monospace"
            }
        }

        // Sidebar / content separator
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: "#2a2a2a"
        }

        // ── RIGHT CONTENT AREA ───────────────────────────────────────────────
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: settingsWindow.selectedCategory

            // ── Page 0: Audio Setup ────────────────────────────────────────
            Item {
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"

                    ColumnLayout {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 30
                        spacing: 20

                        Text {
                            text: "Audio Setup"
                            color: "#f0f0f0"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: "#2a2a2a"
                        }

                          // Global sound API
                          RowLayout {
                              Layout.fillWidth: true
                              spacing: 16

                              Text {
                                  text: "Sound API"
                                  color: "#aaa"
                                  font.pixelSize: 12
                                  Layout.preferredWidth: 130
                              }

                              ComboBox {
                                  id: audioDeviceTypeCombo
                                  Layout.fillWidth: true
                                  model: settingsWindow.audioDeviceTypeOptions
                                  onCurrentIndexChanged: {
                                      if (settingsWindow.audioUiSyncing)
                                          return
                                      if (currentIndex >= 0 && currentIndex < settingsWindow.audioDeviceTypeOptions.length) {
                                          settingsWindow.pendingAudioDeviceType = settingsWindow.audioDeviceTypeOptions[currentIndex]
                                          settingsWindow.refreshOutputsForPendingType()
                                      }
                                  }
                              }
                          }

                          GridLayout {
                              Layout.fillWidth: true
                              columns: 3
                              columnSpacing: 12
                              rowSpacing: 10

                              Text { text: "Role"; color: "#7b7b7b"; font.pixelSize: 11 }
                              Text { text: "Device"; color: "#7b7b7b"; font.pixelSize: 11 }
                              Text { text: "Channels"; color: "#7b7b7b"; font.pixelSize: 11 }

                              Text { text: "Master"; color: "#ddd"; font.pixelSize: 12 }
                              ComboBox {
                                  id: masterOutputCombo
                                  Layout.fillWidth: true
                                  model: settingsWindow.audioOutputDeviceOptions
                                  onCurrentIndexChanged: {
                                      if (settingsWindow.audioUiSyncing)
                                          return
                                      if (currentIndex >= 0 && currentIndex < settingsWindow.audioOutputDeviceOptions.length) {
                                          settingsWindow.pendingMasterOutputDevice = settingsWindow.audioOutputDeviceOptions[currentIndex]
                                          settingsWindow.refreshRoleChannelPairs("master")
                                      }
                                  }
                              }
                              ComboBox {
                                  id: masterChannelCombo
                                  Layout.fillWidth: true
                                  model: settingsWindow.masterChannelPairOptions
                                  onCurrentIndexChanged: {
                                      if (settingsWindow.audioUiSyncing)
                                          return
                                      if (currentIndex >= 0 && currentIndex < settingsWindow.masterChannelPairOptions.length)
                                          settingsWindow.pendingMasterFirstChannel = settingsWindow.parseFirstChannel(settingsWindow.masterChannelPairOptions[currentIndex])
                                  }
                              }

                              Text { text: "Headphones"; color: "#ddd"; font.pixelSize: 12 }
                              ComboBox {
                                  id: headphonesOutputCombo
                                  Layout.fillWidth: true
                                  model: settingsWindow.audioOutputDeviceOptions
                                  onCurrentIndexChanged: {
                                      if (settingsWindow.audioUiSyncing)
                                          return
                                      if (currentIndex >= 0 && currentIndex < settingsWindow.audioOutputDeviceOptions.length) {
                                          settingsWindow.pendingHeadphonesOutputDevice = settingsWindow.audioOutputDeviceOptions[currentIndex]
                                          settingsWindow.refreshRoleChannelPairs("headphones")
                                      }
                                  }
                              }
                              ComboBox {
                                  id: headphonesChannelCombo
                                  Layout.fillWidth: true
                                  model: settingsWindow.headphonesChannelPairOptions
                                  onCurrentIndexChanged: {
                                      if (settingsWindow.audioUiSyncing)
                                          return
                                      if (currentIndex >= 0 && currentIndex < settingsWindow.headphonesChannelPairOptions.length)
                                          settingsWindow.pendingHeadphonesFirstChannel = settingsWindow.parseFirstChannel(settingsWindow.headphonesChannelPairOptions[currentIndex])
                                  }
                              }

                              Text { text: "Booth"; color: "#ddd"; font.pixelSize: 12 }
                              ComboBox {
                                  id: boothOutputCombo
                                  Layout.fillWidth: true
                                  model: settingsWindow.audioOutputDeviceOptions
                                  onCurrentIndexChanged: {
                                      if (settingsWindow.audioUiSyncing)
                                          return
                                      if (currentIndex >= 0 && currentIndex < settingsWindow.audioOutputDeviceOptions.length) {
                                          settingsWindow.pendingBoothOutputDevice = settingsWindow.audioOutputDeviceOptions[currentIndex]
                                          settingsWindow.refreshRoleChannelPairs("booth")
                                      }
                                  }
                              }
                              ComboBox {
                                  id: boothChannelCombo
                                  Layout.fillWidth: true
                                  model: settingsWindow.boothChannelPairOptions
                                  onCurrentIndexChanged: {
                                      if (settingsWindow.audioUiSyncing)
                                          return
                                      if (currentIndex >= 0 && currentIndex < settingsWindow.boothChannelPairOptions.length)
                                          settingsWindow.pendingBoothFirstChannel = settingsWindow.parseFirstChannel(settingsWindow.boothChannelPairOptions[currentIndex])
                                  }
                              }
                          }

                        // Sample Rate row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: "Sample Rate"
                                color: "#aaa"
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            ComboBox {
                                id: sampleRateCombo
                                Layout.fillWidth: true
                                height: 32
                                model: settingsWindow.sampleRateOptions
                                textRole: "label"
                                enabled: !settingsWindow.isJackDeviceSelected
                                opacity: enabled ? 1.0 : 0.4

                                contentItem: Text {
                                    text: sampleRateCombo.currentIndex >= 0 ? sampleRateCombo.displayText : "44.1 kHz"
                                    color: "#ccc"
                                    font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter
                                    leftPadding: 12
                                    elide: Text.ElideRight
                                }

                                delegate: ItemDelegate {
                                    width: sampleRateCombo.width
                                    contentItem: Text {
                                        text: modelData.label
                                        color: "#ddd"
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: highlighted ? "#3a3a3a" : "#252525"
                                    }
                                }

                                background: Rectangle {
                                    color: "#252525"
                                    border.color: "#3a3a3a"
                                    radius: 4
                                }
                            }
                        }

                        // Buffer Size row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: "Buffer Size"
                                color: "#aaa"
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            ComboBox {
                                id: bufferSizeCombo
                                Layout.fillWidth: true
                                height: 32
                                model: settingsWindow.bufferSizeOptions
                                textRole: "label"
                                enabled: !settingsWindow.isJackDeviceSelected
                                opacity: enabled ? 1.0 : 0.4

                                contentItem: Text {
                                    text: bufferSizeCombo.currentIndex >= 0 ? bufferSizeCombo.displayText : "512 samples"
                                    color: "#ccc"
                                    font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter
                                    leftPadding: 12
                                    elide: Text.ElideRight
                                }

                                delegate: ItemDelegate {
                                    width: bufferSizeCombo.width
                                    contentItem: Text {
                                        text: modelData.label
                                        color: "#ddd"
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: highlighted ? "#3a3a3a" : "#252525"
                                    }
                                }

                                background: Rectangle {
                                    color: "#252525"
                                    border.color: "#3a3a3a"
                                    radius: 4
                                }
                            }
                        }

                        Text {
                            text: settingsWindow.isJackDeviceSelected
                                ? "JACK controls buffer size and sample rate. These values are set by the JACK server (PipeWire or jackd) and cannot be changed here."
                                : "Use the lowest stable buffer your device supports. On Windows, ASIO will appear here when available; on macOS and Linux this lists the active system audio backends and outputs."
                            color: settingsWindow.isJackDeviceSelected ? "#ffb86c" : "#7b7b7b"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            Item { Layout.fillWidth: true }

                            Button {
                                text: "Apply Audio"
                                Layout.preferredWidth: 130
                                Layout.preferredHeight: 32

                                background: Rectangle {
                                    color: parent.down ? "#444" : "#2a2a2a"
                                    border.color: parent.hovered ? "#5a5a5a" : "#3a3a3a"
                                    radius: 4
                                }

                                contentItem: Text {
                                    text: parent.text
                                    color: "#f0f0f0"
                                    font.pixelSize: 12
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                onClicked: settingsWindow.applyAudioSettings()
                            }
                        }

                        Text {
                            id: audioApplyStatus
                            text: ""
                            color: "#8fe388"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // ── Page 1: MIDI Controller ────────────────────────────────────
            Item {
                id: midiSettingsPage

                onVisibleChanged: {
                    if (visible)
                        midiSettingsColumn.refreshAll()
                }

                ColumnLayout {
                    id: midiSettingsColumn
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 30
                    spacing: 20

                    property var midiDeviceList: []
                    property var mappingList: []
                    property bool hasMidiDevices: false
                    property bool hasMappings: false
                    property int availableMappingCount: 0
                    readonly property string noMappingLabel: "No Mapping (manual)"

                    ListModel { id: midiDeviceModel }
                    ListModel { id: mappingModel }

                    function updateComboSelection(comboBox, indexValue) {
                        if (!comboBox)
                            return
                        if (indexValue >= 0 && indexValue < comboBox.count)
                            comboBox.currentIndex = indexValue
                        else
                            comboBox.currentIndex = -1
                    }

                    function fillModel(listModel, values) {
                        listModel.clear()
                        for (var i = 0; i < values.length; ++i) {
                            listModel.append({ text: String(values[i]) })
                        }
                    }

                    function indexOfModelText(listModel, value) {
                        for (var i = 0; i < listModel.count; ++i) {
                            if (listModel.get(i).text === value)
                                return i
                        }
                        return -1
                    }

                    function syncFromBackend() {
                        if (midiManager) {
                            midiDeviceList = midiManager.getAvailableMidiDevices()
                            mappingList = midiManager.getAvailableMappingFiles()

                            hasMidiDevices = midiDeviceList.length > 0
                            hasMappings = mappingList.length > 0
                            availableMappingCount = mappingList.length

                            if (!hasMidiDevices)
                                midiDeviceList = ["No MIDI device found"]

                            // Always allow manual mapping without a mapping file.
                            mappingList = [noMappingLabel].concat(mappingList)

                            fillModel(midiDeviceModel, midiDeviceList)
                            fillModel(mappingModel, mappingList)

                            updateComboSelection(midiDeviceCombo, midiManager.getSelectedMidiDeviceIndex())
                            const selectedMapping = midiManager.getSelectedMapping()
                            mappingCombo.currentIndex = selectedMapping === ""
                                ? 0
                                : Math.max(0, indexOfModelText(mappingModel, selectedMapping))
                        }
                    }

                    function refreshAll() {
                        if (midiManager)
                            midiManager.refreshMidiAndMappings()
                        syncFromBackend()
                    }

                    Component.onCompleted: {
                        Qt.callLater(refreshAll)
                    }

                    Connections {
                        target: midiManager

                        function onMidiDevicesUpdated() {
                            midiSettingsColumn.syncFromBackend()
                        }

                        function onControllerListUpdated() {
                            midiSettingsColumn.syncFromBackend()
                        }

                        function onMappingListUpdated() {
                            midiSettingsColumn.syncFromBackend()
                        }
                    }

                    Text {
                        text: "MIDI Controller"
                        color: "#f0f0f0"
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#2a2a2a"
                    }

                    // Device Selection
                    RowLayout {
                        spacing: 16
                        Text {
                            text: "MIDI Device"
                            color: "#aaa"
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }
                        
                        ComboBox {
                            id: midiDeviceCombo
                            Layout.fillWidth: true
                            height: 32
                            model: midiDeviceModel
                            textRole: "text"

                            contentItem: Text {
                                text: midiDeviceCombo.currentIndex >= 0 ? midiDeviceCombo.displayText : "No MIDI device"
                                color: "#ccc"
                                font.pixelSize: 12
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 12
                                elide: Text.ElideRight
                            }

                            delegate: ItemDelegate {
                                width: midiDeviceCombo.width
                                contentItem: Text {
                                    text: model.text
                                    color: "#ddd"
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    color: highlighted ? "#3a3a3a" : "#252525"
                                }
                            }
                            
                            background: Rectangle {
                                color: "#252525"
                                border.color: "#3a3a3a"
                                radius: 0
                            }
                            
                            onActivated: {
                                if (midiManager && midiSettingsColumn.hasMidiDevices) {
                                    midiManager.selectMidiDevice(currentIndex)
                                }
                            }
                        }

                        Button {
                            text: "↻"
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            background: Rectangle {
                                color: parent.down ? "#444" : "#333"
                                border.color: parent.hovered ? "#555" : "transparent"
                                radius: 0
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#fff"
                                font.pixelSize: 16
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                midiSettingsColumn.refreshAll()
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#2a2a2a"
                    }

                    RowLayout {
                        spacing: 16
                        Text {
                            text: "Mapping File"
                            color: "#aaa"
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }

                        ComboBox {
                            id: mappingCombo
                            Layout.fillWidth: true
                            height: 32
                            model: mappingModel
                            textRole: "text"

                            contentItem: Text {
                                text: mappingCombo.currentIndex >= 0 ? mappingCombo.displayText : "No Mapping"
                                color: "#ccc"
                                font.pixelSize: 12
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 12
                                elide: Text.ElideRight
                            }

                            delegate: ItemDelegate {
                                width: mappingCombo.width
                                contentItem: Text {
                                    text: model.text
                                    color: "#ddd"
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    color: highlighted ? "#3a3a3a" : "#252525"
                                }
                            }

                            background: Rectangle {
                                color: "#252525"
                                border.color: "#3a3a3a"
                                radius: 4
                            }

                            onActivated: {
                                if (midiManager) {
                                    if (mappingCombo.currentText === midiSettingsColumn.noMappingLabel)
                                        midiManager.selectMapping("")
                                    else
                                        midiManager.selectMapping(mappingCombo.currentText)
                                }
                            }
                        }

                        Button {
                            text: "↻"
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            background: Rectangle {
                                color: parent.down ? "#444" : "#333"
                                border.color: parent.hovered ? "#555" : "transparent"
                                radius: 4
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#fff"
                                font.pixelSize: 16
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                midiSettingsColumn.refreshAll()
                            }
                        }
                    }

                    Text {
                        text: midiManager ? "Mapping-Ordner: " + midiManager.getMappingsDirectoryPath() : ""
                        color: "#777"
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }

                    Text {
                        text: "MIDI Devices: " + (midiSettingsColumn.hasMidiDevices ? midiSettingsColumn.midiDeviceList.length : 0)
                              + " | Mappings: " + midiSettingsColumn.availableMappingCount
                        color: "#666"
                        font.pixelSize: 10
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Button {
                            text: "Mappings-Ordner öffnen"
                            Layout.preferredHeight: 32

                            background: Rectangle {
                                color: parent.down ? "#444" : "#333"
                                border.color: parent.hovered ? "#555" : "transparent"
                                radius: 4
                            }

                            contentItem: Text {
                                text: parent.text
                                color: "#fff"
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: {
                                if (midiManager)
                                    midiManager.openMappingsDirectory()
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#2a2a2a"
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            text: "Mappings"
                            color: "#eee"
                            font.pixelSize: 14
                            font.bold: true
                            Layout.fillWidth: true
                        }

                        // Auto-save feedback pill
                        Rectangle {
                            id: savedPill
                            width: savedPillText.implicitWidth + 16
                            height: 20
                            radius: 3
                            color: "#1a3a1a"
                            border.color: "#2a5a2a"
                            opacity: 0.0

                            Text {
                                id: savedPillText
                                anchors.centerIn: parent
                                text: "● Gespeichert"
                                color: "#4ac84a"
                                font.pixelSize: 10
                            }

                            Connections {
                                target: midiManager
                                function onMappingUpdated() {
                                    savedPill.opacity = 1.0
                                    savedPillFade.restart()
                                }
                            }

                            NumberAnimation {
                                id: savedPillFade
                                target: savedPill
                                property: "opacity"
                                from: 1.0; to: 0.0
                                duration: 1800
                                easing.type: Easing.InQuad
                            }
                        }
                    }

                    // ── Live MIDI monitor ─────────────────────────────────────────────
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        color: "#0d0d0d"
                        border.color: "#1c1c1c"
                        radius: 4

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 6

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "MIDI IN:"
                                color: "#444"
                                font.pixelSize: 10
                                font.bold: true
                                font.letterSpacing: 0.5
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: midiManager ? (midiManager.lastMidiEvent || "–  (bewege einen Regler oder drücke eine Taste)") : "–"
                                color: midiManager && midiManager.lastMidiEvent ? "#7cdb9a" : "#333"
                                font.pixelSize: 11
                                font.family: "monospace"
                            }
                        }
                    }

                    // ── MappingRow component ──────────────────────────────────────────
                    // Declared here (outside the Flickable) so Qt 6 doesn't confuse it
                    // with the Flickable's content item.  Uses Item + anchors instead of
                    // RowLayout so TapHandler works without Flickable interference.
                    component MappingRow: Item {
                        required property string labelStr
                        required property string paramId
                        height: 28

                        property string currentMapping: midiManager
                            ? midiManager.getMappingLabel(paramId) : ""
                        property bool isLearning: false

                        Connections {
                            target: midiManager
                            function onMappingUpdated() {
                                isLearning = false
                                currentMapping = midiManager ? midiManager.getMappingLabel(paramId) : ""
                            }
                            function onLearnStarted(activeParamId) {
                                if (activeParamId !== paramId) isLearning = false
                            }
                        }

                        // Label
                        Text {
                            id: rowLabel
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: 118
                            text: labelStr
                            color: "#888"
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }

                        // Assignment badge
                        Rectangle {
                            id: rowBadge
                            anchors.left: rowLabel.right
                            anchors.leftMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            width: 72; height: 20
                            color: currentMapping !== "" ? "#0c1e11" : "transparent"
                            border.color: currentMapping !== "" ? "#194d27" : "transparent"
                            radius: 4
                            Text {
                                anchors.centerIn: parent
                                text: currentMapping
                                color: "#40b85a"
                                font.pixelSize: 10; font.bold: true
                            }
                        }

                        // Clear button (right-anchored, only when mapped)
                        Rectangle {
                            id: rowClear
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 24; height: 20
                            visible: currentMapping !== ""
                            color: xHov.hovered ? "#330d0d" : "#1c0808"
                            border.color: "#3a1414"
                            radius: 4
                            Text { anchors.centerIn: parent; text: "✕"; color: "#c04040"; font.pixelSize: 10 }
                            HoverHandler { id: xHov; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: { if (midiManager) midiManager.clearLearnedMapping(paramId) } }
                        }

                        // Learn button (fills remaining width)
                        Rectangle {
                            anchors.left: rowBadge.right
                            anchors.leftMargin: 5
                            anchors.right: currentMapping !== "" ? rowClear.left : parent.right
                            anchors.rightMargin: currentMapping !== "" ? 4 : 0
                            anchors.verticalCenter: parent.verticalCenter
                            height: 20
                            color: isLearning ? "#2a1600" : (lHov.hovered ? "#222" : "#181818")
                            border.color: isLearning ? "#d08000" : (lHov.hovered ? "#353535" : "#222")
                            radius: 4
                            Text {
                                anchors.centerIn: parent
                                text: isLearning ? "⬤  Warte auf MIDI..." : "Learn"
                                color: isLearning ? "#ff9900" : "#555"
                                font.pixelSize: 11; font.bold: isLearning
                            }
                            HoverHandler { id: lHov; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                onTapped: {
                                    if (midiManager && !isLearning) {
                                        isLearning = true
                                        midiManager.startMidiLearn(paramId)
                                    }
                                }
                            }
                        }
                    }

                    // ── Scrollable mapping list via bare Flickable ─────────────────────
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        Flickable {
                            id: mappingFlickable
                            anchors.fill: parent
                            contentWidth: width
                            contentHeight: mappingCol.implicitHeight + 12
                            flickableDirection: Flickable.VerticalFlick
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {
                                id: mScrollBar
                                width: 5
                                policy: ScrollBar.AsNeeded
                                contentItem: Rectangle { radius: 2; color: mScrollBar.active ? "#666" : "#3a3a3a" }
                                background: Rectangle { color: "transparent" }
                            }

                            Column {
                                id: mappingCol
                                width: mappingFlickable.width - 9
                                spacing: 1

                                // ── TRANSPORT ──────────────────────────────────
                                Item { width: parent.width; height: 26
                                    Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left
                                           text: "TRANSPORT"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 } }
                                MappingRow { width: parent.width; labelStr: "Deck A – Play";  paramId: "deckA_play" }
                                MappingRow { width: parent.width; labelStr: "Deck A – Cue";   paramId: "deckA_cue"  }
                                MappingRow { width: parent.width; labelStr: "Deck B – Play";  paramId: "deckB_play" }
                                MappingRow { width: parent.width; labelStr: "Deck B – Cue";   paramId: "deckB_cue"  }

                                Item { width: parent.width; height: 10 }

                                // ── HEADPHONES ───────────────────────────────
                                Item { width: parent.width; height: 26
                                    Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left
                                           text: "HEADPHONES"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 } }
                                MappingRow { width: parent.width; labelStr: "Cue A";      paramId: "deckA_headphone_cue" }
                                MappingRow { width: parent.width; labelStr: "Cue B";      paramId: "deckB_headphone_cue" }
                                MappingRow { width: parent.width; labelStr: "Master Cue"; paramId: "master_cue" }
                                MappingRow { width: parent.width; labelStr: "Cue Mix";    paramId: "headphone_mix" }

                                Item { width: parent.width; height: 10 }

                                // ── MIXER ──────────────────────────────────────
                                Item { width: parent.width; height: 26
                                    Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left
                                           text: "MIXER"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 } }
                                MappingRow { width: parent.width; labelStr: "Volume A";   paramId: "deckA_vol"   }
                                MappingRow { width: parent.width; labelStr: "Volume B";   paramId: "deckB_vol"   }
                                MappingRow { width: parent.width; labelStr: "Crossfader"; paramId: "crossfader"  }

                                Item { width: parent.width; height: 10 }

                                // ── DECK A ─────────────────────────────────────
                                Item { width: parent.width; height: 26
                                    Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left
                                           text: "DECK A — EQ & FILTER"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 } }
                                MappingRow { width: parent.width; labelStr: "Trim";    paramId: "deckA_gain"   }
                                MappingRow { width: parent.width; labelStr: "EQ High"; paramId: "deckA_eqHigh" }
                                MappingRow { width: parent.width; labelStr: "EQ Mid";  paramId: "deckA_eqMid"  }
                                MappingRow { width: parent.width; labelStr: "EQ Low";  paramId: "deckA_eqLow"  }
                                MappingRow { width: parent.width; labelStr: "Filter";  paramId: "deckA_filter" }

                                Item { width: parent.width; height: 10 }

                                // ── DECK B ─────────────────────────────────────
                                Item { width: parent.width; height: 26
                                    Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left
                                           text: "DECK B — EQ & FILTER"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 } }
                                MappingRow { width: parent.width; labelStr: "Trim";    paramId: "deckB_gain"   }
                                MappingRow { width: parent.width; labelStr: "EQ High"; paramId: "deckB_eqHigh" }
                                MappingRow { width: parent.width; labelStr: "EQ Mid";  paramId: "deckB_eqMid"  }
                                MappingRow { width: parent.width; labelStr: "EQ Low";  paramId: "deckB_eqLow"  }
                                MappingRow { width: parent.width; labelStr: "Filter";  paramId: "deckB_filter" }

                                Item { width: parent.width; height: 10 }

                                // ── TEMPO ──────────────────────────────────────
                                Item { width: parent.width; height: 26
                                    Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left
                                           text: "TEMPO"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 } }
                                MappingRow { width: parent.width; labelStr: "Tempo A"; paramId: "deckA_tempo" }
                                MappingRow { width: parent.width; labelStr: "Tempo B"; paramId: "deckB_tempo" }

                                Item { width: parent.width; height: 10 }

                                // ── JOG WHEELS ─────────────────────────────────
                                Item { width: parent.width; height: 26
                                    Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left
                                           text: "JOG WHEELS"; color: "#3a3a3a"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.5 } }
                                MappingRow { width: parent.width; labelStr: "Touch A"; paramId: "deckA_jog_touch" }
                                MappingRow { width: parent.width; labelStr: "Move A";  paramId: "deckA_jog_move"  }
                                MappingRow { width: parent.width; labelStr: "Touch B"; paramId: "deckB_jog_touch" }
                                MappingRow { width: parent.width; labelStr: "Move B";  paramId: "deckB_jog_move"  }

                                Item { width: parent.width; height: 12 }
                            }
                        }
                    }
                }
            }

            // ── Page 2: Library ────────────────────────────────────────────
            Item {
                ColumnLayout {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 30
                    spacing: 20

                    Text {
                        text: "Library"
                        color: "#f0f0f0"
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#2a2a2a"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Text {
                            text: "Music Folder"
                            color: "#aaa"
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 32
                            color: "#252525"
                            border.color: "#3a3a3a"
                            radius: 4

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                text: "~/Music"
                                color: "#ccc"
                                font.pixelSize: 12
                                font.family: "monospace"
                            }
                        }

                        Rectangle {
                            width: 70
                            height: 32
                            color: "#2a2a2a"
                            border.color: "#444"
                            radius: 4

                            Text {
                                anchors.centerIn: parent
                                text: "Browse"
                                color: "#bbb"
                                font.pixelSize: 11
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {}
                            }
                        }
                    }
                }
            }

            // ── Page 3: Legal ─────────────────────────────────────────────
            Item {
                ColumnLayout {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 30
                    spacing: 14

                    Text {
                        text: "Legal Notices"
                        color: "#f0f0f0"
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#2a2a2a"
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#cfcfcf"
                        font.pixelSize: 12
                        text: "This software is licensed under the GNU Affero General Public License v3.0 (AGPL-3.0-or-later)."
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#9f9f9f"
                        font.pixelSize: 12
                        text: "You are entitled to receive the corresponding source code under the terms of the AGPL."
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#9f9f9f"
                        font.pixelSize: 12
                        text: "Source repository: https://github.com/TimoRams/multiplatform-dj-software"
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#9f9f9f"
                        font.pixelSize: 12
                        text: "License and third-party notices are documented in the NOTICE file."
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Button {
                            text: "Open Project Source"
                            Layout.preferredHeight: 32

                            background: Rectangle {
                                color: parent.down ? "#444" : "#333"
                                border.color: parent.hovered ? "#555" : "transparent"
                                radius: 4
                            }

                            contentItem: Text {
                                text: parent.text
                                color: "#fff"
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: Qt.openUrlExternally("https://github.com/TimoRams/multiplatform-dj-software")
                        }

                        Button {
                            text: "Open AGPL License"
                            Layout.preferredHeight: 32

                            background: Rectangle {
                                color: parent.down ? "#444" : "#333"
                                border.color: parent.hovered ? "#555" : "transparent"
                                radius: 4
                            }

                            contentItem: Text {
                                text: parent.text
                                color: "#fff"
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: Qt.openUrlExternally("https://www.gnu.org/licenses/agpl-3.0.html")
                        }
                    }
                }
            }
        }
    }
}
