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
        if (visible)
            syncAudioSettings()
    }

    // ── Navigation categories ────────────────────────────────────────────────
    property int selectedCategory: 0
    property int pendingAudioSampleRate: 44100
    property int pendingAudioBufferSize: 128
    property string pendingAudioDeviceType: ""
    property string pendingAudioOutputDevice: ""
    property var audioDeviceTypeOptions: []
    property var audioOutputDeviceOptions: []

    readonly property var categories: [
        { label: "Audio Setup",     icon: "♪" },
        { label: "MIDI Controller", icon: "⎘" },
        { label: "Library",         icon: "☰" },
        { label: "Legal",           icon: "§" },
    ]

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

    function refreshAudioOutputDevices(deviceType) {
        if (!deckA || !deckA.getAvailableAudioOutputDevices)
            return

        audioOutputDeviceOptions = deckA.getAvailableAudioOutputDevices(deviceType)
        if (!audioOutputDeviceOptions || audioOutputDeviceOptions.length === 0)
            audioOutputDeviceOptions = [""]

        var selectedOutput = pendingAudioOutputDevice
        if (!selectedOutput || indexForText(audioOutputDeviceOptions, selectedOutput) < 0) {
            selectedOutput = deckA.getCurrentAudioOutputDevice ? deckA.getCurrentAudioOutputDevice() : ""
            if (!selectedOutput || indexForText(audioOutputDeviceOptions, selectedOutput) < 0)
                selectedOutput = audioOutputDeviceOptions[0]
        }

        pendingAudioOutputDevice = selectedOutput
        outputDeviceCombo.currentIndex = Math.max(0, indexForText(audioOutputDeviceOptions, selectedOutput))
    }

    function refreshAudioDeviceLists() {
        if (!deckA || !deckA.getAvailableAudioDeviceTypes)
            return

        audioDeviceTypeOptions = deckA.getAvailableAudioDeviceTypes()
        if (!audioDeviceTypeOptions || audioDeviceTypeOptions.length === 0)
            audioDeviceTypeOptions = [""]

        var selectedType = pendingAudioDeviceType
        if (!selectedType || indexForText(audioDeviceTypeOptions, selectedType) < 0) {
            selectedType = deckA.getCurrentAudioDeviceType ? deckA.getCurrentAudioDeviceType() : ""
            if (!selectedType || indexForText(audioDeviceTypeOptions, selectedType) < 0)
                selectedType = audioDeviceTypeOptions[0]
        }

        pendingAudioDeviceType = selectedType
        deviceTypeCombo.currentIndex = Math.max(0, indexForText(audioDeviceTypeOptions, selectedType))
        refreshAudioOutputDevices(selectedType)
    }

    function syncAudioSettings() {
        if (!settingsManager)
            return

        pendingAudioDeviceType = settingsManager.audioDeviceType
        pendingAudioOutputDevice = settingsManager.audioOutputDevice
        pendingAudioSampleRate = settingsManager.audioSampleRate
        pendingAudioBufferSize = settingsManager.audioBufferSize

        refreshAudioDeviceLists()

        sampleRateCombo.currentIndex = indexForValue(sampleRateOptions, pendingAudioSampleRate)
        bufferSizeCombo.currentIndex = indexForValue(bufferSizeOptions, pendingAudioBufferSize)
    }

    function applyAudioSettings() {
        if (!settingsManager)
            return

        var deviceType = audioDeviceTypeOptions.length > 0 && deviceTypeCombo.currentIndex >= 0
            ? audioDeviceTypeOptions[deviceTypeCombo.currentIndex]
            : pendingAudioDeviceType
        var outputDevice = audioOutputDeviceOptions.length > 0 && outputDeviceCombo.currentIndex >= 0
            ? audioOutputDeviceOptions[outputDeviceCombo.currentIndex]
            : pendingAudioOutputDevice
        var sampleRate = sampleRateOptions[sampleRateCombo.currentIndex].value
        var bufferSize = bufferSizeOptions[bufferSizeCombo.currentIndex].value

        pendingAudioDeviceType = deviceType
        pendingAudioOutputDevice = outputDevice
        pendingAudioSampleRate = sampleRate
        pendingAudioBufferSize = bufferSize

        settingsManager.audioDeviceType = deviceType
        settingsManager.audioOutputDevice = outputDevice
        settingsManager.audioSampleRate = sampleRate
        settingsManager.audioBufferSize = bufferSize

        var appliedA = deckA && deckA.applyAudioDeviceSettings
            ? deckA.applyAudioDeviceSettings(deviceType, outputDevice, sampleRate, bufferSize)
            : false
        var appliedB = deckB && deckB.applyAudioDeviceSettings
            ? deckB.applyAudioDeviceSettings(deviceType, outputDevice, sampleRate, bufferSize)
            : false

        audioApplyStatus.text = (appliedA && appliedB)
            ? "Applied to the selected device with a low-latency buffer."
            : "Saved, but the requested device or buffer could not be applied right now."
        audioApplyStatus.color = (appliedA && appliedB) ? "#8fe388" : "#ffb86c"
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

                        Item {
                            Component.onCompleted: settingsWindow.syncAudioSettings()
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: "#2a2a2a"
                        }

                        // Device Type row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: "Device Type"
                                color: "#aaa"
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            ComboBox {
                                id: deviceTypeCombo
                                Layout.fillWidth: true
                                height: 32
                                model: settingsWindow.audioDeviceTypeOptions

                                contentItem: Text {
                                    text: deviceTypeCombo.currentIndex >= 0 && deviceTypeCombo.displayText.length > 0
                                          ? deviceTypeCombo.displayText
                                          : "Default"
                                    color: "#ccc"
                                    font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter
                                    leftPadding: 12
                                    elide: Text.ElideRight
                                }

                                delegate: ItemDelegate {
                                    width: deviceTypeCombo.width
                                    contentItem: Text {
                                        text: modelData.length > 0 ? modelData : "Default"
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

                                onCurrentIndexChanged: {
                                    if (currentIndex >= 0 && currentIndex < settingsWindow.audioDeviceTypeOptions.length) {
                                        settingsWindow.pendingAudioDeviceType = settingsWindow.audioDeviceTypeOptions[currentIndex]
                                        settingsWindow.refreshAudioOutputDevices(settingsWindow.pendingAudioDeviceType)
                                    }
                                }
                            }
                        }

                        // Output Device row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: "Output Device"
                                color: "#aaa"
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            ComboBox {
                                id: outputDeviceCombo
                                Layout.fillWidth: true
                                height: 32
                                model: settingsWindow.audioOutputDeviceOptions

                                contentItem: Text {
                                    text: outputDeviceCombo.currentIndex >= 0 && outputDeviceCombo.displayText.length > 0
                                          ? outputDeviceCombo.displayText
                                          : "System Default"
                                    color: "#ccc"
                                    font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter
                                    leftPadding: 12
                                    elide: Text.ElideRight
                                }

                                delegate: ItemDelegate {
                                    width: outputDeviceCombo.width
                                    contentItem: Text {
                                        text: modelData.length > 0 ? modelData : "System Default"
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

                                onCurrentIndexChanged: {
                                    if (currentIndex >= 0 && currentIndex < settingsWindow.audioOutputDeviceOptions.length)
                                        settingsWindow.pendingAudioOutputDevice = settingsWindow.audioOutputDeviceOptions[currentIndex]
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
                            text: "Use the lowest stable buffer your device supports. On Windows, ASIO will appear here when available; on macOS and Linux this lists the active system audio backends and outputs."
                            color: "#7b7b7b"
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
                    anchors.margins: 30
                    spacing: 20

                    property var midiDeviceList: []
                    property var mappingList: []
                    property bool hasMidiDevices: false
                    property bool hasMappings: false
                    property int availableMappingCount: 0
                    readonly property string noMappingLabel: "Kein Mapping (manuell)"

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
                                midiDeviceList = ["Kein MIDI-Gerät gefunden"]

                            // Always allow manual mapping without a Mixxx XML file.
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
                                text: midiDeviceCombo.currentIndex >= 0 ? midiDeviceCombo.displayText : "Kein MIDI-Gerät"
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
                            text: "Mixxx Mapping"
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
                                text: mappingCombo.currentIndex >= 0 ? mappingCombo.displayText : "Kein Mapping"
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
                        text: midiManager ? "Mappings Ordner: " + midiManager.getMappingsDirectoryPath() : ""
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

                    Text {
                        text: "Mappings"
                        color: "#eee"
                        font.pixelSize: 14
                        font.bold: true
                    }

                    // Mappings List
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        // Helper component for mapping rows
                        component MappingRow: RowLayout {
                            required property string labelStr
                            required property string paramId

                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: labelStr
                                color: "#aaa"
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            Rectangle {
                                id: learnButton
                                Layout.fillWidth: true
                                height: 32
                                color: isLearning ? "#ff9900" : "#252525"
                                border.color: isLearning ? "#ffb732" : "#3a3a3a"
                                radius: 4

                                property bool isLearning: false

                                // Listen for C++ signal to reset learn state
                                Connections {
                                    target: midiManager
                                    function onMappingUpdated() {
                                        learnButton.isLearning = false
                                    }
                                }

                                Text {
                                    anchors.centerIn: parent
                                    text: parent.isLearning ? "Waiting for MIDI..." : "Learn"
                                    color: parent.isLearning ? "#1a1a1a" : "#ccc"
                                    font.pixelSize: 12
                                    font.bold: parent.isLearning
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (midiManager && !learnButton.isLearning) {
                                            learnButton.isLearning = true
                                            midiManager.startMidiLearn(paramId)
                                        }
                                    }
                                }
                            }
                        }

                        MappingRow { labelStr: "Deck A Play";   paramId: "deckA_play" }
                        MappingRow { labelStr: "Deck B Play";   paramId: "deckB_play" }
                        MappingRow { labelStr: "Deck A Volume"; paramId: "deckA_vol" }
                        MappingRow { labelStr: "Deck B Volume"; paramId: "deckB_vol" }
                        MappingRow { labelStr: "Crossfader";    paramId: "crossfader" }
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
