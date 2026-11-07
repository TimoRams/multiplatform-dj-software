import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DJSoftware

Rectangle {
    id: root
    objectName: "samplerPanel"
    property var controller: typeof sampler !== "undefined" ? sampler : null
    property bool pageMode: false
    property bool sequencerMode: false
    signal closeRequested()
    color: UiTheme.panel
    radius: 0
    readonly property var entries: controller ? controller.slots : []
    readonly property var transport: controller && controller.playback ? controller.playback : []
    readonly property var bankActivity: controller && controller.bankActivity ? controller.bankActivity : []
    property int sequencePage: 0
    property int velocityTrack: 0
    property int velocityStep: 0
    readonly property int sequenceLength: controller ? controller.sequencerLength : 16
    readonly property var sequenceSteps: controller ? controller.sequencerSteps : []
    readonly property var sequenceTracks: controller ? controller.sequencerTracks : []
    readonly property bool sequenceRunning: !!controller && controller.sequencerRunning
    readonly property int sequenceBank: controller ? controller.bank : 0
    onSequenceBankChanged: {
        sequencePage = 0
        velocityTrack = 0
        velocityStep = 0
    }
    onSequenceLengthChanged: {
        sequencePage = Math.min(sequencePage, sequenceLength / 16 - 1)
        velocityStep = Math.min(velocityStep, sequenceLength - 1)
    }
    function openEditor(pad, path) { sampleEditor.openForPad(pad, path || "") }
    function seconds(value) { return isFinite(value) ? Math.max(0, value).toFixed(1) : "0.0" }
    onVisibleChanged: if (!visible) sampleEditor.close()

    Flickable {
        id: samplerFlick
        anchors.fill: parent
        anchors.margins: root.pageMode ? UiTheme.px(20) : UiTheme.px(10)
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: content
            width: parent.width
            spacing: UiTheme.px(12)
            RowLayout {
                id: samplerHeader
                Layout.fillWidth: true
                spacing: UiTheme.px(6)
                Text { text: "SAMPLER"; color: UiTheme.textPrimary; font.bold: true; font.pixelSize: root.pageMode ? 20 : 14 }
                Button {
                    touchTarget: true
                    objectName: "samplerSequencerView"
                    text: root.sequencerMode ? "PADS" : "SEQUENCER"
                    onClicked: root.sequencerMode = !root.sequencerMode
                }
                Repeater {
                    model: 4
                    Button { touchTarget: true;
                        required property int index
                        objectName: "samplerBank" + index
                        readonly property var activity: root.bankActivity[index] || ({playing:0, queued:0})
                        readonly property bool sequenceActive: root.sequenceRunning && root.controller.sequencerActiveBank === index
                        readonly property bool sequenceQueued: !!root.controller && root.controller.sequencerPendingBank === index
                        text: "ABCD"[index] + (sequenceQueued ? " NEXT" : sequenceActive ? " RUN"
                            : activity.playing || activity.queued ? " · " + activity.playing + "/" + activity.queued : "")
                        opacity: sequenceActive && root.controller.sequencerStep % 4 >= 2 ? 0.45 : 1
                        Layout.preferredWidth: UiTheme.px(64)
                        Layout.minimumHeight: UiTheme.touchTarget
                        checked: !!root.controller && root.controller.bank === index
                        onClicked: if (root.controller) root.controller.bank = index
                        ToolTip.visible: hovered
                        ToolTip.text: (sequenceActive ? "Running pattern. " : sequenceQueued ? "Switches after the current pattern. " : "")
                            + activity.playing + " playing · " + activity.queued + " waiting"
                    }
                }
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 24
                    text: root.controller ? root.controller.clockSource + " · " + root.controller.bpm.toFixed(1) + " BPM" : "INTERNAL"
                    elide: Text.ElideRight
                    color: UiTheme.textSecondary
                    font.pixelSize: 12
                }
                Button { touchTarget: true; objectName: "samplerStopAll"; text: "STOP ALL"; accentColor: UiTheme.error; onClicked: if (root.controller) root.controller.stopAll() }
                Button { touchTarget: true; text: root.pageMode ? "BACK TO LIVE" : "CLOSE"; onClicked: root.closeRequested() }
            }
            RowLayout {
                id: samplerControls
                Layout.fillWidth: true
                visible: !root.sequencerMode
                spacing: UiTheme.px(10)
                Text { text: "LEVEL"; color: UiTheme.textSecondary; font.pixelSize: 12 }
                Slider {
                    objectName: "samplerGain"
                    Layout.fillWidth: true; Layout.maximumWidth: 280; Layout.minimumWidth: 72
                    from: 0; to: 1.5
                    value: root.controller ? root.controller.gain : 0.72
                    onMoved: if (root.controller) root.controller.gain = value
                }
                Text { text: Math.round((root.controller ? root.controller.gain : 0.72) * 100) + "%"; color: UiTheme.textPrimary; Layout.preferredWidth: 44; font.pixelSize: 12 }
                Text {
                    objectName: "samplerPickup"
                    visible: !!root.controller && !!root.controller.midiPickupPending
                    text: "KNOB PICKUP"; color: UiTheme.textSecondary; font.pixelSize: 10
                    ToolTip.visible: pickupHover.hovered
                    ToolTip.text: "Move the sampler knob to the current level to take control"
                    HoverHandler { id: pickupHover }
                }
                Button { touchTarget: true; objectName: "samplerCue"; text: "CUE"; checked: root.controller ? root.controller.cue : false; onClicked: if (root.controller) root.controller.cue = !root.controller.cue }
                Text { text: "QUANTIZE"; color: UiTheme.textSecondary; font.pixelSize: 12 }
                ComboBox {
                    objectName: "samplerQuantize"
                    model: ["OFF", "1 BEAT", "4 BEATS"]
                    currentIndex: root.controller ? (root.controller.quantize === 0 ? 0 : root.controller.quantize === 4 ? 2 : 1) : 1
                    onActivated: if (root.controller) root.controller.quantize = [0, 1, 4][currentIndex]
                }
                Item { Layout.fillWidth: true }
            }
            Rectangle { id: samplerDivider; Layout.fillWidth: true; implicitHeight: 1; color: UiTheme.separatorSubtle }
            ColumnLayout {
                id: sequencerControls
                Layout.fillWidth: true
                visible: root.sequencerMode
                spacing: UiTheme.px(12)
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "samplerSequencerTransport"
                        touchTarget: true
                        checked: root.controller ? root.controller.sequencerEnabled : false
                        text: checked ? "STOP" : "PLAY"
                        accentColor: checked ? UiTheme.error : UiTheme.orange
                        onClicked: if (root.controller) root.controller.sequencerEnabled = !root.controller.sequencerEnabled
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.controller
                            ? root.controller.clockSource + " · " + root.controller.bpm.toFixed(1) + " BPM"
                                + (root.sequenceRunning ? " · RUN " + "ABCD"[root.controller.sequencerActiveBank] : "")
                                + (root.controller.sequencerPendingBank >= 0 ? " · NEXT " + "ABCD"[root.controller.sequencerPendingBank] : "")
                            : "INTERNAL · 120 BPM · 16 STEPS"
                        color: UiTheme.textSecondary
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Text { text: "SWING"; color: UiTheme.textSecondary; font.pixelSize: 12 }
                    Slider {
                        id: swingSlider
                        objectName: "samplerSequencerSwing"
                        Layout.preferredWidth: UiTheme.px(140)
                        Layout.minimumWidth: UiTheme.px(60)
                        from: 0; to: 0.5; stepSize: 0.01
                        onValueChanged: if ((dragActive || !enabled) && root.controller)
                            root.controller.sequencerSwing = value
                        Binding {
                            target: swingSlider; property: "value"
                            value: root.controller ? root.controller.sequencerSwing : 0
                            when: !swingSlider.dragActive
                            restoreMode: Binding.RestoreNone
                        }
                    }
                    Text {
                        text: Math.round((root.controller ? root.controller.sequencerSwing : 0) * 100) + "%"
                        color: UiTheme.textPrimary
                        font.family: UiTheme.numericFontFamily
                        Layout.preferredWidth: UiTheme.px(36)
                    }
                    Button {
                        objectName: "samplerSequencerCopy"
                        touchTarget: true
                        text: "COPY"
                        onClicked: {
                            patternCopy.sourceBank = root.controller ? root.controller.bank : 0
                            patternCopy.open()
                        }
                    }
                    Button {
                        objectName: "samplerSequencerClear"
                        touchTarget: true
                        text: "CLEAR"
                        onClicked: if (root.controller) root.controller.clearSequencerPattern()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: UiTheme.px(6)
                    Text { text: "LENGTH"; color: UiTheme.textSecondary; font.pixelSize: 11 }
                    ComboBox {
                        objectName: "samplerSequencerLength"
                        model: ["16", "32", "48", "64"]
                        currentIndex: root.sequenceLength / 16 - 1
                        onActivated: if (root.controller) root.controller.sequencerLength = (currentIndex + 1) * 16
                    }
                    Repeater {
                        model: root.sequenceLength / 16
                        Button {
                            required property int index
                            objectName: "samplerSequencerPage" + index
                            touchTarget: true
                            text: (index * 16 + 1) + "-" + (index * 16 + 16)
                            checked: root.sequencePage === index
                            onClicked: root.sequencePage = index
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text { text: "TEMPO"; color: UiTheme.textSecondary; font.pixelSize: 11 }
                    SpinBox {
                        objectName: "samplerSequencerTempo"
                        from: 20; to: 400; editable: true
                        value: root.controller ? Math.round(root.controller.internalBpm) : 120
                        enabled: !root.controller || root.controller.clockSource === "INTERNAL"
                        onValueModified: if (root.controller) root.controller.internalBpm = value
                        ToolTip.visible: hovered
                        ToolTip.text: "Internal tempo. Deck/Link tempo takes priority while synchronized."
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "VELOCITY · PAD " + (root.velocityTrack + 1) + " / STEP " + (root.velocityStep + 1)
                        color: UiTheme.textSecondary; font.pixelSize: 11
                    }
                    Slider {
                        id: velocitySlider
                        objectName: "samplerSequencerVelocity"
                        Layout.fillWidth: true
                        Layout.maximumWidth: UiTheme.px(280)
                        Layout.minimumHeight: UiTheme.touchTarget
                        from: 1; to: 127; stepSize: 1
                        defaultValue: 127
                        onValueChanged: if ((dragActive || !enabled) && root.controller)
                            root.controller.setSequencerVelocity(root.velocityTrack, root.velocityStep, Math.round(value))
                        Binding {
                            target: velocitySlider; property: "value"
                            value: (root.sequenceSteps[root.velocityTrack] || [])[root.velocityStep] || 127
                            when: !velocitySlider.dragActive
                            restoreMode: Binding.RestoreNone
                        }
                    }
                    Text { text: Math.round(velocitySlider.value); color: UiTheme.textPrimary; font.pixelSize: 11 }
                    Item { Layout.fillWidth: true }
                }
                Repeater {
                    model: 8
                    RowLayout {
                        id: sequenceTrack
                        required property int index
                        Layout.fillWidth: true
                        spacing: UiTheme.px(3)
                        readonly property var steps: root.sequenceSteps[index] || []
                        readonly property var trackState: root.sequenceTracks[index] || ({muted:false, solo:false})
                        Text {
                            Layout.preferredWidth: UiTheme.px(90)
                            Layout.minimumWidth: UiTheme.px(48)
                            text: {
                                const entry = root.entries[sequenceTrack.index] || ({})
                                return entry.loaded ? entry.name : "PAD " + (sequenceTrack.index + 1)
                            }
                            color: UiTheme.textPrimary
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Button {
                            objectName: "samplerSequencerMute" + sequenceTrack.index
                            touchTarget: true
                            text: "M"
                            checked: sequenceTrack.trackState.muted
                            Layout.preferredWidth: UiTheme.touchTarget
                            onClicked: if (root.controller) root.controller.setSequencerMuted(sequenceTrack.index, !checked)
                            ToolTip.visible: hovered; ToolTip.text: "Mute this sequencer track (live pads remain available)"
                        }
                        Button {
                            objectName: "samplerSequencerSolo" + sequenceTrack.index
                            touchTarget: true
                            text: "S"
                            checked: sequenceTrack.trackState.solo
                            Layout.preferredWidth: UiTheme.touchTarget
                            onClicked: if (root.controller) root.controller.setSequencerSolo(sequenceTrack.index, !checked)
                            ToolTip.visible: hovered; ToolTip.text: "Solo this sequencer track"
                        }
                        Repeater {
                            model: 16
                            Button {
                                required property int index
                                readonly property int stepNumber: root.sequencePage * 16 + index
                                readonly property int velocity: sequenceTrack.steps[stepNumber] || 0
                                readonly property bool playhead: root.sequenceRunning
                                    && root.controller.sequencerActiveBank === root.controller.bank
                                    && root.controller.sequencerStep === stepNumber
                                objectName: "samplerSequencerStep" + sequenceTrack.index + "_" + stepNumber
                                Layout.fillWidth: true
                                Layout.minimumWidth: UiTheme.px(22)
                                Layout.preferredWidth: UiTheme.px(34)
                                Layout.minimumHeight: UiTheme.touchTarget
                                checked: velocity > 0
                                text: velocity > 0 && velocity < 127 ? velocity.toString()
                                    : index % 4 === 0 ? (stepNumber + 1).toString() : ""
                                font.pixelSize: 10
                                background: Rectangle {
                                    color: parent.checked ? UiTheme.orange
                                        : (parent.playhead
                                            ? UiTheme.panelRaised : UiTheme.panelDeep)
                                    border.width: parent.playhead ? 3 : parent.checked ? 2 : 1
                                    border.color: parent.playhead ? UiTheme.textPrimary
                                        : parent.checked ? UiTheme.orange : UiTheme.borderSubtle
                                    radius: 0
                                }
                                onClicked: {
                                    root.velocityTrack = sequenceTrack.index
                                    root.velocityStep = stepNumber
                                    if (root.controller) root.controller.setSequencerStep(sequenceTrack.index, stepNumber, velocity === 0)
                                }
                                onPressAndHold: {
                                    root.velocityTrack = sequenceTrack.index
                                    root.velocityStep = stepNumber
                                }
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: "Each bank has an 8-track pattern with 1-4 bars. Select or hold a step to edit velocity. "
                        + "Pattern and length changes wait for the current cycle to finish; selecting the running bank cancels NEXT. "
                        + "COPY replaces steps, length and mute/solo only, not samples."
                    color: UiTheme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
            }
            GridLayout {
                id: samplerPads
                Layout.fillWidth: true
                visible: !root.sequencerMode
                columns: 4
                columnSpacing: UiTheme.px(8); rowSpacing: UiTheme.px(8)
                Repeater {
                    model: 8
                    Rectangle {
                        id: pad
                        required property int index
                        property var entry: root.entries[index] || ({})
                        property var playback: root.transport[index] || ({})
                        property string transportState: entry.transportState || entry.state || "EMPTY"
                        property bool active: transportState === "PLAYING" || transportState === "QUEUED"
                        objectName: "samplerPad" + index
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredHeight: root.pageMode ? Math.max(UiTheme.px(168), Math.min(UiTheme.px(280), (samplerFlick.height - samplerHeader.implicitHeight - samplerControls.implicitHeight - samplerDivider.implicitHeight - content.spacing*3 - samplerPads.rowSpacing - (samplerError.visible ? samplerError.implicitHeight + content.spacing : 0)) / 2)) : UiTheme.px(168)
                        radius: 0
                        color: UiTheme.padBackground(entry.color || UiTheme.orange, !!entry.loaded, active, triggerMouse.containsMouse)
                        Rectangle { width: 2; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; color: pad.active || pad.entry.loaded ? (pad.entry.color || UiTheme.orange) : UiTheme.borderSubtle }
                        MouseArea {
                            id: triggerMouse
                            anchors.fill: parent
                            anchors.bottomMargin: UiTheme.touchTarget + UiTheme.px(10)
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onPressed: mouse => {
                                if (mouse.button === Qt.RightButton || !pad.entry.loaded) root.openEditor(pad.index, "")
                                else if (root.controller) { root.controller.selectedSlot = pad.index; root.controller.trigger(pad.index) }
                            }
                        }
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: UiTheme.px(10)
                            spacing: UiTheme.px(4)
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: (pad.index + 1).toString(); font.bold: true; font.pixelSize: 12; color: UiTheme.textPrimary }
                                Item { Layout.fillWidth: true }
                                Text { text: pad.entry.state || "EMPTY"; color: pad.entry.state === "ERROR" ? UiTheme.error : pad.active ? (pad.entry.color || UiTheme.orange) : UiTheme.textSecondary; font.pixelSize: 10 }
                            }
                            Text { Layout.fillWidth: true; text: pad.entry.name || "Load sample"; elide: Text.ElideRight; color: UiTheme.textPrimary; font.bold: true; font.pixelSize: root.pageMode ? 16 : 12 }
                            Text {
                                Layout.fillWidth: true
                                text: pad.entry.loaded || pad.active ? (pad.entry.loop ? "LOOP" : "ONE-SHOT") + (pad.entry.sync ? " · SYNC " + Number(pad.entry.bpm || 0).toFixed(1) : " · FREE") : ""
                                elide: Text.ElideRight; color: UiTheme.textSecondary; font.pixelSize: 11
                            }
                            Item {
                                Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 20
                                clip: true
                                Canvas {
                                    anchors.fill: parent
                                    property var peaks: pad.entry.waveform || []
                                    property real trimStart: pad.entry.start || 0
                                    property real trimEnd: pad.entry.end || pad.entry.duration || 0
                                    property real duration: pad.entry.duration || 0
                                    property color stroke: pad.active ? (pad.entry.color || UiTheme.orange) : UiTheme.textMuted
                                    onPeaksChanged: requestPaint()
                                    onTrimStartChanged: requestPaint()
                                    onTrimEndChanged: requestPaint()
                                    onStrokeChanged: requestPaint()
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()
                                    onPaint: {
                                        const ctx = getContext("2d"); if (!ctx) return
                                        ctx.reset(); ctx.strokeStyle = stroke; ctx.lineWidth = 1; ctx.beginPath()
                                        const first = duration > 0 ? Math.max(0, Math.floor(trimStart / duration * peaks.length)) : 0
                                        const last = duration > 0 ? Math.min(peaks.length, Math.ceil(trimEnd / duration * peaks.length)) : peaks.length
                                        for(let i=first;i<last;++i) { const x=(i-first)*width/Math.max(1,last-first), a=Math.min(1,peaks[i])*height*0.45; ctx.moveTo(x,height/2-a); ctx.lineTo(x,height/2+a) }
                                        ctx.stroke()
                                    }
                                }
                                Rectangle { visible: pad.transportState === "PLAYING"; x: Math.min(parent.width-1, parent.width*(pad.playback.progress || 0)); width: 1; height: parent.height; color: UiTheme.textPrimary }
                            }
                            Text {
                                objectName: "samplerProgressText" + pad.index
                                Layout.fillWidth: true
                                text: pad.transportState === "QUEUED" ? "START IN " + Number(pad.playback.remainingBeats || 0).toFixed(1) + " BEATS"
                                    : pad.entry.loaded || pad.active ? root.seconds(pad.active ? pad.playback.position || 0 : 0) + " / " + root.seconds(pad.playback.duration || Math.max(0, (pad.entry.end || pad.entry.duration || 0) - (pad.entry.start || 0))) + " s" : ""
                                elide: Text.ElideRight; color: UiTheme.textSecondary; font.pixelSize: 11; font.family: UiTheme.numericFontFamily
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: UiTheme.px(4)
                                Button { touchTarget: true; objectName: "samplerStop" + pad.index; text: "STOP"; enabled: pad.active; Layout.fillWidth: true; Layout.minimumWidth: UiTheme.touchTarget; onClicked: if (root.controller) root.controller.stop(pad.index) }
                                Button { touchTarget: true; objectName: "samplerEdit" + pad.index; text: "EDIT"; Layout.fillWidth: true; Layout.minimumWidth: UiTheme.touchTarget; onClicked: root.openEditor(pad.index, "") }
                            }
                        }
                        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 3; color: UiTheme.borderSubtle }
                        Rectangle { objectName: "samplerProgress" + pad.index; anchors.left: parent.left; anchors.bottom: parent.bottom; height: 3; width: parent.width * Math.max(0, Math.min(1, pad.playback.progress || 0)); visible: pad.active; color: pad.entry.color || UiTheme.orange }
                    }
                }
            }
            Text { id: samplerError; Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.controller ? root.controller.error : ""; visible: text.length > 0; color: UiTheme.error; font.pixelSize: 12 }
        }
    }
    Dialog {
        id: patternCopy
        objectName: "samplerSequencerCopyDialog"
        property int sourceBank: 0
        readonly property var destinations: [0, 1, 2, 3].filter(function(bank) { return bank !== sourceBank })
        anchors.centerIn: parent
        width: Math.min(UiTheme.px(380), root.width - UiTheme.px(24))
        modal: true
        title: "COPY PATTERN " + "ABCD"[sourceBank]
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: ColumnLayout {
            Text {
                Layout.fillWidth: true
                text: "Replace the destination pattern's steps, length and mute/solo settings? Samples are not copied."
                color: UiTheme.textPrimary
                wrapMode: Text.Wrap
            }
            ComboBox {
                id: copyDestination
                objectName: "samplerSequencerCopyDestination"
                Layout.fillWidth: true
                model: patternCopy.destinations.map(function(bank) { return "ABCD"[bank] })
            }
        }
        onAccepted: if (root.controller) root.controller.copySequencerPattern(destinations[copyDestination.currentIndex])
    }
    SamplerEditor { id: sampleEditor; controller: root.controller }
}
