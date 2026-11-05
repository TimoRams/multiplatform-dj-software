import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import DJSoftware

Dialog {
    id: root
    objectName: "samplerEditor"
    property var controller: null
    property double session: 0
    property int slot: 0
    property string source: ""
    property real sourceDuration: 0
    property real sourceRate: 44100
    property var overviewPeaks: []
    property var detailPeaks: []
    property bool existing: false
    property bool settingDraft: false
    property bool nameDirty: false
    property int activeBoundary: 0
    property int zoomIndex: 0
    property real viewStart: 0
    property bool waveformLoading: false
    readonly property real viewLength: sourceDuration > 0 ? Math.min(30, sourceDuration) / [1,2,4,8][zoomIndex] : 1
    readonly property real viewEnd: Math.min(sourceDuration, viewStart + viewLength)
    readonly property real rangeStart: parseNumber(startField.text)
    readonly property real rangeEnd: parseNumber(endField.text)
    readonly property bool validRange: isFinite(rangeStart) && isFinite(rangeEnd) && rangeStart >= 0 && rangeEnd <= sourceDuration + 0.000001 && rangeEnd > rangeStart && rangeEnd - rangeStart <= 30.0001 && Math.round(rangeEnd * sourceRate) - Math.round(rangeStart * sourceRate) >= 2
    readonly property bool validSync: !loopMode.currentIndex || !syncCheck.checked || (isFinite(parseNumber(bpmField.text)) && parseNumber(bpmField.text) >= 20 && parseNumber(bpmField.text) <= 400)
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 24 : 776, UiTheme.px(1120))
    height: Math.min(parent ? parent.height - 24 : 576, UiTheme.px(800))
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: UiTheme.px(16)
    background: Rectangle { color: UiTheme.panel; radius: 0; border.color: UiTheme.borderSubtle }
    component EditorField: TextField {
        implicitHeight: UiTheme.touchTarget
        color: UiTheme.textPrimary
        placeholderTextColor: UiTheme.textMuted
        selectionColor: UiTheme.blue
        selectedTextColor: UiTheme.textPrimary
        font.family: UiTheme.uiFontFamily
        font.pixelSize: 14
        padding: 12
        selectByMouse: true
        background: Rectangle {
            radius: 0
            color: UiTheme.displayBackground
            border.color: parent.activeFocus ? UiTheme.focus : UiTheme.borderSubtle
        }
    }
    function parseNumber(text) { return Number(String(text).trim().replace(",", ".")) }
    function draft() { return {name: nameField.text, color: colorField.text, gain: gainSlider.value, loop: loopMode.currentIndex === 1, sync: syncCheck.checked, bpm: parseNumber(bpmField.text), start: rangeStart, end: rangeEnd} }
    function requestDetail() {
        if (!visible || !controller || !source || viewEnd <= viewStart || controller.editorBusy) return
        waveformLoading = true
        controller.requestEditorWaveform(session, viewStart, viewEnd)
    }
    function stopPreview() { if (!settingDraft && controller && session) controller.stopEditorPreview() }
    function focusRange() {
        const center = validRange ? (rangeStart + rangeEnd) / 2 : viewLength / 2
        viewStart = Math.max(0, Math.min(Math.max(0, sourceDuration - viewLength), center - viewLength / 2))
        detailTimer.restart()
    }
    function setBoundary(index, value) {
        if (!isFinite(value) || sourceDuration <= 0) return
        const gap = 2 / sourceRate
        let start = isFinite(rangeStart) ? rangeStart : 0
        let end = isFinite(rangeEnd) ? rangeEnd : sourceDuration
        if (index === 0) start = Math.max(0, end - 30, Math.min(end - gap, value))
        else end = Math.min(sourceDuration, start + 30, Math.max(start + gap, value))
        start = Math.round(start * sourceRate) / sourceRate; end = Math.round(end * sourceRate) / sourceRate
        if (index === 0) startField.text = start.toFixed(6)
        else endField.text = end.toFixed(6)
    }
    function openForPad(pad, path) {
        if (!controller || typeof controller.beginEditor !== "function") return
        const initial = controller.beginEditor(pad)
        if (!initial.session) return
        settingDraft = true
        session = initial.session; slot = initial.slot; source = initial.source || ""
        sourceDuration = initial.duration || 0; sourceRate = initial.sampleRate || 44100
        overviewPeaks = initial.waveform || []; detailPeaks = []; existing = !!initial.loaded
        nameField.text = initial.name || ""; colorField.text = initial.color || "#e0a344"
        gainSlider.value = initial.gain === undefined ? 1 : initial.gain
        loopMode.currentIndex = initial.loop ? 1 : 0; syncCheck.checked = !!initial.sync
        bpmField.text = initial.bpm > 0 ? initial.bpm.toFixed(2) : ""
        startField.text = Number(initial.start || 0).toFixed(6); endField.text = Number(initial.end || 0).toFixed(6)
        nameDirty = false; zoomIndex = 0; viewStart = 0; waveformLoading = false
        settingDraft = false
        open(); focusRange()
        if (path) controller.setEditorFile(session, path)
    }
    onClosed: {
        picker.close()
        detailTimer.stop()
        if (controller && session) controller.cancelEditor(session)
        session = 0; waveformLoading = false
    }
    Component.onDestruction: if (controller && session) controller.cancelEditor(session)
    Connections {
        target: root.controller
        function onEditorSourceReady(token, info) {
            if (token !== root.session) return
            root.settingDraft = true
            root.source = info.source; root.sourceDuration = info.duration; root.sourceRate = info.sampleRate || 44100
            root.overviewPeaks = info.waveform || []; root.detailPeaks = []
            if (!root.nameDirty) nameField.text = info.name || ""
            startField.text = Number(info.start || 0).toFixed(6)
            endField.text = Number(info.end === undefined ? Math.min(30, info.duration) : info.end).toFixed(6)
            if (info.loop !== undefined) { loopMode.currentIndex = info.loop ? 1 : 0; syncCheck.checked = !!info.sync; bpmField.text = Number(info.bpm || 0).toFixed(2) }
            root.zoomIndex = 0; root.settingDraft = false; root.focusRange()
        }
        function onEditorWaveformReady(token, start, end, peaks) {
            if (token !== root.session || Math.abs(start - root.viewStart) > 0.000001 || Math.abs(end - root.viewEnd) > 0.000001) return
            root.detailPeaks = peaks; root.waveformLoading = false
        }
        function onEditorChanged() { if (root.controller.editorError) root.waveformLoading = false }
        function onEditorFinished(token, success, error) { if (token === root.session && success) root.close() }
    }
    Timer { id: detailTimer; interval: 90; onTriggered: root.requestDetail() }
    FileDialog {
        id: picker
        title: "Load sampler audio"
        nameFilters: ["Audio (*.wav *.flac *.ogg *.mp3 *.aif *.aiff)"]
        onAccepted: if (root.controller && root.session) root.controller.setEditorFile(root.session, selectedFile.toString())
    }
    header: Rectangle {
        color: UiTheme.displayBackground
        implicitHeight: UiTheme.px(56)
        RowLayout {
            anchors.fill: parent; anchors.margins: UiTheme.px(16)
            Text { text: "SAMPLE " + "ABCD"[Math.floor(root.slot / 8)] + (root.slot % 8 + 1); font.bold: true; font.pixelSize: 18; color: UiTheme.textPrimary }
            Text { Layout.fillWidth: true; text: nameField.text || "New sample"; elide: Text.ElideRight; color: UiTheme.textSecondary; font.pixelSize: 14 }
            Text { text: root.controller && root.controller.editorBusy ? "WORKING…" : ""; color: UiTheme.textSecondary }
        }
    }
    contentItem: Flickable {
        clip: true; contentWidth: width; contentHeight: editorContent.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: editorContent
            width: parent.width
            spacing: UiTheme.px(12)
            RowLayout {
                Layout.fillWidth: true
                Button { touchTarget: true; objectName: "samplerEditorLoad"; text: "LOAD FILE"; enabled: !root.controller || !root.controller.editorBusy; onClicked: picker.open() }
                Text { Layout.fillWidth: true; text: root.source ? "SOURCE · " + root.sourceDuration.toFixed(2) + " s · " + (root.sourceRate / 1000).toFixed(1) + " kHz" : "Choose a file or capture from a deck"; color: UiTheme.textSecondary; font.pixelSize: 12; elide: Text.ElideRight }
            }
            Rectangle {
                id: overview
                Layout.fillWidth: true; Layout.preferredHeight: UiTheme.px(54)
                color: UiTheme.displayBackground; radius: 0
                Canvas {
                    anchors.fill: parent
                    property var peaks: root.overviewPeaks
                    onPeaksChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d"); ctx.reset(); ctx.strokeStyle = UiTheme.textSecondary; ctx.lineWidth = 1; ctx.beginPath()
                        for (let i = 0; i < peaks.length; ++i) { const x = i * width / peaks.length, a = Math.min(1, peaks[i]) * height * 0.43; ctx.moveTo(x, height/2-a); ctx.lineTo(x, height/2+a) }
                        ctx.stroke()
                    }
                }
                Rectangle { x: root.sourceDuration > 0 ? overview.width * root.viewStart / root.sourceDuration : 0; width: root.sourceDuration > 0 ? Math.min(overview.width, overview.width * root.viewLength / root.sourceDuration) : 0; height: parent.height; color: UiTheme.orange; opacity: 0.16 }
                MouseArea { anchors.fill: parent; enabled: root.sourceDuration > 0; onPressed: mouse => { root.viewStart = Math.max(0, Math.min(root.sourceDuration - root.viewLength, mouse.x / width * root.sourceDuration - root.viewLength/2)); detailTimer.restart() } }
            }
            RowLayout {
                Layout.fillWidth: true
                Button { touchTarget: true; text: "START"; checked: root.activeBoundary === 0; onClicked: root.activeBoundary = 0 }
                Button { touchTarget: true; text: "END"; checked: root.activeBoundary === 1; onClicked: root.activeBoundary = 1 }
                Button { touchTarget: true; objectName: "samplerTrimMinus"; text: "− FRAME"; enabled: root.validRange; onClicked: root.setBoundary(root.activeBoundary, (root.activeBoundary === 0 ? root.rangeStart : root.rangeEnd) - 1/root.sourceRate) }
                Button { touchTarget: true; objectName: "samplerTrimPlus"; text: "+ FRAME"; enabled: root.validRange; onClicked: root.setBoundary(root.activeBoundary, (root.activeBoundary === 0 ? root.rangeStart : root.rangeEnd) + 1/root.sourceRate) }
                Item { Layout.fillWidth: true }
                ComboBox { objectName: "samplerEditorZoom"; model: ["1×", "2×", "4×", "8×"]; currentIndex: root.zoomIndex; onActivated: { const center = root.viewStart + root.viewLength/2; root.zoomIndex = currentIndex; root.viewStart = Math.max(0, Math.min(root.sourceDuration-root.viewLength, center-root.viewLength/2)); detailTimer.restart() } }
            }
            Rectangle {
                id: detail
                objectName: "samplerEditorWaveform"
                Layout.fillWidth: true; Layout.preferredHeight: UiTheme.px(160)
                color: UiTheme.displayBackground; radius: 0
                clip: true
                Canvas {
                    anchors.fill: parent
                    property var peaks: root.detailPeaks
                    onPeaksChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d"); ctx.reset(); ctx.strokeStyle = UiTheme.orange; ctx.lineWidth = 1; ctx.beginPath()
                        for (let i = 0; i < peaks.length; ++i) { const x = i * width / peaks.length, a = Math.min(1, peaks[i]) * height * 0.43; ctx.moveTo(x, height/2-a); ctx.lineTo(x, height/2+a) }
                        ctx.stroke()
                    }
                }
                Rectangle {
                    readonly property real start: isFinite(root.rangeStart) ? Math.max(0, Math.min(parent.width, (root.rangeStart-root.viewStart)/root.viewLength*parent.width)) : 0
                    readonly property real end: isFinite(root.rangeEnd) ? Math.max(start, Math.min(parent.width, (root.rangeEnd-root.viewStart)/root.viewLength*parent.width)) : 0
                    x: start; width: end-start; height: parent.height; color: UiTheme.orange; opacity: 0.12
                }
                Repeater {
                    model: 2
                    Item {
                        required property int index
                        readonly property real time: index === 0 ? root.rangeStart : root.rangeEnd
                        readonly property real markerX: Math.max(0, Math.min(detail.width - 2, (time-root.viewStart)/root.viewLength*detail.width))
                        visible: isFinite(time) && time >= root.viewStart && time <= root.viewEnd
                        anchors.fill: parent
                        z: root.activeBoundary === index ? 3 : 2
                        Rectangle { x: parent.markerX; width: 2; height: parent.height; color: parent.index === 0 ? UiTheme.play : UiTheme.error }
                        MouseArea {
                            objectName: "samplerTrimHandle" + parent.index
                            x: Math.max(0, Math.min(detail.width - width, parent.markerX - width/2))
                            width: UiTheme.touchTarget; height: parent.height
                            preventStealing: true
                            onPressed: root.activeBoundary = parent.index
                            onPositionChanged: mouse => { if (pressed) { const point = mapToItem(detail, mouse.x, mouse.y); root.setBoundary(parent.index, root.viewStart + point.x/detail.width*root.viewLength) } }
                        }
                    }
                }
                Rectangle {
                    visible: !!root.controller && root.controller.previewPlaying && root.validRange
                    x: Math.max(0, Math.min(parent.width-1, (root.rangeStart + (root.controller ? root.controller.previewPlayback.position : 0) - root.viewStart)/root.viewLength*parent.width))
                    width: 1; height: parent.height; color: UiTheme.textPrimary
                }
                Text { anchors.centerIn: parent; text: root.waveformLoading ? "LOADING WAVEFORM…" : root.source ? "" : "LOAD OR CAPTURE A SAMPLE"; color: UiTheme.textSecondary; font.pixelSize: 12 }
            }
            Slider { objectName: "samplerEditorPan"; Layout.fillWidth: true; from: 0; to: Math.max(0, root.sourceDuration-root.viewLength); value: root.viewStart; enabled: to > 0; onMoved: { root.viewStart = value; detailTimer.restart() } }
            GridLayout {
                Layout.fillWidth: true
                columns: root.width >= UiTheme.px(1100) ? 2 : 1
                columnSpacing: UiTheme.px(24); rowSpacing: UiTheme.px(10)
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "START (s)"; Layout.preferredWidth: 80; color: UiTheme.textSecondary }
                    EditorField { id: startField; objectName: "samplerEditorStart"; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly; onTextChanged: root.stopPreview() }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "END (s)"; Layout.preferredWidth: 80; color: UiTheme.textSecondary }
                    EditorField { id: endField; objectName: "samplerEditorEnd"; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly; onTextChanged: root.stopPreview() }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "NAME"; Layout.preferredWidth: 80; color: UiTheme.textSecondary }
                    EditorField { id: nameField; objectName: "samplerEditorName"; Layout.fillWidth: true; maximumLength: 128; placeholderText: "Sample name"; onTextEdited: root.nameDirty = true }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "COLOR"; Layout.preferredWidth: 80; color: UiTheme.textSecondary }
                    ComboBox {
                        id: colorChoice
                        model: ["AMBER", "BLUE", "GREEN", "RED", "PURPLE", "WHITE"]
                        property var colors: ["#e0a344", "#40a0e0", "#80b040", "#e05a55", "#ad79df", "#e2e5e9"]
                        currentIndex: colors.indexOf(colorField.text.toLowerCase())
                        displayText: currentIndex >= 0 ? model[currentIndex] : "CUSTOM"
                        onActivated: colorField.text = colors[currentIndex]
                    }
                    EditorField { id: colorField; objectName: "samplerEditorColor"; Layout.fillWidth: true; maximumLength: 64; placeholderText: "#e0a344" }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "MODE"; Layout.preferredWidth: 80; color: UiTheme.textSecondary }
                    ComboBox { id: loopMode; objectName: "samplerEditorMode"; model: ["ONE-SHOT", "LOOP"]; onCurrentIndexChanged: root.stopPreview() }
                    CheckBox {
                        id: syncCheck
                        objectName: "samplerEditorSync"
                        text: "SYNC"
                        implicitHeight: UiTheme.touchTarget
                        enabled: loopMode.currentIndex === 1
                        indicator: Rectangle {
                            width: 20; height: 20; y: (syncCheck.height-height)/2
                            radius: 0; color: UiTheme.displayBackground
                            border.color: syncCheck.activeFocus ? UiTheme.focus : UiTheme.borderSubtle
                            Rectangle { anchors.fill: parent; anchors.margins: 4; visible: syncCheck.checked; color: UiTheme.orange }
                        }
                        contentItem: Text { text: syncCheck.text; leftPadding: 28; verticalAlignment: Text.AlignVCenter; color: syncCheck.enabled ? UiTheme.textPrimary : UiTheme.textMuted; font.pixelSize: 12 }
                    }
                    Item { Layout.fillWidth: true }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "SOURCE BPM"; Layout.preferredWidth: 80; color: UiTheme.textSecondary }
                    EditorField { id: bpmField; objectName: "samplerEditorBpm"; Layout.fillWidth: true; placeholderText: "20–400 for Sync"; inputMethodHints: Qt.ImhFormattedNumbersOnly }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "GAIN"; Layout.preferredWidth: 80; color: UiTheme.textSecondary }
                    Slider { id: gainSlider; objectName: "samplerEditorGain"; Layout.fillWidth: true; from: 0; to: 1.5; onMoved: root.stopPreview() }
                    Text { text: Math.round(gainSlider.value * 100) + "%"; Layout.preferredWidth: 44; color: UiTheme.textPrimary }
                }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: UiTheme.separatorSubtle }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "CAPTURE"; color: UiTheme.textSecondary }
                ComboBox { id: captureDeck; objectName: "samplerCaptureDeck"; model: ["DECK A", "DECK B", "DECK C", "DECK D"] }
                ComboBox { id: captureBeats; objectName: "samplerCaptureBeats"; model: [1,2,4,8,16,32]; currentIndex: 2 }
                Button { touchTarget: true; objectName: "samplerEditorCapture"; text: "USE LOOP / BEATS"; enabled: !!root.controller && !root.controller.editorBusy; onClicked: root.controller.captureEditor(root.session, captureDeck.currentIndex, Number(captureBeats.currentText)) }
                Item { Layout.fillWidth: true }
            }
            Text { Layout.fillWidth: true; text: root.validRange ? "Selection: " + (root.rangeEnd-root.rangeStart).toFixed(3) + " s · Maximum 30 seconds. Active deck loops take precedence when capturing." : "Choose a range of 2 frames to 30 seconds."; wrapMode: Text.Wrap; color: root.validRange ? UiTheme.textSecondary : UiTheme.error; font.pixelSize: 12 }
            Text { Layout.fillWidth: true; text: root.controller && !root.controller.previewAvailable ? "Preview needs an active audio device and separate headphone outputs." : "Preview plays only in headphones, at source tempo."; wrapMode: Text.Wrap; color: UiTheme.textSecondary; font.pixelSize: 12 }
        }
    }
    footer: Rectangle {
        color: UiTheme.displayBackground
        implicitHeight: UiTheme.px(68) + (footerError.visible ? footerError.implicitHeight + UiTheme.px(8) : 0)
        ColumnLayout {
            anchors.fill: parent; anchors.margins: UiTheme.px(12)
            spacing: UiTheme.px(8)
            Text { id: footerError; objectName: "samplerEditorError"; Layout.fillWidth: true; text: root.controller ? root.controller.editorError : ""; visible: text.length > 0; color: UiTheme.error; wrapMode: Text.Wrap; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                Layout.minimumHeight: UiTheme.touchTarget
                spacing: UiTheme.px(8)
                Button { touchTarget: true; objectName: "samplerEditorClear"; text: "CLEAR"; enabled: root.existing && !!root.controller && !root.controller.editorBusy; onClicked: root.controller.clearEditor(root.session) }
                Button { touchTarget: true;
                    objectName: "samplerEditorPreview"
                    text: root.controller && root.controller.previewLoading ? "CANCEL PREVIEW" : root.controller && root.controller.previewPlaying ? "STOP PREVIEW" : "PREVIEW · CUE"
                    enabled: !!root.controller && ((root.controller.previewLoading || root.controller.previewPlaying) || (root.controller.previewAvailable && root.validRange && !root.controller.editorBusy))
                    onClicked: { if (root.controller.previewLoading || root.controller.previewPlaying) root.controller.stopEditorPreview(); else root.controller.previewEditor(root.session, root.draft()) }
                }
                Item { Layout.fillWidth: true }
                Button { touchTarget: true; objectName: "samplerEditorCancel"; text: "CANCEL"; onClicked: root.close() }
                Button { touchTarget: true; objectName: "samplerEditorSave"; text: root.controller && root.controller.editorBusy ? "WORKING…" : "SAVE"; enabled: !!root.controller && !root.controller.editorBusy && root.validRange && root.validSync && !!root.source; onClicked: root.controller.applyEditor(root.session, root.draft()) }
            }
        }
    }
}
