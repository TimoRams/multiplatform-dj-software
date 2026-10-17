import QtQuick
import DJSoftware

Item {
    id: root
    property Item sourceItem: null
    property point sourceOrigin: sourceItem ? mapToItem(sourceItem, 0, 0) : Qt.point(0, 0)
    property bool requested: true
    property bool graphicsAvailable: GraphicsInfo.api !== GraphicsInfo.Software
                                     && GraphicsInfo.api !== GraphicsInfo.Unknown
    property bool applicationActive: Qt.application.state === Qt.ApplicationActive
    property size maximumCaptureSize: Qt.size(128, 256)
    property real downsampleFactor: 4
    property real captureSupersampling: 1
    property real blurRadius: 0
    property bool live: true
    property bool frameVisible: true
    property color tint: Qt.rgba(UiTheme.panel.r, UiTheme.panel.g, UiTheme.panel.b, 0.72)
    readonly property bool pressureAllowsBlur: typeof renderPressurePolicy === "undefined"
                                               || !renderPressurePolicy
                                               || renderPressurePolicy.tier === "normal"
    readonly property bool blurActive: requested && visible && width > 0 && height > 0
                                       && sourceItem && graphicsAvailable && pressureAllowsBlur
                                       && applicationActive
    readonly property real captureDpr: Math.max(1, Screen.devicePixelRatio)
    // Sidebar defaults: two RGBA textures at most 128x256 (256 KiB per pane).
    readonly property size captureSize: Qt.size(Math.min(maximumCaptureSize.width, Math.max(1, Math.ceil(width * captureDpr / Math.max(1, downsampleFactor)))),
                                                Math.min(maximumCaptureSize.height, Math.max(1, Math.ceil(height * captureDpr / Math.max(1, downsampleFactor)))))
    readonly property size sourceTextureSize: captureSupersampling <= 1 ? captureSize : Qt.size(
        Math.ceil(Math.min(width * captureDpr, maximumCaptureSize.width * 2,
                           captureSize.width * Math.max(1, captureSupersampling))),
        Math.ceil(Math.min(height * captureDpr, maximumCaptureSize.height * 2,
                           captureSize.height * Math.max(1, captureSupersampling))))
    readonly property rect captureRect: {
        if (!sourceItem)
            return Qt.rect(0, 0, 0, 0)
        return Qt.rect(Math.max(0, Math.min(sourceOrigin.x, sourceItem.width - width)),
                       Math.max(0, Math.min(sourceOrigin.y, sourceItem.height - height)),
                       Math.min(width, sourceItem.width), Math.min(height, sourceItem.height))
    }
    clip: true

    Loader {
        id: effects
        objectName: "backdropEffects"
        anchors.fill: parent
        active: root.blurActive
        sourceComponent: Item {
            ShaderEffectSource {
                id: capture
                objectName: "backdropCapture"
                sourceItem: root.sourceItem
                sourceRect: root.captureRect
                textureSize: root.sourceTextureSize
                live: root.live
                recursive: false
                hideSource: false
                visible: false
                smooth: true
                mipmap: root.captureSupersampling > 1
                samples: root.captureSupersampling > 1 ? 4 : 0
                // A frozen modal backdrop needs a fresh snapshot after resize.
                onSourceRectChanged: scheduleUpdate()
                onTextureSizeChanged: scheduleUpdate()
            }
            ShaderEffect {
                id: horizontal
                anchors.fill: parent
                property var source: capture
                property vector2d stepSize: Qt.vector2d(1 / root.captureSize.width, 0)
                // A wide blur needs neighboring texels, not widely spaced copies
                // of the five-tap sidebar kernel.
                property real sigma: Math.max(0.5, Math.min(4,
                    root.blurRadius * root.captureSize.width
                    / (3 * Math.max(1, root.captureRect.width))))
                fragmentShader: root.blurRadius > 0
                    ? "qrc:/shaders/modalblur.frag.qsb" : "qrc:/shaders/sidebarblur.frag.qsb"
            }
            ShaderEffectSource {
                id: intermediate
                objectName: "backdropIntermediate"
                sourceItem: horizontal
                textureSize: root.captureSize
                hideSource: true
                visible: false
                smooth: true
                recursive: false
            }
            ShaderEffect {
                anchors.fill: parent
                property var source: intermediate
                property vector2d stepSize: Qt.vector2d(0, 1 / root.captureSize.height)
                property real sigma: Math.max(0.5, Math.min(4,
                    root.blurRadius * root.captureSize.height
                    / (3 * Math.max(1, root.captureRect.height))))
                fragmentShader: root.blurRadius > 0
                    ? "qrc:/shaders/modalblur.frag.qsb" : "qrc:/shaders/sidebarblur.frag.qsb"
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.blurActive
               ? root.tint
               : Qt.rgba(UiTheme.panel.r, UiTheme.panel.g, UiTheme.panel.b, 0.98)
        border.color: UiTheme.borderSubtle
        border.width: root.frameVisible ? 1 : 0
        radius: 0
    }
}
