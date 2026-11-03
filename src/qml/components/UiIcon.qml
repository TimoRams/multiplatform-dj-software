import QtQuick
import DJSoftware

Canvas {
    property string kind: "unknown"
    property color ink: UiTheme.textSecondary
    implicitWidth: 20
    implicitHeight: 20
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onKindChanged: requestPaint()
    onInkChanged: requestPaint()
    onPaint: {
        const c = getContext("2d")
        c.reset()
        c.scale(width / 20, height / 20)
        c.strokeStyle = ink
        c.lineWidth = 1.5
        c.lineCap = "round"
        c.lineJoin = "round"
        c.beginPath()
        if (kind === "bluetooth") {
            c.moveTo(9, 2); c.lineTo(15, 7); c.lineTo(5, 15)
            c.moveTo(5, 5); c.lineTo(15, 13); c.lineTo(9, 18); c.lineTo(9, 2)
        } else if (kind === "wifi") {
            c.arc(10, 17, 14, -2.35, -0.79)
            c.moveTo(3, 10); c.arc(10, 17, 10, -2.35, -0.79)
            c.moveTo(6, 13); c.arc(10, 17, 6, -2.35, -0.79)
            c.moveTo(9, 17); c.lineTo(11, 17)
        } else if (kind === "search") {
            c.arc(8, 8, 5, 0, Math.PI * 2)
            c.moveTo(12, 12); c.lineTo(17, 17)
        } else if (kind === "library") {
            c.rect(3, 3, 3, 14); c.rect(8, 3, 3, 14)
            c.moveTo(13, 4); c.lineTo(16, 3); c.lineTo(19, 16); c.lineTo(16, 17); c.closePath()
        } else if (kind === "computer") {
            c.rect(2, 3, 16, 11)
            c.moveTo(10, 14); c.lineTo(10, 18)
            c.moveTo(6, 18); c.lineTo(14, 18)
        } else if (kind === "ethernet") {
            c.rect(3, 2, 14, 10)
            c.moveTo(10, 12); c.lineTo(10, 17)
            c.moveTo(3, 17); c.lineTo(17, 17)
            c.moveTo(3, 15); c.lineTo(3, 19)
            c.moveTo(17, 15); c.lineTo(17, 19)
        } else if (kind === "usb") {
            c.moveTo(10, 18); c.lineTo(10, 2)
            c.moveTo(7, 5); c.lineTo(10, 2); c.lineTo(13, 5)
            c.moveTo(10, 13); c.lineTo(5, 10); c.lineTo(5, 7)
            c.moveTo(10, 10); c.lineTo(15, 8); c.lineTo(15, 5)
            c.rect(14, 3, 2, 2)
        } else if (kind === "audio") {
            c.moveTo(3, 8); c.lineTo(6, 8); c.lineTo(10, 4); c.lineTo(10, 16); c.lineTo(6, 12); c.lineTo(3, 12); c.closePath()
            c.moveTo(14, 6); c.bezierCurveTo(18, 8, 18, 12, 14, 14)
        } else if (kind === "controller") {
            c.rect(2, 4, 16, 12); c.moveTo(10, 4); c.lineTo(10, 16)
            c.moveTo(5, 7); c.lineTo(5, 13); c.moveTo(13, 10); c.lineTo(16, 10)
        } else if (kind === "analysis") {
            c.moveTo(2, 10); c.lineTo(5, 10); c.lineTo(7, 4); c.lineTo(11, 16); c.lineTo(14, 7); c.lineTo(16, 10); c.lineTo(18, 10)
        } else if (kind === "sync") {
            c.arc(10, 10, 7, -2.8, -0.2); c.moveTo(17, 4); c.lineTo(17, 9); c.lineTo(12, 9)
            c.moveTo(3, 11); c.arc(10, 10, 7, 0.34, 2.94); c.moveTo(3, 16); c.lineTo(3, 11); c.lineTo(8, 11)
        } else if (kind === "legal") {
            c.rect(4, 2, 12, 16); c.moveTo(7, 6); c.lineTo(13, 6); c.moveTo(7, 10); c.lineTo(13, 10); c.moveTo(7, 14); c.lineTo(11, 14)
        } else {
            c.arc(10, 10, 8, 0, Math.PI * 2)
            c.moveTo(2, 10); c.lineTo(18, 10)
            c.moveTo(10, 2); c.bezierCurveTo(4, 7, 4, 13, 10, 18)
            c.bezierCurveTo(16, 13, 16, 7, 10, 2)
        }
        c.stroke()
    }
}
