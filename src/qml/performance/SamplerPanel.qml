import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import DJSoftware

Rectangle {
    id: root
    objectName: "samplerPanel"
    property var controller: typeof sampler !== "undefined" ? sampler : null
    signal closeRequested()
    color: UiTheme.panel
    border.color: UiTheme.borderSubtle
    radius: 0
    readonly property var entries: controller ? controller.slots : []
    property int editorPad: 0
    property int editorBank: 0
    property string editorPath: ""
    property real editorDuration: 0
    property var editorWaveform: []
    property string editorError: ""

    function openEditor(pad, path) {
        if (!controller) return
        editorPad = pad; editorBank = controller.bank
        controller.selectedSlot = pad
        editorPath = path || ""
        editorError = ""
        const entry = entries[pad] || {}
        sampleName.text = entry.name || ""
        sampleColor.text = entry.color || "#e0a344"
        sampleBpm.text = entry.bpm > 0 ? entry.bpm.toFixed(2) : ""
        sampleGain.value = entry.gain === undefined ? 1 : entry.gain
        mode.currentIndex = entry.loop ? 1 : 0
        syncCheck.checked = !!entry.sync
        editorDuration = entry.duration || 0
        rangeStart.text = Number(entry.start || 0).toFixed(3)
        rangeEnd.text = Number(entry.end || entry.duration || 0).toFixed(3)
        editorWaveform = entry.waveform || []
        editor.open()
        if (editorPath) controller.probeFile(editorPath)
    }
    Connections {
        target: root.controller
        function onFileInfoChanged() {
            if (!editor.visible || !root.editorPath) return
            const info = root.controller.fileInfo
            if (info.loading) return
            root.editorError = info.error || ""
            if (!info.error) {
                root.editorDuration = info.duration
                sampleName.text = info.name
                rangeStart.text = "0.000"
                rangeEnd.text = Math.min(30, info.duration).toFixed(3)
                root.editorWaveform = info.waveform || []
            }
        }
    }
    Flickable {
        anchors.fill: parent; anchors.margins: 10
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: content
            width: parent.width
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text { text: "SAMPLER"; color: UiTheme.textPrimary; font.bold: true; font.pixelSize: 12 }
                Repeater {
                    model: 4
                    Button {
                        required property int index
                        objectName: "samplerBank" + index
                        text: "ABCD"[index]
                        Layout.preferredWidth: UiTheme.touchTarget
                        Layout.preferredHeight: UiTheme.touchTarget
                        checked: !!root.controller && root.controller.bank === index
                        onClicked: if (root.controller) root.controller.bank = index
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: root.controller ? root.controller.clockSource + " · " + root.controller.bpm.toFixed(1) + " BPM" : ""
                    elide: Text.ElideRight; color: UiTheme.textSecondary; font.pixelSize: 11
                }
                Button { objectName: "samplerStopAll"; text: "STOP ALL"; accentColor: UiTheme.error; onClicked: if (root.controller) root.controller.stopAll() }
                Button { text: "CLOSE"; onClicked: root.closeRequested() }
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 4; columnSpacing: 6; rowSpacing: 6
                Repeater {
                    model: 8
                    Rectangle {
                        id: pad
                        required property int index
                        property var entry: root.entries[index] || ({})
                        property bool active: entry.state === "PLAYING" || entry.state === "QUEUED"
                        objectName: "samplerPad" + index
                        Layout.fillWidth: true
                        Layout.preferredHeight: UiTheme.px(56)
                        radius: 0
                        color: UiTheme.padBackground(entry.color || UiTheme.orange, !!entry.loaded, active, padMouse.containsMouse)
                        border.color: active ? (entry.color || UiTheme.orange) : UiTheme.borderSubtle
                        Rectangle { width: 2; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; color: pad.entry.loaded ? (pad.entry.color || UiTheme.orange) : UiTheme.borderSubtle }
                        Text { x: 8; y: 5; text: (pad.index + 1) + " · " + (pad.entry.state || "EMPTY"); color: UiTheme.textSecondary; font.pixelSize: 9 }
                        Text {
                            anchors.left: parent.left; anchors.right: editButton.left; anchors.bottom: parent.bottom
                            anchors.leftMargin: 8; anchors.bottomMargin: 8
                            text: pad.entry.name || "Load sample"
                            elide: Text.ElideRight; color: UiTheme.textPrimary; font.pixelSize: 11
                        }
                        MouseArea {
                            id: padMouse
                            anchors.fill: parent; hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onPressed: mouse => {
                                if (mouse.button === Qt.RightButton || !pad.entry.loaded) root.openEditor(pad.index, "")
                                else if (root.controller) { root.controller.selectedSlot = pad.index; root.controller.trigger(pad.index) }
                            }
                        }
                        Button {
                            id: editButton
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(UiTheme.touchTarget, UiTheme.px(52)); height: UiTheme.touchTarget
                            leftPadding: 4; rightPadding: 4
                            text: "EDIT"
                            onClicked: root.openEditor(pad.index, "")
                        }
                        DropArea {
                            anchors.fill: parent
                            keys: ["text/uri-list"]
                            onDropped: drop => {
                                if (drop.hasUrls && drop.urls.length) root.openEditor(pad.index, drop.urls[0].toString())
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "LEVEL"; color: UiTheme.textSecondary; font.pixelSize: 10 }
                Slider {
                    objectName: "samplerGain"
                    Layout.fillWidth: true; Layout.maximumWidth: 280
                    from: 0; to: 1.5
                    value: root.controller ? root.controller.gain : 0.72
                    onMoved: if (root.controller) root.controller.gain = value
                }
                Button { text: "CUE"; checked: root.controller ? root.controller.cue : false; onClicked: if (root.controller) root.controller.cue = !root.controller.cue }
                Text { text: "QUANTIZE"; color: UiTheme.textSecondary; font.pixelSize: 10 }
                ComboBox {
                    model: ["OFF", "1 BEAT", "4 BEATS"]
                    currentIndex: root.controller ? (root.controller.quantize === 0 ? 0 : root.controller.quantize === 4 ? 2 : 1) : 1
                    onActivated: if (root.controller) root.controller.quantize = [0, 1, 4][currentIndex]
                }
                Item { Layout.fillWidth: true }
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: root.controller ? root.controller.error : ""
                visible: text.length > 0; color: UiTheme.error; font.pixelSize: 11
            }
        }
    }

    FileDialog {
        id: picker
        title: "Load sampler audio"
        nameFilters: ["Audio (*.wav *.flac *.ogg *.mp3 *.aif *.aiff)"]
        onAccepted: { root.editorPath = selectedFile.toString(); root.controller.probeFile(root.editorPath) }
    }
    Dialog {
        id: editor
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, 660)
        height: Math.min(parent.height - 24, 600)
        modal: true
        title: "SAMPLE " + "ABCD"[root.editorBank] + (root.editorPad + 1)
        background: Rectangle { color: UiTheme.panel; border.color: UiTheme.borderSubtle; radius: 0 }
        contentItem: Flickable {
            clip: true; contentWidth: width; contentHeight: editorContent.implicitHeight
            ScrollBar.vertical: ScrollBar {}
            ColumnLayout {
                id: editorContent
                width: parent.width; spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Button { text: "LOAD FILE"; onClicked: picker.open() }
                    Text { Layout.fillWidth: true; text: root.editorPath || "Local sample asset"; elide: Text.ElideMiddle; color: UiTheme.textSecondary }
                }
                TextField { id: sampleName; Layout.fillWidth: true; placeholderText: "Sample name" }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 80
                    color: UiTheme.displayBackground; radius: 0
                    Canvas {
                        id: waveform
                        anchors.fill: parent
                        property var peaks: root.editorWaveform
                        onPeaksChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d"); ctx.reset()
                            ctx.strokeStyle = UiTheme.orange; ctx.lineWidth = 1
                            ctx.beginPath()
                            for (let i = 0; i < peaks.length; ++i) {
                                const x = i * width / peaks.length
                                const amplitude = Math.min(1, peaks[i]) * height * 0.45
                                ctx.moveTo(x, height/2 - amplitude); ctx.lineTo(x, height/2 + amplitude)
                            }
                            ctx.stroke()
                        }
                    }
                    Repeater {
                        model: 2
                        Rectangle {
                            required property int index
                            width: 2; height: parent.height
                            color: index === 0 ? UiTheme.play : UiTheme.error
                            x: root.editorDuration > 0 ? Math.max(0, Math.min(parent.width - width,
                                Number(index === 0 ? rangeStart.text : rangeEnd.text) / root.editorDuration * parent.width)) : 0
                            MouseArea {
                                width: UiTheme.touchTarget; height: parent.height
                                anchors.horizontalCenter: parent.horizontalCenter
                                onPositionChanged: mouse => {
                                    if (!pressed) return
                                    const point = mapToItem(waveform, mouse.x, mouse.y)
                                    const seconds = Math.max(0, Math.min(root.editorDuration, point.x / waveform.width * root.editorDuration))
                                    if (parent.index === 0) rangeStart.text = seconds.toFixed(3)
                                    else rangeEnd.text = seconds.toFixed(3)
                                }
                            }
                        }
                    }
                }
                RowLayout {
                    Text { text: "START / END (s)"; color: UiTheme.textSecondary }
                    TextField { id: rangeStart; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
                    TextField { id: rangeEnd; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
                }
                RowLayout {
                    Text { text: "MODE"; color: UiTheme.textSecondary }
                    ComboBox { id: mode; model: ["ONE-SHOT", "LOOP"] }
                    CheckBox { id: syncCheck; text: "SYNC"; enabled: mode.currentIndex === 1 }
                    TextField { id: sampleBpm; Layout.fillWidth: true; placeholderText: "Source BPM"; inputMethodHints: Qt.ImhFormattedNumbersOnly }
                }
                RowLayout {
                    Text { text: "GAIN"; color: UiTheme.textSecondary }
                    Slider { id: sampleGain; from: 0; to: 1.5; Layout.fillWidth: true }
                    TextField { id: sampleColor; Layout.preferredWidth: 110; placeholderText: "#e0a344" }
                }
                Text { Layout.fillWidth: true; text: "CAPTURE FROM DECK"; color: UiTheme.textSecondary }
                RowLayout {
                    ComboBox { id: captureDeck; model: ["DECK A", "DECK B", "DECK C", "DECK D"] }
                    ComboBox { id: captureBeats; model: [1, 2, 4, 8, 16, 32]; currentIndex: 2 }
                    Button {
                        text: "CAPTURE LOOP / BEATS"
                        onClicked: {
                            root.controller.bank = root.editorBank
                            root.controller.captureDeck(root.editorPad, captureDeck.currentIndex, Number(captureBeats.currentText))
                            editor.close()
                        }
                    }
                }
                Text { Layout.fillWidth: true; text: root.editorError; color: UiTheme.error; wrapMode: Text.Wrap; visible: text.length > 0 }
                Text { Layout.fillWidth: true; text: "Maximum 30 seconds per sample. An active deck loop takes precedence over the beat count."; color: UiTheme.textLabel; wrapMode: Text.Wrap }
                RowLayout {
                    Button { text: "CLEAR"; onClicked: { root.controller.bank = root.editorBank; root.controller.clear(root.editorPad); editor.close() } }
                    Item { Layout.fillWidth: true }
                    Button { text: "CANCEL"; onClicked: editor.close() }
                    Button {
                        text: "SAVE"
                        enabled: !!root.controller && (!root.editorPath || !root.controller.fileInfo.loading)
                        onClicked: {
                            const start = Number(rangeStart.text), end = Number(rangeEnd.text), bpm = Number(sampleBpm.text)
                            if (!isFinite(start) || !isFinite(end) || start < 0 || end <= start || end - start > 30.0001 || end > root.editorDuration + 0.001) {
                                root.editorError = "Choose a valid range of at most 30 seconds"; return
                            }
                            if (mode.currentIndex === 1 && syncCheck.checked && (!isFinite(bpm) || bpm < 20 || bpm > 400)) {
                                root.editorError = "Enter source BPM between 20 and 400 for Sync"; return
                            }
                            root.controller.bank = root.editorBank
                            if (root.editorPath) root.controller.loadFile(root.editorPad, root.editorPath, start, end, bpm, mode.currentIndex === 1, syncCheck.checked,
                                sampleName.text, sampleColor.text, sampleGain.value)
                            else root.controller.edit(root.editorPad, sampleName.text, sampleColor.text, sampleGain.value,
                                mode.currentIndex === 1, syncCheck.checked, bpm, start, end)
                            editor.close()
                        }
                    }
                }
            }
        }
    }
}
