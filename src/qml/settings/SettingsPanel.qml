import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

Item {
    id: settingsWindow
    implicitWidth: 800
    implicitHeight: 600
    property bool active: visible

    onActiveChanged: {
        if (active) {
            if (!audioSyncPending) {
                audioSyncPending = true
                audioSyncTimer.start()
            }
            audioDeviceListRetryCount = 0
            audioDeviceListTimer.start()
        } else {
            audioSyncTimer.stop()
            audioDeviceListTimer.stop()
            audioSyncPending = false
        }
    }

    // Device enumeration can settle after this panel already populated its
    // combo boxes once (external interfaces on macOS/CoreAudio, or backends
    // that only become ready shortly after launch). Rather than guessing a
    // fixed timeout, refresh live whenever the backend actually confirms a
    // device configuration — this also covers the bootstrap retries that run
    // a few seconds after a failed startup device restore.
    Connections {
        target: deckA
        function onAudioDeviceConfigurationChanged() {
            settingsWindow.refreshAudioDeviceLists()
        }
    }

    // ── Navigation categories ────────────────────────────────────────────────
    property int selectedCategory: 0
    property int pendingAudioSampleRate: 44100
    property int pendingAudioBufferSize: 512
    property string pendingAudioDeviceType: ""
    property bool audioUiSyncing: false
    property bool audioSyncPending: false
    property var outputChannelPairsCache: ({})
    property var mappingEditorInstance: null

    readonly property var audioRoleDefinitions: [
        {
            key: "master",
            label: "Master",
            outputDeviceProperty: "pendingMasterOutputDevice",
            firstChannelProperty: "pendingMasterFirstChannel",
            channelPairsProperty: "masterChannelPairOptions"
        },
        {
            key: "headphones",
            label: "Headphones",
            outputDeviceProperty: "pendingHeadphonesOutputDevice",
            firstChannelProperty: "pendingHeadphonesFirstChannel",
            channelPairsProperty: "headphonesChannelPairOptions"
        },
        {
            key: "booth",
            label: "Booth",
            outputDeviceProperty: "pendingBoothOutputDevice",
            firstChannelProperty: "pendingBoothFirstChannel",
            channelPairsProperty: "boothChannelPairOptions"
        }
    ]

    component AudioRoleRow: RowLayout {
        id: audioRoleRow
        required property var modelData
        property var roleDefinition: modelData
        Layout.fillWidth: true
        Layout.columnSpan: 3
        spacing: 12

        property alias outputCombo: outputCombo
        property alias channelCombo: channelCombo

        Text {
            Layout.preferredWidth: 76
            text: audioRoleRow.roleDefinition.label
            color: UiTheme.textSecondary
            font.pixelSize: 12
        }

        ComboBox {
            id: outputCombo
            Layout.fillWidth: true
            model: settingsWindow.audioOutputDeviceOptions
            onCurrentIndexChanged: {
                if (settingsWindow.audioUiSyncing)
                    return
                if (currentIndex >= 0 && currentIndex < settingsWindow.audioOutputDeviceOptions.length) {
                    settingsWindow.setRoleSelections(
                        audioRoleRow.roleDefinition.key,
                        settingsWindow.audioOutputDeviceOptions[currentIndex],
                        settingsWindow.getRoleSelections(audioRoleRow.roleDefinition.key).firstChannel)
                    settingsWindow.refreshRoleChannelPairs(audioRoleRow.roleDefinition.key)
                }
            }
        }

        ComboBox {
            id: channelCombo
            Layout.fillWidth: true
            model: settingsWindow[audioRoleRow.roleDefinition.channelPairsProperty]
            onCurrentIndexChanged: {
                if (settingsWindow.audioUiSyncing)
                    return
                var options = settingsWindow[audioRoleRow.roleDefinition.channelPairsProperty]
                if (currentIndex >= 0 && currentIndex < options.length) {
                    var selections = settingsWindow.getRoleSelections(audioRoleRow.roleDefinition.key)
                    settingsWindow.setRoleSelections(
                        audioRoleRow.roleDefinition.key,
                        selections.outputDevice,
                        settingsWindow.parseFirstChannel(options[currentIndex]))
                }
            }
        }

    }

    Component.onDestruction: {
        if (settingsWindow.mappingEditorInstance) {
            settingsWindow.mappingEditorInstance.destroy()
            settingsWindow.mappingEditorInstance = null
        }
    }

    function showMappingEditor() {
        if (!mappingEditorInstance)
            mappingEditorInstance = mappingEditorFactory.createObject(null)
        if (!mappingEditorInstance)
            return
        mappingEditorInstance.show()
        mappingEditorInstance.raise()
        mappingEditorInstance.requestActivate()
    }

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

    // Backup refresh in case device enumeration (CoreAudio/ALSA/JACK) wasn't
    // complete yet when the panel opened — some external interfaces take a
    // moment to enumerate after launch. Retries a bounded number of times
    // instead of a single fixed 500ms guess, and stops as soon as real
    // outputs (not just "None") show up.
    property int audioDeviceListRetryCount: 0
    Timer {
        id: audioDeviceListTimer
        interval: 700
        repeat: true
        onTriggered: {
            settingsWindow.refreshAudioDeviceLists()
            settingsWindow.audioDeviceListRetryCount += 1
            const hasRealOutputs = settingsWindow.audioOutputDeviceOptions.length > 1
            if (hasRealOutputs || settingsWindow.audioDeviceListRetryCount >= 6)
                audioDeviceListTimer.stop()
        }
    }

    readonly property var categories: [
        { label: "Audio Setup",     icon: "♪" },
        { label: "MIDI Controller", icon: "⎘" },
        { label: "Library",         icon: "☰" },
        { label: "DJ / Sync",       icon: "⟳" },
        { label: "Legal",           icon: "§" },
    ]

    readonly property bool isJackDeviceSelected: String(pendingAudioDeviceType).toLowerCase().indexOf("jack") >= 0

    readonly property var sampleRateOptions: [
        { label: "44.1 kHz", value: 44100 },
        { label: "48 kHz", value: 48000 },
        { label: "88.2 kHz", value: 88200 },
        { label: "96 kHz", value: 96000 }
    ]

    readonly property var standardBufferSizeOptions: [
        { label: "64 samples", value: 64 },
        { label: "128 samples", value: 128 },
        { label: "256 samples (low latency)", value: 256 },
        { label: "512 samples (safe)", value: 512 },
        { label: "1024 samples", value: 1024 },
        { label: "2048 samples", value: 2048 },
        { label: "4096 samples", value: 4096 }
    ]

    readonly property var jackBufferSizeOptions: [
        { label: "64 frames/period", value: 64 },
        { label: "128 frames/period", value: 128 },
        { label: "256 frames/period", value: 256 },
        { label: "512 frames/period", value: 512 },
        { label: "1024 frames/period", value: 1024 },
        { label: "2048 frames/period", value: 2048 },
        { label: "4096 frames/period", value: 4096 }
    ]

    readonly property var bufferSizeOptions: isJackDeviceSelected
        ? jackBufferSizeOptions
        : standardBufferSizeOptions

    function indexForValue(options, value) {
        for (var i = 0; i < options.length; ++i) {
            if (options[i].value === value)
                return i
            if (options[i].minValue !== undefined && options[i].maxValue !== undefined
                    && value >= options[i].minValue && value <= options[i].maxValue)
                return i
        }
        return 0
    }

    function indexForText(options, value) {
        for (var i = 0; i < options.length; ++i) {
            if (String(options[i]).toLowerCase() === String(value).toLowerCase())
                return i
        }
        return -1
    }

    function firstRealOutput(options) {
        for (var i = 0; i < options.length; ++i) {
            if (String(options[i]).toLowerCase() !== "none")
                return String(options[i])
        }
        return "None"
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

        var options = (deckA && deckA.getAvailableOutputChannelPairs)
            ? deckA.getAvailableOutputChannelPairs(pendingAudioDeviceType, outputDevice)
            : []
        if (!options || options.length === 0)
            options = ["None", "1-2"]

        outputChannelPairsCache[key] = options
        return options
    }

    function getRoleSelections(role) {
        var definition = roleDefinition(role)
        return {
            outputDevice: settingsWindow[definition.outputDeviceProperty],
            firstChannel: settingsWindow[definition.firstChannelProperty]
        }
    }

    function setRoleSelections(role, outputDevice, firstChannel) {
        var definition = roleDefinition(role)
        settingsWindow[definition.outputDeviceProperty] = outputDevice
        settingsWindow[definition.firstChannelProperty] = firstChannel
    }

    function roleDefinition(role) {
        for (var i = 0; i < audioRoleDefinitions.length; ++i) {
            if (audioRoleDefinitions[i].key === role)
                return audioRoleDefinitions[i]
        }
        throw new Error("Unknown audio output role: " + role)
    }

    function roleRow(role) {
        return audioRoleRepeater.itemAt(audioRoleDefinitions.indexOf(roleDefinition(role)))
    }

    function selectedRoleSelections(role) {
        var selections = getRoleSelections(role)
        var row = roleRow(role)
        if (!row)
            return selections

        if (row.outputCombo.currentIndex >= 0
                && row.outputCombo.currentIndex < audioOutputDeviceOptions.length)
            selections.outputDevice = audioOutputDeviceOptions[row.outputCombo.currentIndex]

        var definition = roleDefinition(role)
        var pairOptions = settingsWindow[definition.channelPairsProperty]
        if (row.channelCombo.currentIndex >= 0
                && row.channelCombo.currentIndex < pairOptions.length)
            selections.firstChannel = parseFirstChannel(pairOptions[row.channelCombo.currentIndex])
        return selections
    }

    function refreshRoleOutputAndPairs(role) {
        if (!deckA || !deckA.getAvailableAudioOutputDevices)
            return

        var selections = getRoleSelections(role)
        var outputOptions = audioOutputDeviceOptions
        if (!outputOptions || outputOptions.length === 0)
            outputOptions = ["None"]

        // Only override the saved device when the list actually has real devices.
        // If the list contains only "None" the device scan is still incomplete —
        // preserve the saved name so it is found on the next (500 ms) refresh.
        var listHasRealDevices = outputOptions.length > 1 ||
            (outputOptions.length === 1 && String(outputOptions[0]).toLowerCase() !== "none")
        if (listHasRealDevices && indexForText(outputOptions, selections.outputDevice) < 0)
            selections.outputDevice = firstRealOutput(outputOptions)

        setRoleSelections(role, selections.outputDevice, selections.firstChannel)

        audioUiSyncing = true
        var row = roleRow(role)
        if (row)
            row.outputCombo.currentIndex = Math.max(0, indexForText(outputOptions, selections.outputDevice))
        refreshRoleChannelPairs(role)
        audioUiSyncing = false
    }

    function refreshRoleChannelPairs(role) {
        if (!deckA || !deckA.getAvailableOutputChannelPairs)
            return

        var selections = getRoleSelections(role)
        var pairOptions = getOutputPairOptions(selections.outputDevice)

        var pairText = pairTextForFirstChannel(pairOptions, selections.firstChannel)
        if (role === "master"
                && String(selections.outputDevice).toLowerCase() !== "none"
                && String(selections.outputDevice) !== ""
                && pairText === "None") {
            pairText = pairOptions.length > 1 ? String(pairOptions[1]) : "1-2"
        }
        var firstChannel = parseFirstChannel(pairText)
        setRoleSelections(role, selections.outputDevice, firstChannel)

        var definition = roleDefinition(role)
        audioUiSyncing = true
        settingsWindow[definition.channelPairsProperty] = pairOptions
        var row = roleRow(role)
        if (row)
            row.channelCombo.currentIndex = Math.max(0, indexForText(pairOptions, pairText))
        audioUiSyncing = false
    }

    function refreshOutputsForPendingType() {
        if (!deckA || !deckA.getAvailableAudioOutputDevices)
            return

        outputChannelPairsCache = ({})

        // Assigning a ComboBox model changes currentIndex synchronously. Guard
        // the assignment itself so index 0 ("None") cannot overwrite a saved
        // device before we restore the intended index below.
        audioUiSyncing = true
        audioOutputDeviceOptions = settingsManager && settingsManager.getAvailableAudioOutputDevices
            ? settingsManager.getAvailableAudioOutputDevices(pendingAudioDeviceType)
            : deckA.getAvailableAudioOutputDevices(pendingAudioDeviceType)
        if (!audioOutputDeviceOptions || audioOutputDeviceOptions.length === 0)
            audioOutputDeviceOptions = ["None"]

        refreshRoleOutputAndPairs("master")
        refreshRoleOutputAndPairs("headphones")
        refreshRoleOutputAndPairs("booth")
        audioUiSyncing = false
    }

    function refreshAudioDeviceLists() {
        if (!deckA || !deckA.getAvailableAudioDeviceTypes) {
            audioDeviceTypeOptions = pendingAudioDeviceType ? [pendingAudioDeviceType] : [""]
            audioOutputDeviceOptions = pendingMasterOutputDevice ? [pendingMasterOutputDevice] : ["None"]
            masterChannelPairOptions = ["None", "1-2"]
            headphonesChannelPairOptions = ["None", "1-2"]
            boothChannelPairOptions = ["None", "1-2"]
            return
        }

        // The model assignment can emit currentIndexChanged immediately.
        audioUiSyncing = true
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

        var masterSelections = selectedRoleSelections("master")
        var masterOutputDevice = masterSelections.outputDevice
        var masterFirstChannel = masterSelections.firstChannel
        var headphonesSelections = selectedRoleSelections("headphones")
        var headphonesOutputDevice = headphonesSelections.outputDevice
        var headphonesFirstChannel = headphonesSelections.firstChannel
        var boothSelections = selectedRoleSelections("booth")
        var boothOutputDevice = boothSelections.outputDevice
        var boothFirstChannel = boothSelections.firstChannel

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

        // Persist the complete routing atomically. The old property-by-property
        // path forced a synchronous XML write and signal emission for every field.
        settingsManager.setAudioConfiguration(deviceType,
                                              masterOutputDevice,
                                              masterFirstChannel,
                                              headphonesOutputDevice,
                                              headphonesFirstChannel,
                                              boothOutputDevice,
                                              boothFirstChannel,
                                              sampleRate,
                                              bufferSize)

        var deckToApply = deckA && deckA.applyAudioDeviceSettings ? deckA
            : (deckB && deckB.applyAudioDeviceSettings ? deckB : null)

        if (!deckToApply) {
            audioApplyStatus.text = "Audio engine is still starting. Open Settings again in a moment."
            audioApplyStatus.color = UiTheme.warning
            return
        }

        if (deviceType && String(deviceType).toLowerCase().indexOf("jack") >= 0
            && deckToApply && deckToApply.isJackServerRunning && !deckToApply.isJackServerRunning()) {
            audioApplyStatus.text = deckToApply.jackServerStatus
                ? deckToApply.jackServerStatus()
                : "JACK server not running. Start PipeWire-JACK or jackd."
            audioApplyStatus.color = UiTheme.warning
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

        if (applied) {
            if (deckToApply && deckToApply.getCurrentAudioSampleRate) {
                var actualSR = deckToApply.getCurrentAudioSampleRate()
                var actualBuf = deckToApply.getCurrentAudioBufferSize()
                if (actualSR > 0) {
                    audioUiSyncing = true
                    sampleRateCombo.currentIndex = indexForValue(sampleRateOptions, actualSR)
                    audioUiSyncing = false
                }
                if (actualBuf > 0) {
                    audioUiSyncing = true
                    bufferSizeCombo.currentIndex = indexForValue(bufferSizeOptions, actualBuf)
                    audioUiSyncing = false
                }
            }

            var warningText = (deckToApply && deckToApply.audioDeviceFallbackMessage)
                ? deckToApply.audioDeviceFallbackMessage : ""
            var note = "Applied: Sound API selected once, role devices and channels updated."
            if (isJackDeviceSelected) {
                var actualSRNote = (deckToApply && deckToApply.getCurrentAudioSampleRate)
                    ? deckToApply.getCurrentAudioSampleRate() : 0
                var actualBufNote = (deckToApply && deckToApply.getCurrentAudioBufferSize)
                    ? deckToApply.getCurrentAudioBufferSize() : 0
                var jackState = actualBufNote > 0 && actualSRNote > 0
                    ? actualBufNote + " frames @ " + (actualSRNote / 1000).toFixed(1) + " kHz"
                    : ""
                if (warningText) {
                    note = warningText + (jackState ? " Current JACK setting: " + jackState + "." : "")
                } else {
                    note = "Applied (JACK). Sample rate follows the JACK server; frames/period request applied"
                        + (jackState ? " (" + jackState + ")." : ".")
                }
            } else if (warningText) {
                note = warningText
            } else if ((headphonesOutputDevice && masterOutputDevice && headphonesOutputDevice !== masterOutputDevice)
                || (boothOutputDevice && masterOutputDevice && boothOutputDevice !== masterOutputDevice)) {
                note = "Applied: Pre-cue uses channel pairs on the master device. Separate devices are not yet supported."
            }
            audioApplyStatus.text = note
            audioApplyStatus.color = warningText ? UiTheme.warning : UiTheme.green
        } else {
            var errText = (deckToApply && deckToApply.lastAudioDeviceError)
                ? deckToApply.lastAudioDeviceError
                : "Saved, but the requested device, channels, or buffer could not be applied right now."
            audioApplyStatus.text = errText
            audioApplyStatus.color = UiTheme.warning
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── LEFT SIDEBAR ────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            color: UiTheme.panel

            // Top: app / window title
            Rectangle {
                id: sidebarHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 48
                color: UiTheme.panelInset

                Text {
                    anchors.centerIn: parent
                    text: "SETTINGS"
                    color: UiTheme.textPrimary
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
                color: UiTheme.separatorSubtle
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
                               ? UiTheme.blueDim
                               : containsMouse ? UiTheme.panelRaised : "transparent"
                        property bool containsMouse: false

                        // Active indicator bar (left edge)
                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            radius: 0
                            color: UiTheme.blue
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
                                       ? UiTheme.blue : UiTheme.textLabel
                                font.pixelSize: 14
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Text {
                                text: modelData.label
                                color: settingsWindow.selectedCategory === index
                                       ? UiTheme.textPrimary : UiTheme.textSecondary
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
                    color: UiTheme.buttonBg(false, parent.hovered, parent.down)
                    border.color: parent.hovered ? UiTheme.borderHover : UiTheme.border
                    radius: 0
                }

                contentItem: Text {
                    text: parent.text
                    color: UiTheme.textPrimary
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
                text: "Brock DJ Engine"
                color: UiTheme.textMuted
                font.pixelSize: 10
                font.family: UiTheme.numericFontFamily
            }
        }

        // Sidebar / content separator
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: UiTheme.separatorSubtle
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

                    // A touchscreen must never require sideways scrolling to
                    // reach controls: contentWidth is pinned to the viewport
                    // width (never to the layout's own implicit width) and the
                    // horizontal scrollbar/flick is disabled outright, so
                    // overflow can only ever resolve vertically.
                    ScrollView {
                        anchors.fill: parent
                        anchors.margins: 30
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: parent.width
                        spacing: 20

                        Text {
                            text: "Audio Setup"
                            color: UiTheme.textPrimary
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: UiTheme.separatorSubtle
                        }

                          // Global sound API
                          RowLayout {
                              Layout.fillWidth: true
                              spacing: 16

                              Text {
                                  text: "Sound API"
                                  color: UiTheme.textLabel
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
                                          settingsWindow.audioUiSyncing = true
                                          sampleRateCombo.currentIndex = settingsWindow.indexForValue(settingsWindow.sampleRateOptions, settingsWindow.pendingAudioSampleRate)
                                          bufferSizeCombo.currentIndex = settingsWindow.indexForValue(settingsWindow.bufferSizeOptions, settingsWindow.pendingAudioBufferSize)
                                          settingsWindow.audioUiSyncing = false
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

                              Text { text: "Role"; color: UiTheme.textDim; font.pixelSize: 11 }
                              Text { text: "Device"; color: UiTheme.textDim; font.pixelSize: 11 }
                              Text { text: "Channels"; color: UiTheme.textDim; font.pixelSize: 11 }

                              Repeater {
                                  id: audioRoleRepeater
                                  model: settingsWindow.audioRoleDefinitions
                                  delegate: AudioRoleRow { }
                              }
                          }

                        // Sample Rate row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: settingsWindow.isJackDeviceSelected ? "Sample Rate (JACK)" : "Sample Rate"
                                color: UiTheme.textLabel
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
                                    text: settingsWindow.isJackDeviceSelected
                                          ? (settingsWindow.pendingAudioSampleRate > 0
                                             ? "JACK server (" + (settingsWindow.pendingAudioSampleRate / 1000).toFixed(1) + " kHz)"
                                             : "JACK server")
                                          : (sampleRateCombo.currentIndex >= 0 ? sampleRateCombo.displayText : "44.1 kHz")
                                    color: UiTheme.textSecondary
                                    font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter
                                    leftPadding: 12
                                    elide: Text.ElideRight
                                }

                                delegate: ItemDelegate {
                                    width: sampleRateCombo.width
                                    contentItem: Text {
                                        text: modelData.label
                                        color: UiTheme.textSecondary
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: highlighted ? UiTheme.surfaceRaised : UiTheme.surfaceInset
                                    }
                                }

                                background: Rectangle {
                                    color: UiTheme.surfaceInset
                                    border.color: UiTheme.border
                                    radius: 0
                                }
                            }
                        }

                        // Buffer Size row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: settingsWindow.isJackDeviceSelected ? "Frames / Period" : "Buffer Size"
                                color: UiTheme.textLabel
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            ComboBox {
                                id: bufferSizeCombo
                                Layout.fillWidth: true
                                height: 32
                                model: settingsWindow.bufferSizeOptions
                                textRole: "label"
                                enabled: true
                                opacity: 1.0

                                contentItem: Text {
                                    text: bufferSizeCombo.currentIndex >= 0 ? bufferSizeCombo.displayText : "512 samples"
                                    color: UiTheme.textSecondary
                                    font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter
                                    leftPadding: 12
                                    elide: Text.ElideRight
                                }

                                delegate: ItemDelegate {
                                    width: bufferSizeCombo.width
                                    contentItem: Text {
                                        text: modelData.label
                                        color: UiTheme.textSecondary
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: highlighted ? UiTheme.surfaceRaised : UiTheme.surfaceInset
                                    }
                                }

                                background: Rectangle {
                                    color: UiTheme.surfaceInset
                                    border.color: UiTheme.border
                                    radius: 0
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: "Keylock Engine"
                                color: UiTheme.textLabel
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            ComboBox {
                                id: timeStretchBackendCombo
                                Layout.fillWidth: true
                                height: 32
                                model: ["Signalsmith (Standard)", "Rubber Band"]
                                currentIndex: settingsManager && settingsManager.timeStretchBackend === "rubberband" ? 1 : 0
                                onActivated: {
                                    if (settingsManager)
                                        settingsManager.timeStretchBackend = currentIndex === 1 ? "rubberband" : "signalsmith"
                                }
                            }
                        }

                        Text {
                            text: "Signalsmith is the default keylock engine. The selection applies to all decks immediately; Rubber Band remains available for compatibility."
                            color: UiTheme.textDim
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Text {
                            text: settingsWindow.isJackDeviceSelected
                                ? "JACK sample rate follows the server. Frames/period is requested from JACK/PipeWire and the actual opened value is shown after Apply."
                                : "Use the lowest stable buffer your device supports. On Windows, ASIO will appear here when available; on macOS and Linux this lists the active system audio backends and outputs."
                            color: settingsWindow.isJackDeviceSelected ? UiTheme.warning : UiTheme.textDim
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
                                    color: UiTheme.buttonBg(false, parent.hovered, parent.down)
                                    border.color: parent.hovered ? UiTheme.borderHover : UiTheme.border
                                    radius: 0
                                }

                                contentItem: Text {
                                    text: parent.text
                                    color: UiTheme.textPrimary
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
                            color: UiTheme.green
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
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

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 30
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    id: midiSettingsColumn
                    width: parent.width
                    spacing: 20

                    property var midiDeviceList: []
                    property var midiOutputDeviceList: []
                    property var mappingList: []
                    property bool hasMidiDevices: false
                    property bool hasMidiOutputs: false
                    property bool hasMappings: false
                    property int availableMappingCount: 0
                    readonly property string noMappingLabel: "No Mapping (manual)"

                    ListModel { id: midiDeviceModel }
                    ListModel { id: midiOutputDeviceModel }
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
                            midiOutputDeviceList = midiManager.getAvailableMidiOutputDevices()
                            mappingList = midiManager.getAvailableMappingFiles()

                            hasMidiDevices = midiDeviceList.length > 0
                            hasMidiOutputs = midiOutputDeviceList.length > 0
                            hasMappings = mappingList.length > 0
                            availableMappingCount = mappingList.length

                            if (!hasMidiDevices)
                                midiDeviceList = ["No MIDI device found"]
                            if (!hasMidiOutputs)
                                midiOutputDeviceList = ["No MIDI output found"]

                            // Always allow manual mapping without a mapping file.
                            mappingList = [noMappingLabel].concat(mappingList)

                            fillModel(midiDeviceModel, midiDeviceList)
                            fillModel(midiOutputDeviceModel, midiOutputDeviceList)
                            fillModel(mappingModel, mappingList)

                            updateComboSelection(midiDeviceCombo, midiManager.getSelectedMidiDeviceIndex())
                            updateComboSelection(midiOutputDeviceCombo, midiManager.getSelectedMidiOutputIndex())
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
                        color: UiTheme.textPrimary
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: UiTheme.separatorSubtle
                    }

                    RowLayout {
                        visible: false
                        Layout.preferredHeight: 0
                        spacing: 16
                        Text {
                            text: "MIDI Output"
                            color: UiTheme.textLabel
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }

                        ComboBox {
                            id: midiOutputDeviceCombo
                            Layout.fillWidth: true
                            height: 32
                            model: midiOutputDeviceModel
                            textRole: "text"

                            contentItem: Text {
                                text: midiOutputDeviceCombo.currentIndex >= 0 ? midiOutputDeviceCombo.displayText : "No MIDI output"
                                color: UiTheme.textSecondary
                                font.pixelSize: 12
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 12
                                elide: Text.ElideRight
                            }

                            delegate: ItemDelegate {
                                width: midiOutputDeviceCombo.width
                                contentItem: Text {
                                    text: model.text
                                    color: UiTheme.textSecondary
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    color: highlighted ? UiTheme.surfaceRaised : UiTheme.surfaceInset
                                }
                            }

                            background: Rectangle {
                                color: UiTheme.surfaceInset
                                border.color: UiTheme.border
                                radius: 0
                            }

                            onActivated: {
                                if (midiManager && midiSettingsColumn.hasMidiOutputs)
                                    midiManager.selectMidiOutputDevice(currentIndex)
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: UiTheme.separatorSubtle
                    }

                    RowLayout {
                        spacing: 16

                        Text {
                            text: "Integrated Controller"
                            color: UiTheme.textLabel
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            CheckBox {
                                id: flx10SupportCheckBox
                                text: "Enable DDJ-FLX10 HID jog display support"
                                checked: settingsManager ? settingsManager.flx10ControllerSupportEnabled : false
                                onToggled: {
                                    if (settingsManager)
                                        settingsManager.flx10ControllerSupportEnabled = checked
                                }
                            }

                            Text {
                                text: controllerManager ? controllerManager.flx10Status : ""
                                color: controllerManager && controllerManager.flx10Connected ? UiTheme.green : UiTheme.textLabel
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: UiTheme.separatorSubtle
                    }

                    // Device Selection
                    RowLayout {
                        spacing: 16
                        Text {
                            text: "Controller Device"
                            color: UiTheme.textLabel
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
                                color: UiTheme.textSecondary
                                font.pixelSize: 12
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 12
                                elide: Text.ElideRight
                            }

                            delegate: ItemDelegate {
                                width: midiDeviceCombo.width
                                contentItem: Text {
                                    text: model.text
                                    color: UiTheme.textSecondary
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    color: highlighted ? UiTheme.surfaceRaised : UiTheme.surfaceInset
                                }
                            }
                            
                            background: Rectangle {
                                color: UiTheme.surfaceInset
                                border.color: UiTheme.border
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
                                color: UiTheme.buttonBg(false, parent.hovered, parent.down)
                                border.color: parent.hovered ? UiTheme.borderHover : "transparent"
                                radius: 0
                            }
                            contentItem: Text {
                                text: parent.text
                                color: UiTheme.textPrimary
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
                        color: UiTheme.separatorSubtle
                    }

                    RowLayout {
                        spacing: 16
                        Text {
                            text: "Mapping File"
                            color: UiTheme.textLabel
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
                                color: UiTheme.textSecondary
                                font.pixelSize: 12
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 12
                                elide: Text.ElideRight
                            }

                            delegate: ItemDelegate {
                                width: mappingCombo.width
                                contentItem: Text {
                                    text: model.text
                                    color: UiTheme.textSecondary
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    color: highlighted ? UiTheme.surfaceRaised : UiTheme.surfaceInset
                                }
                            }

                            background: Rectangle {
                                color: UiTheme.surfaceInset
                                border.color: UiTheme.border
                                radius: 0
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
                                color: UiTheme.buttonBg(false, parent.hovered, parent.down)
                                border.color: parent.hovered ? UiTheme.borderHover : "transparent"
                                radius: 0
                            }
                            contentItem: Text {
                                text: parent.text
                                color: UiTheme.textPrimary
                                font.pixelSize: 16
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                midiSettingsColumn.refreshAll()
                            }
                        }
                    }

                    RowLayout {
                        spacing: 16
                        Text {
                            text: "LED Tests"
                            color: UiTheme.textLabel
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Button {
                                text: "Raw LEDs"
                                enabled: midiSettingsColumn.hasMidiDevices
                                Layout.preferredWidth: 96
                                Layout.preferredHeight: 32
                                onClicked: {
                                    if (midiManager)
                                        midiManager.testFlx10LedOutput()
                                }
                            }

                            Button {
                                text: "Hotcue Pads"
                                enabled: midiSettingsColumn.hasMidiDevices
                                Layout.preferredWidth: 112
                                Layout.preferredHeight: 32
                                onClicked: {
                                    if (midiManager)
                                        midiManager.sendFlx10HotcuePaletteTest()
                                }
                            }
                        }
                    }

                    Text {
                        text: midiManager ? "Mapping-Ordner: " + midiManager.getMappingsDirectoryPath() : ""
                        color: UiTheme.textDim
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }

                    Text {
                        text: "MIDI Devices: " + (midiSettingsColumn.hasMidiDevices ? midiSettingsColumn.midiDeviceList.length : 0)
                              + " | Mappings: " + midiSettingsColumn.availableMappingCount
                        color: UiTheme.textMuted
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
                                color: UiTheme.buttonBg(false, parent.hovered, parent.down)
                                border.color: parent.hovered ? UiTheme.borderHover : "transparent"
                                radius: 0
                            }

                            contentItem: Text {
                                text: parent.text
                                color: UiTheme.textPrimary
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
                        color: UiTheme.separatorSubtle
                    }

                    // ── Live MIDI monitor ─────────────────────────────────────────────
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        color: UiTheme.surfaceInset
                        border.color: UiTheme.border
                        radius: 0

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 6

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "MIDI IN:"
                                color: UiTheme.textDim
                                font.pixelSize: 10
                                font.bold: true
                                font.letterSpacing: 0.5
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: midiManager ? (midiManager.lastMidiEvent || "–  (bewege einen Regler oder drücke eine Taste)") : "–"
                                color: midiManager && midiManager.lastMidiEvent ? UiTheme.green : UiTheme.textMuted
                                font.pixelSize: 11
                                font.family: UiTheme.numericFontFamily
                            }
                        }
                    }

                    // ── Open mapping editor ────────────────────────────────────────────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Item { Layout.fillWidth: true }

                        Button {
                            text: "Mappings bearbeiten"
                            Layout.preferredHeight: 34

                            background: Rectangle {
                                color: UiTheme.buttonBg(false, parent.hovered, parent.down)
                                border.color: parent.hovered ? UiTheme.borderHover : UiTheme.border
                                radius: 0
                            }

                            contentItem: Text {
                                text: parent.text
                                color: UiTheme.blue
                                font.pixelSize: 12
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: settingsWindow.showMappingEditor()
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
                }
            }

            Component {
                id: mappingEditorFactory
                MappingEditorWindow { }
            }

            // ── Page 2: Library ────────────────────────────────────────────
            Item {
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 30
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: parent.width
                    spacing: 20

                    Text {
                        text: "Library"
                        color: UiTheme.textPrimary
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: UiTheme.separatorSubtle
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Text {
                            text: "Music Folder"
                            color: UiTheme.textLabel
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 32
                            color: UiTheme.surfaceInset
                            border.color: UiTheme.border
                            radius: 0

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                text: "~/Music"
                                color: UiTheme.textSecondary
                                font.pixelSize: 12
                                font.family: UiTheme.numericFontFamily
                            }
                        }

                        Rectangle {
                            width: 70
                            height: 32
                            color: UiTheme.buttonBg(false, false, false)
                            border.color: UiTheme.border
                            radius: 0

                            Text {
                                anchors.centerIn: parent
                                text: "Browse"
                                color: UiTheme.textSecondary
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
            }

            // ── Page 3: DJ / Sync ─────────────────────────────────────────
            Item {
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 30
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        Text {
                            text: "DJ / Sync"
                            color: UiTheme.textPrimary
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: UiTheme.separatorSubtle
                        }

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: UiTheme.textSecondary
                            font.pixelSize: 12
                            text: "When the same file plays on two synced decks, waveforms may align on the beat grid but not sample-for-sample. Summing them in the mixer can cause comb filtering (thin/hollow bass). Nudge one deck, use EQ, or polarity invert (−) on one channel."
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            Text {
                                text: "Tight Double (same-file sample align)"
                                color: UiTheme.textPrimary
                                font.pixelSize: 12
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }

                            Switch {
                                id: tightDoubleSwitch
                                checked: typeof settingsManager !== "undefined"
                                         && settingsManager
                                         ? settingsManager.tightDoubleSync
                                         : false
                                onToggled: {
                                    if (typeof settingsManager !== "undefined" && settingsManager)
                                        settingsManager.tightDoubleSync = checked
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: UiTheme.textDim
                            font.pixelSize: 11
                            text: "Optional: when SYNC is on and two decks share the same file, the follower trims transport position toward the master (including keylock latency). Off by default — normal beat/bar sync is unchanged."
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            Text {
                                text: "Waveform rendering"
                                color: UiTheme.textPrimary
                                font.pixelSize: 12
                                Layout.fillWidth: true
                            }

                            ComboBox {
                                id: waveformRenderStyleCombo
                                Layout.preferredWidth: 180
                                model: ["RGB", "EQ Color", "3-Band"]
                                currentIndex: (typeof settingsManager !== "undefined"
                                               && settingsManager)
                                              ? settingsManager.waveformRenderStyle : 0
                                onActivated: (index) => {
                                    if (typeof settingsManager !== "undefined" && settingsManager)
                                        settingsManager.waveformRenderStyle = index
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: UiTheme.textDim
                            font.pixelSize: 11
                            text: "Changes visible waveform tiles and overviews only. Audio analysis, beatgrids and playback caches are not rebuilt."
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Text {
                                text: "Playhead position"
                                color: UiTheme.textPrimary
                                font.pixelSize: 12
                                Layout.fillWidth: true
                            }
                            ComboBox {
                                objectName: "waveformPlayheadPositionCombo"
                                Layout.preferredWidth: 180
                                readonly property var positions: [0.5, 0.4, 0.33, 0.25, 0.2]
                                model: ["Center · 50%", "40%", "33%", "25%", "Left · 20%"]
                                currentIndex: {
                                    var value = (typeof settingsManager !== "undefined" && settingsManager)
                                                ? settingsManager.waveformPlayheadPosition : 0.5
                                    var closest = 0
                                    for (var i = 1; i < positions.length; ++i)
                                        if (Math.abs(positions[i] - value) < Math.abs(positions[closest] - value))
                                            closest = i
                                    return closest
                                }
                                onActivated: (index) => {
                                    if (typeof settingsManager !== "undefined" && settingsManager)
                                        settingsManager.waveformPlayheadPosition = positions[index]
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: UiTheme.textSecondary
                            font.pixelSize: 11
                            text: "Move the white playhead left to see more upcoming audio. Applies to all scrolling waveforms and is saved automatically."
                        }
                    }
                }
            }

            // ── Page 4: Legal ─────────────────────────────────────────────
            Item {
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 30
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: parent.width
                    spacing: 14

                    Text {
                        text: "Legal Notices"
                        color: UiTheme.textPrimary
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: UiTheme.separatorSubtle
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: UiTheme.textSecondary
                        font.pixelSize: 12
                        text: "This software is licensed under the GNU Affero General Public License v3.0 (AGPL-3.0-or-later)."
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: UiTheme.textLabel
                        font.pixelSize: 12
                        text: "You are entitled to receive the corresponding source code under the terms of the AGPL."
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: UiTheme.textLabel
                        font.pixelSize: 12
                        text: "Source repository: https://github.com/TimoRams/multiplatform-dj-software"
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: UiTheme.textLabel
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
                                color: UiTheme.buttonBg(false, parent.hovered, parent.down)
                                border.color: parent.hovered ? UiTheme.borderHover : "transparent"
                                radius: 0
                            }

                            contentItem: Text {
                                text: parent.text
                                color: UiTheme.textPrimary
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
                                color: UiTheme.buttonBg(false, parent.hovered, parent.down)
                                border.color: parent.hovered ? UiTheme.borderHover : "transparent"
                                radius: 0
                            }

                            contentItem: Text {
                                text: parent.text
                                color: UiTheme.textPrimary
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
}
