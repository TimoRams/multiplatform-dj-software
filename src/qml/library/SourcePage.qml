import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DJSoftware

Rectangle {
    id: root
    required property var appWindow
    property string activeSourceType: "local"
    property string activeDeviceId: ""
    signal libraryRequested()
    signal usbLibraryRequested()
    signal sourceBackRequested()

    readonly property color selectionSurface: UiTheme.browserSelectionSurface
    readonly property color selectionAccent: UiTheme.browserSelectionAccent
    property string cursorDeviceId: ""
    property bool navigationReady: false
    color: "#11171f"
    focus: visible
    property string sourcePane: "sources"
    property int sourceCursorIndex: 0
    property int serviceCursorIndex: 0
    readonly property var devices: typeof deviceLibraryManager !== "undefined"
                                 && deviceLibraryManager ? deviceLibraryManager.devices : []
    readonly property int sourceCount: 1 + devices.length
    readonly property int serviceCount: 1

    function selectedSourceLabel() {
        if (sourceCursorIndex === 0)
            return "LOCAL"
        var device = devices[sourceCursorIndex - 1]
        return device ? (device.name || "USB") : "USB"
    }

    function selectLocalLibrary() {
        libraryRequested()
    }

    function selectUsbLibrary(deviceId) {
        if (typeof deviceLibraryManager === "undefined" || !deviceLibraryManager)
            return
        deviceLibraryManager.chooseDevice(deviceId)
        usbLibraryRequested()
    }

    function ensureSourceCursor() {
        sourceCursorIndex = Math.max(0, Math.min(sourceCount - 1, sourceCursorIndex))
    }

    function syncCursorToActiveSource() {
        sourceCursorIndex = 0
        if (activeSourceType === "usb") {
            for (var i = 0; i < devices.length; ++i) {
                if (String(devices[i].id) === String(activeDeviceId)) {
                    sourceCursorIndex = i + 1
                    break
                }
            }
        }
        ensureSourceCursor()
    }

    function moveCursor(rawDelta) {
        if (rawDelta === 0)
            return
        var steps = Math.max(1, Math.min(24, Math.round(Math.abs(rawDelta))))
        var delta = rawDelta > 0 ? steps : -steps
        if (sourcePane === "sources") {
            sourceCursorIndex = Math.max(0, Math.min(sourceCount - 1,
                                                      sourceCursorIndex + delta))
            if (sourceCursorIndex > 0)
                deviceList.positionViewAtIndex(sourceCursorIndex - 1, ListView.Contain)
        } else {
            serviceCursorIndex = Math.max(0, Math.min(serviceCount - 1,
                                                       serviceCursorIndex + delta))
        }
    }

    function activateSourceCursor() {
        if (sourceCursorIndex === 0) {
            sourcePane = "services"
            serviceCursorIndex = 0
            return
        }

        var device = devices[sourceCursorIndex - 1]
        if (!device || typeof deviceLibraryManager === "undefined" || !deviceLibraryManager)
            return
        if (device.ready) {
            sourcePane = "services"
            serviceCursorIndex = 0
        }
        else if (device.canMount && !device.operationPending)
            deviceLibraryManager.mountDevice(device.id)
    }

    function activateServiceCursor() {
        if (serviceCursorIndex !== 0)
            return
        if (sourceCursorIndex === 0) {
            selectLocalLibrary()
            return
        }

        var device = devices[sourceCursorIndex - 1]
        if (device && device.ready)
            selectUsbLibrary(device.id)
    }

    function activateCursor() {
        if (sourcePane === "sources")
            activateSourceCursor()
        else
            activateServiceCursor()
    }

    function selectSourceItem(index) {
        if (sourcePane !== "sources" || sourceCursorIndex !== index) {
            sourcePane = "sources"
            sourceCursorIndex = index
            return
        }
        activateSourceCursor()
    }

    function selectServiceItem(index) {
        if (sourcePane !== "services" || serviceCursorIndex !== index) {
            sourcePane = "services"
            serviceCursorIndex = index
            return
        }
        activateServiceCursor()
    }

    function goBack() {
        if (sourcePane === "services") {
            sourcePane = "sources"
            return
        }
        sourceBackRequested()
    }

    function rememberSourceCursor() {
        if (!navigationReady) return
        const device = sourceCursorIndex > 0 ? devices[sourceCursorIndex - 1] : null
        cursorDeviceId = device ? String(device.id) : ""
    }

    function restoreSourceCursor() {
        if (!navigationReady) return
        var index = 0
        if (cursorDeviceId) {
            for (var i = 0; i < devices.length; ++i) {
                if (String(devices[i].id) === cursorDeviceId) { index = i + 1; break }
            }
            if (index === 0) {
                cursorDeviceId = ""
                sourcePane = "sources"
            }
        }
        sourceCursorIndex = index
        ensureSourceCursor()
    }

    onSourceCursorIndexChanged: rememberSourceCursor()
    onDevicesChanged: restoreSourceCursor()
    onVisibleChanged: if (visible && navigationReady) forceActiveFocus()
    Component.onCompleted: {
        syncCursorToActiveSource()
        navigationReady = true
        rememberSourceCursor()
        if (visible) forceActiveFocus()
    }

    Connections {
        target: typeof parameterStore !== "undefined" && parameterStore ? parameterStore : null
        function onParameterChanged(id, value) {
            if (!root.visible)
                return
            if (id === "library_browse") {
                if (value !== 0)
                    root.moveCursor(value)
                return
            }
            if (value <= 0)
                return
            if (id === "library_expand"
                    || id.indexOf("library_load_deck_") === 0)
                root.activateCursor()
            else if (id === "library_back" || id === "library_collapse")
                root.goBack()
        }
    }

    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Up) {
            moveCursor(-1)
            event.accepted = true
        } else if (event.key === Qt.Key_Down) {
            moveCursor(1)
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            activateCursor()
            event.accepted = true
        } else if (event.key === Qt.Key_Right) {
            sourcePane = "services"
            event.accepted = true
        } else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Left) {
            goBack()
            event.accepted = true
        }
    }

    Item {
        anchors.fill: parent
        readonly property int dividerWidth: UiTheme.dividerWidth

        Rectangle {
            id: sourcePanel
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: (parent.width - parent.dividerWidth) / 2
            color: "#151c25"

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    color: "#171f29"
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 22
                        anchors.verticalCenter: parent.verticalCenter
                        text: "MEDIA"
                        color: UiTheme.textLabel
                        font.pixelSize: 11
                        font.bold: true
                        font.letterSpacing: 1.4
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 62
                    readonly property bool focused: root.sourcePane === "sources"
                                                    && root.sourceCursorIndex === 0
                    readonly property bool active: root.activeSourceType === "local"
                    color: focused ? root.selectionSurface : "#151c25"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 80
                        color: "#151c25"
                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            color: root.selectionAccent
                            visible: root.sourcePane === "sources" && root.sourceCursorIndex === 0
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "▣\nLOCAL"
                            horizontalAlignment: Text.AlignHCenter
                            color: UiTheme.textPrimary
                            font.pixelSize: 10
                            font.bold: true
                            lineHeight: 0.9
                        }
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 102
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Internal Collection"
                        color: UiTheme.textPrimary
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        id: localMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.selectSourceItem(0)
                        }
                    }
                }

                ListView {
                    id: deviceList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.devices
                    spacing: 1

                    delegate: Item {
                        required property var modelData
                        required property int index
                        readonly property bool focused: root.sourcePane === "sources"
                                                    && root.sourceCursorIndex === index + 1
                        readonly property bool active: root.activeSourceType === "usb"
                                                       && String(root.activeDeviceId) === String(modelData.id)
                        readonly property color deviceAccent: modelData.color
                                                             ? modelData.color : UiTheme.blue
                        width: ListView.view.width
                        height: 63

                        Row {
                            anchors.fill: parent
                            spacing: 0

                            Rectangle {
                                width: 80
                                height: parent.height
                                color: "#151c25"
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 3
                                    color: root.selectionAccent
                                    visible: focused
                                }
                                Column {
                                    anchors.centerIn: parent
                                    spacing: 3
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: "▣"
                                        color: UiTheme.textPrimary
                                        font.pixelSize: 18
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: "USB " + (index + 1)
                                        color: UiTheme.textPrimary
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                }
                            }

                            Rectangle {
                                width: parent.width - 80
                                height: parent.height
                                color: focused ? root.selectionSurface : "#151c25"
                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 20
                                    anchors.top: parent.top
                                    anchors.topMargin: 10
                                    anchors.right: deviceAction.left
                                    anchors.rightMargin: 12
                                    text: modelData.name || ("USB " + (index + 1))
                                    color: UiTheme.textPrimary
                                    font.pixelSize: 17
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 20
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 9
                                    anchors.right: deviceAction.left
                                    anchors.rightMargin: 12
                                    text: modelData.operationPending ? "Mounting device…"
                                          : modelData.scanning ? "Scanning Rekordbox library…"
                                          : modelData.ready
                                            ? modelData.badge + "  ·  " + modelData.trackCount + " tracks"
                                            : (modelData.status || "Not mounted")
                                    color: focused ? UiTheme.textPrimary : UiTheme.textSecondary
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    id: deviceAction
                                    anchors.right: parent.right
                                    anchors.rightMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 64
                                    height: 27
                                    visible: modelData.canMount || modelData.canEject
                                             || modelData.operationPending
                                    color: deviceActionMouse.containsMouse
                                           && !modelData.operationPending ? UiTheme.panelRaised : UiTheme.panelInset
                                    border.width: UiTheme.dividerWidth
                                    border.color: deviceActionMouse.containsMouse
                                                  ? UiTheme.borderHover : UiTheme.border
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.operationPending ? "…" : modelData.actionLabel
                                        color: modelData.operationPending ? UiTheme.textDim : UiTheme.textPrimary
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                    MouseArea {
                                        id: deviceActionMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        enabled: !modelData.operationPending
                                                 && (modelData.canMount || modelData.canEject)
                                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: {
                                            root.sourcePane = "sources"
                                            root.sourceCursorIndex = index + 1
                                            if (modelData.canMount)
                                                deviceLibraryManager.mountDevice(modelData.id)
                                            else if (modelData.canEject)
                                                deviceLibraryManager.ejectDevice(modelData.id)
                                        }
                                    }
                                }
                                MouseArea {
                                    id: deviceMouse
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    anchors.right: deviceAction.left
                                    hoverEnabled: true
                                    cursorShape: modelData.ready ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: {
                                        root.selectSourceItem(index + 1)
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: deviceList.count === 0
                        text: "No USB devices connected"
                        color: UiTheme.textDim
                        font.pixelSize: 13
                    }
                }
            }
        }

        Rectangle {
            id: middleDivider
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: parent.dividerWidth
            color: UiTheme.borderSubtle
        }

        Rectangle {
            id: servicePanel
            anchors.left: middleDivider.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            color: "#151c25"

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    color: "#171f29"
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 30
                        anchors.verticalCenter: parent.verticalCenter
                        text: "SOURCE"
                        color: UiTheme.textSecondary
                        font.pixelSize: 14
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 108
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 24
                        elide: Text.ElideRight
                        text: root.selectedSourceLabel()
                        color: UiTheme.textPrimary
                        font.pixelSize: 17
                        font.weight: Font.Medium
                    }

                }

                Repeater {
                    model: [
                        { key: "library", icon: "▣",
                          label: root.sourceCursorIndex === 0 ? "Local Library" : "USB Library",
                          enabled: true }
                    ]

                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        readonly property bool focused: root.sourcePane === "services"
                                                     && root.serviceCursorIndex === index
                        Layout.fillWidth: true
                        Layout.preferredHeight: 94
                        color: focused ? root.selectionSurface : "#151c25"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 1
                            color: UiTheme.borderSubtle
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 31
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.focused ? "›" : ""
                            color: root.selectionAccent
                            font.pixelSize: 20
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 67
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.icon
                                color: modelData.enabled ? UiTheme.textPrimary : UiTheme.textDim
                                font.pixelSize: 22
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.label
                                horizontalAlignment: Text.AlignHCenter
                                color: modelData.enabled ? UiTheme.textPrimary : UiTheme.textDim
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                lineHeight: 0.9
                            }
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 168
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !modelData.enabled
                            text: "SERVICE NOT CONFIGURED"
                            color: parent.focused ? UiTheme.textPrimary : UiTheme.textDim
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: modelData.enabled
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: root.selectServiceItem(index)
                        }
                    }
                }

                Item { Layout.fillWidth: true; Layout.fillHeight: true }
            }
        }
    }
}
