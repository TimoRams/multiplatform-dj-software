import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import DJSoftware

Window {
    id: root
    required property var appWindow

    width: 1280
    height: 430
    minimumWidth: 1280
    maximumWidth: 1280
    minimumHeight: 430
    maximumHeight: 430
    visible: appWindow && appWindow.showDevelopmentControls
    title: "BrockDJ — Development Controls"
    color: UiTheme.bgDeep

    // FxBar historically derives its height from the containing window.
    readonly property int fxBarHeight: UiTheme.px(90)

    onVisibleChanged: {
        if (!visible && appWindow)
            appWindow.showDevelopmentControls = false
    }

    Item {
        anchors.fill: parent
        clip: true

        Item {
            id: committedContent
            anchors.fill: parent

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 26
                    Layout.minimumHeight: 26
                    Layout.maximumHeight: 26
                    color: UiTheme.bg0

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: "DEVELOPMENT CONTROLS"
                        color: UiTheme.textPrimary
                        font.pixelSize: 9
                        font.bold: true
                        font.letterSpacing: 1.0
                    }

                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Desktop bridge — toggle in View"
                        color: UiTheme.textLabel
                        font.pixelSize: 8
                    }
                }

                Flickable {
                    id: controlFlickable
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: width
                    contentHeight: controlsColumn.implicitHeight

                    ColumnLayout {
                        id: controlsColumn
                        width: controlFlickable.width
                        spacing: 1

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: mixerAB.minimumUsableHeight
                            Layout.minimumHeight: mixerAB.minimumUsableHeight
                            Layout.maximumHeight: mixerAB.minimumUsableHeight
                            spacing: 1

                            DeckControl {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                deckName: "A"
                                engine: deckA
                                hostWindow: root.appWindow
                                developmentControls: true
                                controlsOnly: true
                            }

                            MixerSection {
                                id: mixerAB
                                Layout.fillHeight: true
                                Layout.preferredWidth: UiTheme.mixerPreferredWidth
                                Layout.minimumWidth: UiTheme.mixerPreferredWidth
                                Layout.maximumWidth: UiTheme.mixerPreferredWidth
                                engineA: deckA
                                engineB: deckB
                                mc: mixerControl
                                fx: fxManager
                            }

                            DeckControl {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                deckName: "B"
                                engine: deckB
                                hostWindow: root.appWindow
                                developmentControls: true
                                controlsOnly: true
                            }
                        }

                        RowLayout {
                            visible: root.appWindow && root.appWindow.fourDeckMode
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible ? mixerCD.minimumUsableHeight : 0
                            Layout.minimumHeight: visible ? mixerCD.minimumUsableHeight : 0
                            Layout.maximumHeight: visible ? mixerCD.minimumUsableHeight : 0
                            spacing: 1

                            DeckControl {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                deckName: "C"
                                engine: deckC
                                hostWindow: root.appWindow
                                developmentControls: true
                                controlsOnly: true
                            }

                            MixerSection {
                                id: mixerCD
                                Layout.fillHeight: true
                                Layout.preferredWidth: UiTheme.mixerPreferredWidth
                                Layout.minimumWidth: UiTheme.mixerPreferredWidth
                                Layout.maximumWidth: UiTheme.mixerPreferredWidth
                                engineA: deckC
                                engineB: deckD
                                channelAId: "deckC"
                                channelBId: "deckD"
                                deckNameA: "C"
                                deckNameB: "D"
                                mc: mixerControl
                                fx: fxManager
                            }

                            DeckControl {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                deckName: "D"
                                engine: deckD
                                hostWindow: root.appWindow
                                developmentControls: true
                                controlsOnly: true
                            }
                        }

                        CrossfaderBar {
                            Layout.fillWidth: true
                            Layout.preferredHeight: UiTheme.px(36)
                            Layout.minimumHeight: UiTheme.px(36)
                            Layout.maximumHeight: UiTheme.px(36)
                            hostWindow: root.appWindow
                            mc: mixerControl
                            engineA: deckA
                            engineB: deckB
                            engineC: deckC
                            engineD: deckD
                            fourDeckMode: root.appWindow ? root.appWindow.fourDeckMode : false
                        }

                        FxBar {
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.fxBarHeight
                            Layout.minimumHeight: root.fxBarHeight
                            Layout.maximumHeight: root.fxBarHeight
                        }
                    }
                }
            }
        }
    }

    component CrossfaderBar: Rectangle {
        id: cfBar
        color: UiTheme.panel
        required property var hostWindow

        property var  engineA: null
        property var  engineB: null
        property var  engineC: null
        property var  engineD: null
        property bool fourDeckMode: false

        property string assignA: "A"
        property string assignB: "B"
        property string assignC: "A"
        property string assignD: "B"

        property real cfPos: 0.0

        // 0 = smooth/wide fade, 1 = sharp/cut (exponential mode only)
        property real cfSharpness: 0.0

        // "exponential" (Serato-style power curve) or "linear"
        property string cfCurveMode: "exponential"

        property bool _restoringSettings: false

        readonly property color clrA: "#ff9900"
        readonly property color clrB: "#00ccff"
        readonly property color clrC: "#cc44ff"
        readonly property color clrD: "#44ddaa"

        readonly property bool cfCurveIsLinear: cfCurveMode === "linear"

        // Exponential mode: e = 1 → equal linear slopes; e → 0 → sharp cut
        function curveExponent() {
            return Math.pow(10.0, -cfSharpness * 2.0)
        }

        function channelGain(t, isA) {
            if (cfCurveIsLinear)
                return isA ? Math.max(0.0, 1.0 - t) : Math.max(0.0, t)

            var e = curveExponent()
            if (isA)
                return Math.pow(Math.max(0.0, 1.0 - t), e)
            return Math.pow(Math.max(0.0, t), e)
        }

        property var mc: null

        function applyVolumes() {
            if (!mc)
                return
            mc.syncCrossfaderState(cfPos, assignA, assignB, assignC, assignD,
                                   cfSharpness, cfCurveMode)
            mc.applyAllVolumes()
        }

        function scheduleSettingsSave() {
            if (_restoringSettings)
                return
            settingsSaveTimer.restart()
        }

        function persistSettings() {
            if (typeof settingsManager === "undefined" || !settingsManager)
                return
            settingsManager.crossfaderPosition = cfPos
            settingsManager.crossfaderSharpness = cfSharpness
            settingsManager.crossfaderCurveMode = cfCurveMode
            settingsManager.crossfaderAssignA = assignA
            settingsManager.crossfaderAssignB = assignB
            settingsManager.crossfaderAssignC = assignC
            settingsManager.crossfaderAssignD = assignD
        }

        function restoreSettings() {
            if (typeof settingsManager === "undefined" || !settingsManager)
                return

            _restoringSettings = true
            cfPos = settingsManager.crossfaderPosition
            cfSharpness = settingsManager.crossfaderSharpness
            cfCurveMode = settingsManager.crossfaderCurveMode
            assignA = settingsManager.crossfaderAssignA
            assignB = settingsManager.crossfaderAssignB
            assignC = settingsManager.crossfaderAssignC
            assignD = settingsManager.crossfaderAssignD
            cfSlider.value = cfPos
            curveDial.value = cfSharpness
            _restoringSettings = false
            applyVolumes()
        }

        property real volA: 1.0
        property real volB: 1.0
        property real volC: 1.0
        property real volD: 1.0

        onCfPosChanged: {
            if (_restoringSettings)
                return
            applyVolumes()
            scheduleSettingsSave()
        }
        onCfSharpnessChanged: {
            applyVolumes()
            scheduleSettingsSave()
        }
        onCfCurveModeChanged: {
            applyVolumes()
            scheduleSettingsSave()
        }
        onAssignAChanged: { applyVolumes(); scheduleSettingsSave() }
        onAssignBChanged: { applyVolumes(); scheduleSettingsSave() }
        onAssignCChanged: { applyVolumes(); scheduleSettingsSave() }
        onAssignDChanged: { applyVolumes(); scheduleSettingsSave() }
        onEngineAChanged: applyVolumes()
        onEngineBChanged: applyVolumes()
        onEngineCChanged: applyVolumes()
        onEngineDChanged: applyVolumes()

        Timer {
            id: settingsSaveTimer
            interval: 250
            repeat: false
            onTriggered: cfBar.persistSettings()
        }

        Component.onCompleted: {
            restoreSettings()
            applyVolumes()
        }

        Connections {
            target: parameterStore
            function onParameterChanged(id, value) {
                // Audio + MixerControl fader state: MixerControl (C++).
                if      (id === "deckA_vol") cfBar.volA = value
                else if (id === "deckB_vol") cfBar.volB = value
                else if (id === "deckC_vol") cfBar.volC = value
                else if (id === "deckD_vol") cfBar.volD = value
                else if (id === "crossfader") {
                    // Audio: parameterStore → MixerControl; mirror slider only.
                    _restoringSettings = true
                    cfSlider.value = value * 2.0 - 1.0
                    _restoringSettings = false
                }
            }
        }

        // ── Curve settings — anchored to right ────────────────────────────────
        RowLayout {
            id: rightPanel
            anchors.right: parent.right; anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Rectangle {
                width: 1; height: cfBar.height - 12; color: UiTheme.separatorSubtle
                Layout.rightMargin: 5
            }

            // Curve type: linear vs exponential
            Repeater {
                model: [
                    { label: "EXP", mode: "exponential" },
                    { label: "LIN", mode: "linear" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    Layout.preferredWidth: 28; Layout.preferredHeight: 20
                    radius: 0
                    readonly property bool isActive: cfBar.cfCurveMode === modelData.mode
                    color: isActive ? UiTheme.panelRaised : UiTheme.panelDeep

                    Rectangle {
                        anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                        height: isActive ? 2 : 0
                        visible: isActive
                        color: "#3a7ad4"
                    }
                    Text {
                        anchors.centerIn: parent; text: parent.modelData.label
                        color: parent.isActive ? "#7ab8f5" : "#444"
                        font.pixelSize: cfBar.hostWindow.spViewport(7); font.bold: true; font.family: "monospace"
                    }
                    HoverHandler { id: modeHov }
                    Rectangle { anchors.fill: parent; radius: 2; color: "#fff"
                        opacity: modeHov.hovered && !parent.isActive ? 0.04 : 0.0 }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            cfBar.cfCurveMode = modelData.mode
                            if (modelData.mode === "linear")
                                cfBar.cfSharpness = 0.0
                        }
                    }
                }
            }

            Item { Layout.preferredWidth: 4 }

            // Sharpness presets (exponential mode)
            Repeater {
                model: [
                    { label: "SFT", s: 0.0 },
                    { label: "SHP", s: 0.5 },
                    { label: "CUT", s: 1.0 }
                ]
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    Layout.preferredWidth: 28; Layout.preferredHeight: 20
                    radius: 0
                    readonly property bool isActive: !cfBar.cfCurveIsLinear
                                                     && Math.abs(cfBar.cfSharpness - modelData.s) < 0.02
                    opacity: cfBar.cfCurveIsLinear ? 0.45 : 1.0
                    color: isActive ? UiTheme.panelRaised : UiTheme.panelDeep

                    Rectangle {
                        anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                        height: isActive ? 2 : 0
                        visible: isActive
                        color: "#3a7ad4"
                    }
                    Text {
                        anchors.centerIn: parent; text: parent.modelData.label
                        color: parent.isActive ? "#7ab8f5" : "#444"
                        font.pixelSize: cfBar.hostWindow.spViewport(7); font.bold: true; font.family: "monospace"
                    }
                    HoverHandler { id: ph }
                    Rectangle { anchors.fill: parent; radius: 2; color: "#fff"
                        opacity: ph.hovered && !parent.isActive ? 0.04 : 0.0 }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        enabled: !cfBar.cfCurveIsLinear
                        onClicked: {
                            cfBar.cfCurveMode = "exponential"
                            cfBar.cfSharpness = modelData.s
                            curveDial.value = modelData.s
                        }
                    }
                }
            }

            Item { Layout.preferredWidth: 4 }

            // Manual sharpness knob (exponential mode)
            Item {
                Layout.preferredWidth: 22; Layout.preferredHeight: 22
                Layout.alignment: Qt.AlignVCenter
                opacity: cfBar.cfCurveIsLinear ? 0.45 : 1.0

                Dial {
                    id: curveDial
                    anchors.fill: parent
                    from: 0.0; to: 1.0; value: 0.0; stepSize: 0.01
                    enabled: !cfBar.cfCurveIsLinear
                    onValueChanged: {
                        if (!cfBar.cfCurveIsLinear)
                            cfBar.cfSharpness = value
                    }

                    MouseArea {
                        id: curveDragLock
                        anchors.fill: parent
                        z: 100
                        acceptedButtons: Qt.LeftButton
                        preventStealing: true
                        enabled: !cfBar.cfCurveIsLinear

                        property real _pressGX:  0
                        property real _pressGY:  0
                        property real _pressVal: 0
                        property bool _active:   false

                        onPressed: (mouse) => {
                            var g    = curveDragLock.mapToGlobal(mouse.x, mouse.y)
                            _pressGX  = g.x
                            _pressGY  = g.y
                            _pressVal = curveDial.value
                            _active   = false
                            mouse.accepted = true
                        }

                        onPositionChanged: (mouse) => {
                            var g  = curveDragLock.mapToGlobal(mouse.x, mouse.y)
                            var dy = _pressGY - g.y
                            if (!_active) {
                                if (Math.abs(dy) < 4) return
                                _active = true
                                cursorControl.hideCursor()
                            }
                            cfBar.cfCurveMode = "exponential"
                            var newVal = _pressVal + dy * (curveDial.to - curveDial.from) / 150.0
                            curveDial.value = Math.min(curveDial.to, Math.max(curveDial.from, newVal))
                        }

                        onReleased: {
                            if (_active) {
                                _active = false
                                cursorControl.restoreCursor()
                                cursorControl.moveCursor(_pressGX, _pressGY)
                            }
                        }

                        onDoubleClicked: {
                            cfBar.cfCurveMode = "exponential"
                            curveDial.value = 0.0
                        }
                    }

                    background: Rectangle {
                        x: curveDial.width/2 - width/2; y: curveDial.height/2 - height/2
                        width: curveDial.width; height: curveDial.height
                        radius: width/2; color: "transparent"
                        Canvas {
                            id: dialArc; anchors.fill: parent; antialiasing: true
                            onPaint: {
                                var ctx = getContext("2d"); ctx.reset()
                                var cx = width/2; var cy = height/2
                                var r  = Math.min(width,height) * 0.42
                                var s0 = 120 * Math.PI/180; var span = 300 * Math.PI/180
                                ctx.lineWidth = 2; ctx.lineCap = "round"
                                ctx.strokeStyle = "#222"
                                ctx.beginPath(); ctx.arc(cx,cy,r, s0, s0+span, false); ctx.stroke()
                                ctx.strokeStyle = cfBar.cfCurveIsLinear ? "#333" : "#3a7ad4"
                                ctx.beginPath(); ctx.arc(cx,cy,r, s0, s0 + curveDial.value*span, false); ctx.stroke()
                            }
                            Connections {
                                target: cfBar
                                function onCfSharpnessChanged() { dialArc.requestPaint() }
                                function onCfCurveModeChanged() { dialArc.requestPaint() }
                            }
                            Connections { target: curveDial; function onValueChanged() { dialArc.requestPaint() } }
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width*0.72; height: parent.height*0.72
                            radius: width/2; color: "#141414"
                        }
                    }
                    handle: Rectangle {
                        id: dh
                        x: curveDial.background.x + curveDial.background.width /2 - width /2
                        y: curveDial.background.y + curveDial.background.height/2 - height/2
                        width: curveDial.width*0.72; height: curveDial.height*0.72; color: "transparent"
                        Rectangle {
                            color: "#aaa"; width: 1; height: parent.height*0.38
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top; anchors.topMargin: -1
                        }
                        transform: Rotation { angle: curveDial.angle; origin.x: dh.width/2; origin.y: dh.height/2 }
                    }
                }
            }

            Item { Layout.preferredWidth: 4 }

            // Curve visualizer uses the same channel-gain formula.
            Canvas {
                id: curveViz
                Layout.preferredWidth: 58; Layout.preferredHeight: 24
                Layout.alignment: Qt.AlignVCenter
                antialiasing: true

                onPaint: {
                    var ctx = getContext("2d"); ctx.reset()
                    var w = width; var h = height
                    var pad = 2

                    ctx.fillStyle = "#161616"; ctx.fillRect(0,0,w,h)
                    ctx.strokeStyle = "#2a2a2a"; ctx.lineWidth = 1
                    ctx.strokeRect(0.5, 0.5, w-1, h-1)
                    ctx.strokeStyle = "#333333"; ctx.lineWidth = 0.5
                    ctx.beginPath(); ctx.moveTo(w/2, pad); ctx.lineTo(w/2, h-pad); ctx.stroke()

                    function drawCurve(color, isA) {
                        ctx.strokeStyle = color; ctx.lineWidth = 1.3
                        ctx.beginPath()
                        for (var i = 0; i <= w; i++) {
                            var xn = (i / w) * 2.0 - 1.0
                            var t  = (xn + 1.0) * 0.5
                            var vol = cfBar.channelGain(t, isA)
                            var py  = (h - pad) - vol * (h - pad*2)
                            if (i === 0) ctx.moveTo(i, py); else ctx.lineTo(i, py)
                        }
                        ctx.stroke()
                    }
                    drawCurve("#ff9900", true)
                    drawCurve("#00ccff", false)
                }

                Connections {
                    target: cfBar
                    function onCfSharpnessChanged() { curveViz.requestPaint() }
                    function onCfCurveModeChanged() { curveViz.requestPaint() }
                }
            }
        }

        // ── Crossfader + assigns — truly centered in the full bar ─────────────
        RowLayout {
            id: centerGroup
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            height: parent.height
            spacing: 0

            AssignGroup {
                crossfader: cfBar
                deckLabel: "A"; deckAccent: cfBar.clrA; currentAssign: cfBar.assignA
                Layout.alignment: Qt.AlignVCenter
                onPicked: (v) => { cfBar.assignA = v }
            }
            AssignGroup {
                crossfader: cfBar
                visible: cfBar.fourDeckMode; Layout.preferredWidth: visible ? implicitWidth : 0
                deckLabel: "C"; deckAccent: cfBar.clrC; currentAssign: cfBar.assignC
                Layout.leftMargin: visible ? 6 : 0; Layout.alignment: Qt.AlignVCenter
                onPicked: (v) => { cfBar.assignC = v }
            }

            Item { Layout.preferredWidth: 10 }

            Item {
                Layout.preferredWidth: 220; Layout.fillHeight: true

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 0; width: 1; height: 4; color: "#2a2a2a"
                }
                Text {
                    anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                    text: "◄"; color: cfBar.clrA; font.pixelSize: cfBar.hostWindow.spViewport(8)
                }
                Text {
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    text: "►"; color: cfBar.clrB; font.pixelSize: cfBar.hostWindow.spViewport(8)
                }

                Slider {
                    id: cfSlider
                    anchors.left: parent.left;   anchors.leftMargin:  14
                    anchors.right: parent.right; anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    height: 22
                    from: -1.0; to: 1.0; value: 0.0; stepSize: 0.005
                    onValueChanged: cfBar.cfPos = value

                    property bool cfDragActive: false

                    background: Rectangle {
                        x: cfSlider.leftPadding; y: cfSlider.height/2 - height/2
                        width: cfSlider.availableWidth; height: 4
                        radius: 0; color: UiTheme.faderTrack
                        Rectangle {
                            y: 1; height: parent.height - 2; radius: 0
                            color: cfSlider.pressed ? UiTheme.borderHover : UiTheme.faderFill
                            readonly property real mid: parent.width / 2
                            readonly property real pos: 1 + cfSlider.visualPosition * (parent.width - 2)
                            x: Math.min(mid, pos); width: Math.max(0, Math.abs(pos - mid))
                        }
                    }

                    handle: Rectangle {
                        x: cfSlider.leftPadding + cfSlider.visualPosition * (cfSlider.availableWidth - width)
                        y: cfSlider.height / 2 - height / 2
                        implicitWidth: 18; implicitHeight: 22; radius: 0
                        color: cfSlider.pressed || cfSlider.cfDragActive ? "#f0f0f0" : UiTheme.faderCap

                        Rectangle { anchors.centerIn: parent; width: 2; height: parent.height * 0.48; color: "#666666" }
                    }

                    MouseArea {
                        id: cfDragLock
                        anchors.fill: parent
                        z: 100
                        acceptedButtons: Qt.LeftButton
                        preventStealing: true

                        property real _pressGX:  0
                        property real _pressGY:  0
                        property real _pressVal: 0
                        property bool _active:   false

                        onPressed: (mouse) => {
                            var g    = cfDragLock.mapToGlobal(mouse.x, mouse.y)
                            _pressGX  = g.x
                            _pressGY  = g.y
                            _pressVal = cfSlider.value
                            _active   = false
                            mouse.accepted = true
                        }

                        onPositionChanged: (mouse) => {
                            var g     = cfDragLock.mapToGlobal(mouse.x, mouse.y)
                            var delta = g.x - _pressGX
                            if (!_active) {
                                if (Math.abs(delta) < 4) return
                                _active = true
                                cfSlider.cfDragActive = true
                                cursorControl.hideCursor()
                            }
                            var newVal = _pressVal + delta * (cfSlider.to - cfSlider.from) / 200.0
                            cfSlider.value = Math.max(cfSlider.from, Math.min(cfSlider.to, newVal))
                        }

                        onReleased: {
                            if (_active) {
                                _active = false
                                cfSlider.cfDragActive = false
                                cursorControl.restoreCursor()
                                cursorControl.moveCursor(_pressGX, _pressGY)
                            }
                        }

                        onDoubleClicked: {
                            cfSlider.enabled = false
                            cfSlider.value   = 0.0
                            cfSlider.enabled = true
                        }
                    }
                }
            }

            Item { Layout.preferredWidth: 10 }

            AssignGroup {
                crossfader: cfBar
                visible: cfBar.fourDeckMode; Layout.preferredWidth: visible ? implicitWidth : 0
                deckLabel: "D"; deckAccent: cfBar.clrD; currentAssign: cfBar.assignD
                Layout.rightMargin: visible ? 6 : 0; Layout.alignment: Qt.AlignVCenter
                onPicked: (v) => { cfBar.assignD = v }
            }
            AssignGroup {
                crossfader: cfBar
                deckLabel: "B"; deckAccent: cfBar.clrB; currentAssign: cfBar.assignB
                Layout.alignment: Qt.AlignVCenter
                onPicked: (v) => { cfBar.assignB = v }
            }
        }

    }

    component AssignGroup: RowLayout {
        id: ag
        required property var crossfader
        required property string deckLabel
        required property color deckAccent
        property string currentAssign: "A"
        signal picked(string v)

        spacing: 1

        Text {
            text: ag.deckLabel
            color: ag.deckAccent
            font.pixelSize: ag.crossfader.hostWindow.spViewport(8)
            font.bold: true
            font.family: "monospace"
            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: 3
        }
        Repeater {
            model: ["A", "T", "B"]
            delegate: Rectangle {
                required property string modelData
                required property int index
                Layout.preferredWidth: 18
                Layout.preferredHeight: 20
                color: ag.currentAssign === modelData ? UiTheme.panelRaised : UiTheme.panelDeep

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: ag.currentAssign === modelData ? 2 : 0
                    visible: ag.currentAssign === modelData
                    color: ag.deckAccent
                }
                Text {
                    anchors.centerIn: parent
                    text: parent.modelData
                    color: ag.currentAssign === parent.modelData ? ag.deckAccent : "#3a3a3a"
                    font.pixelSize: ag.crossfader.hostWindow.spViewport(7)
                    font.bold: true
                    font.family: "monospace"
                }
                HoverHandler { id: hov }
                Rectangle {
                    anchors.fill: parent
                    color: "#fff"
                    opacity: hov.hovered && ag.currentAssign !== parent.modelData ? 0.04 : 0.0
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: ag.picked(parent.modelData)
                }
            }
        }
    }
}
