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
    color: UiTheme.displayBackground
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

    component MediaIcon: Canvas {
        property string kind: "local"
        property color ink: UiTheme.textSecondary
        onKindChanged: requestPaint()
        onInkChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.scale(width / 24, height / 24)
            ctx.strokeStyle = ink; ctx.fillStyle = ink
            ctx.lineWidth = 1.5; ctx.lineCap = "round"; ctx.lineJoin = "round"
            if (kind === "local") {
                ctx.strokeRect(3, 4, 18, 12)
                ctx.beginPath(); ctx.moveTo(12, 16); ctx.lineTo(12, 20)
                ctx.moveTo(7, 20); ctx.lineTo(17, 20); ctx.stroke()
            } else if (kind === "library") {
                ctx.strokeRect(3, 5, 4, 15); ctx.strokeRect(9, 3, 4, 17)
                ctx.beginPath(); ctx.moveTo(16, 5); ctx.lineTo(21, 19)
                ctx.lineTo(17, 20); ctx.lineTo(13, 6); ctx.closePath(); ctx.stroke()
            } else {
                ctx.beginPath(); ctx.moveTo(12, 19); ctx.lineTo(12, 3)
                ctx.moveTo(9, 6); ctx.lineTo(12, 3); ctx.lineTo(15, 6)
                ctx.moveTo(12, 14); ctx.lineTo(6, 10); ctx.lineTo(6, 7)
                ctx.moveTo(12, 12); ctx.lineTo(18, 8); ctx.lineTo(18, 5); ctx.stroke()
                ctx.fillRect(16.5, 3, 3, 3)
                ctx.beginPath(); ctx.arc(6, 6, 1.5, 0, Math.PI * 2); ctx.fill()
                ctx.beginPath(); ctx.arc(12, 20, 2, 0, Math.PI * 2); ctx.stroke()
            }
        }
    }

    Item {
        id: browser
        width: Math.min(parent.width - 40, 1040)
        anchors.top: parent.top; anchors.bottom: parent.bottom
        anchors.topMargin: 20; anchors.bottomMargin: 20
        anchors.horizontalCenter: parent.horizontalCenter

        Item {
            id: heading
            width: parent.width; height: 56
            Column {
                spacing: 5
                Text {
                    text: "Sources"; color: UiTheme.textPrimary
                    font.pixelSize: 24; font.weight: Font.DemiBold
                }
                Text {
                    text: "Choose where to browse your music."
                    color: UiTheme.textSecondary; font.pixelSize: 12
                }
            }
            Rectangle {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 32; height: 32; radius: 4
                color: closeMouse.containsMouse ? UiTheme.inspectorInset : "transparent"
                border.color: UiTheme.borderSubtle
                Text { anchors.centerIn: parent; text: "×"; color: UiTheme.textSecondary; font.pixelSize: 20 }
                MouseArea {
                    id: closeMouse; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.sourceBackRequested()
                }
            }
        }

        RowLayout {
            anchors.top: heading.bottom; anchors.topMargin: 16
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            spacing: 16

            Rectangle {
                id: sourcePanel
                Layout.preferredWidth: Math.min(400, browser.width * 0.43)
                Layout.preferredHeight: Math.min(parent.height, Math.max(196, 48 + root.sourceCount * 68 + (root.devices.length === 0 ? 64 : 0)))
                Layout.alignment: Qt.AlignTop
                color: UiTheme.inspectorSurface; radius: 4
                border.color: UiTheme.borderSubtle

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 8
                    spacing: 4
                    Item {
                        Layout.fillWidth: true; Layout.preferredHeight: 32
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "MEDIA"; color: UiTheme.textLabel
                            font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 1
                        }
                        Text {
                            anchors.right: parent.right; anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.devices.length + " USB"
                            color: UiTheme.textLabel; font.pixelSize: 10
                        }
                    }
                    Rectangle {
                        objectName: "localSourceRow"
                        Layout.fillWidth: true; Layout.preferredHeight: 60; Layout.minimumHeight: 60
                        readonly property bool focused: root.sourcePane === "sources" && root.sourceCursorIndex === 0
                        color: focused ? root.selectionSurface : UiTheme.inspectorInset
                        border.color: focused ? root.selectionAccent : "transparent"
                        radius: 3
                        MediaIcon {
                            anchors.left: parent.left; anchors.leftMargin: 13
                            anchors.verticalCenter: parent.verticalCenter
                            width: 24; height: 24
                            ink: parent.focused ? root.selectionAccent : UiTheme.textSecondary
                        }
                        Column {
                            anchors.left: parent.left; anchors.leftMargin: 52
                            anchors.right: parent.right; anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4
                            Text {
                                width: parent.width; text: "Local collection"
                                color: UiTheme.textPrimary; font.pixelSize: 14; font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text { text: "Internal library"; color: UiTheme.textSecondary; font.pixelSize: 11 }
                        }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectSourceItem(0)
                        }
                    }
                    ListView {
                        id: deviceList
                        Layout.fillWidth: true; Layout.fillHeight: true
                        clip: true; model: root.devices; spacing: 4
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            readonly property bool focused: root.sourcePane === "sources" && root.sourceCursorIndex === index + 1
                            width: ListView.view.width; height: 64; radius: 3
                            color: focused ? root.selectionSurface : UiTheme.inspectorInset
                            border.color: focused ? root.selectionAccent : "transparent"
                            MediaIcon {
                                kind: "usb"; width: 24; height: 24
                                anchors.left: parent.left; anchors.leftMargin: 13
                                anchors.verticalCenter: parent.verticalCenter
                                ink: parent.focused ? root.selectionAccent : UiTheme.textSecondary
                            }
                            Column {
                                anchors.left: parent.left; anchors.leftMargin: 52
                                anchors.right: deviceAction.left; anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter; spacing: 4
                                Text {
                                    width: parent.width
                                    text: modelData.name || ("USB " + (index + 1))
                                    color: UiTheme.textPrimary; font.pixelSize: 14; font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width
                                    text: modelData.operationPending ? "Mounting…"
                                          : modelData.scanning ? "Scanning library…"
                                          : modelData.ready ? ((modelData.trackCount || 0) + " tracks · USB " + (index + 1))
                                          : (modelData.status || "Not mounted")
                                    color: UiTheme.textSecondary; font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                            }
                            Rectangle {
                                id: deviceAction
                                anchors.right: parent.right; anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: visible ? 56 : 0; height: 28; radius: 3
                                visible: modelData.canMount || modelData.canEject || modelData.operationPending
                                color: actionMouse.containsMouse ? UiTheme.panelRaised : UiTheme.inspectorSurface
                                border.color: UiTheme.borderSubtle
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.operationPending ? "…" : (modelData.actionLabel || (modelData.canMount ? "Mount" : "Eject"))
                                    color: UiTheme.textSecondary; font.pixelSize: 10
                                }
                                MouseArea {
                                    id: actionMouse; anchors.fill: parent; hoverEnabled: true
                                    enabled: !modelData.operationPending && (modelData.canMount || modelData.canEject)
                                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: {
                                        root.sourcePane = "sources"
                                        root.sourceCursorIndex = index + 1
                                        if (modelData.canMount) deviceLibraryManager.mountDevice(modelData.id)
                                        else if (modelData.canEject) deviceLibraryManager.ejectDevice(modelData.id)
                                    }
                                }
                            }
                            MouseArea {
                                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                anchors.right: deviceAction.left
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectSourceItem(index + 1)
                            }
                        }
                        Text {
                            anchors.centerIn: parent; visible: deviceList.count === 0
                            text: "No USB devices connected"
                            color: UiTheme.textLabel; font.pixelSize: 11
                        }
                    }
                }
            }

            Rectangle {
                id: servicePanel
                Layout.fillWidth: true; Layout.preferredHeight: 236
                Layout.alignment: Qt.AlignTop
                readonly property var device: root.sourceCursorIndex > 0 ? root.devices[root.sourceCursorIndex - 1] : null
                readonly property bool ready: root.sourceCursorIndex === 0 || (device && device.ready)
                color: UiTheme.inspectorSurface; radius: 4
                border.color: UiTheme.borderSubtle
                Column {
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.top: parent.top; anchors.margins: 20
                    spacing: 10
                    Text {
                        text: "BROWSE"; color: UiTheme.textLabel
                        font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 1
                    }
                    Text {
                        width: parent.width
                        text: root.sourceCursorIndex === 0 ? "Local collection" : root.selectedSourceLabel()
                        color: UiTheme.textPrimary; font.pixelSize: 22; font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: servicePanel.ready ? "Tracks and playlists, ready to browse."
                                                 : "Mount this device to browse its library."
                        color: UiTheme.textSecondary; font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }
                Rectangle {
                    objectName: "sourceLibraryRow"
                    readonly property bool focused: root.sourcePane === "services" && root.serviceCursorIndex === 0
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.bottom: parent.bottom; anchors.margins: 12
                    height: 66; radius: 3
                    color: focused ? root.selectionSurface : UiTheme.inspectorInset
                    border.color: focused ? root.selectionAccent : UiTheme.borderSubtle
                    opacity: servicePanel.ready ? 1 : 0.5
                    MediaIcon {
                        kind: "library"; width: 26; height: 26
                        anchors.left: parent.left; anchors.leftMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        ink: parent.focused ? root.selectionAccent : UiTheme.textSecondary
                    }
                    Column {
                        anchors.left: parent.left; anchors.leftMargin: 54
                        anchors.right: parent.right; anchors.rightMargin: 26
                        anchors.verticalCenter: parent.verticalCenter; spacing: 4
                        Text {
                            text: "Open library"; color: UiTheme.textPrimary
                            font.pixelSize: 14; font.weight: Font.DemiBold
                        }
                        Text {
                            width: parent.width; text: "Browse tracks and playlists"
                            color: UiTheme.textSecondary; font.pixelSize: 11; elide: Text.ElideRight
                        }
                    }
                    Text {
                        anchors.right: parent.right; anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: "›"; color: UiTheme.textSecondary; font.pixelSize: 22
                    }
                    MouseArea {
                        anchors.fill: parent; enabled: servicePanel.ready
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.selectServiceItem(0)
                    }
                }
            }
        }
    }
}
