import QtQuick
import DJSoftware

Item {
    id: root
    required property var appWindow
    readonly property var window: appWindow
    property alias uncleanShutdownWarning: uncleanShutdownWarning

// ── Previous unsafe shutdown notification ───────────────────────────────
Rectangle {
    id: uncleanShutdownWarning
    anchors.top: parent.top
    anchors.topMargin: 18
    anchors.horizontalCenter: parent.horizontalCenter
    width: Math.min(parent.width * 0.92, 640)
    height: unsafeShutdownRow.implicitHeight + 22
    radius: 6
    color: "#1d1508"
    border.color: "#8a5a14"
    z: 1001
    visible: window.uncleanShutdownWarningVisible
    opacity: visible ? 1.0 : 0.0

    property string visibleMessage: ""

    Behavior on opacity {
        NumberAnimation { duration: 180; easing.type: Easing.InOutQuad }
    }

    Row {
        id: unsafeShutdownRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 10

        Rectangle {
            width: 22
            height: 22
            radius: 11
            color: "#3a2505"
            border.color: "#a76a16"
            anchors.verticalCenter: parent.verticalCenter

            Text {
                anchors.centerIn: parent
                text: "!"
                color: "#ffb347"
                font.pixelSize: 14
                font.bold: true
            }
        }

        Text {
            width: parent.width - 64
            text: uncleanShutdownWarning.visibleMessage
            color: "#dfc08a"
            font.pixelSize: window.sp(12)
            wrapMode: Text.WordWrap
            anchors.verticalCenter: parent.verticalCenter
        }

        Rectangle {
            width: 22
            height: 22
            radius: 3
            anchors.verticalCenter: parent.verticalCenter
            color: unsafeShutdownDismissHover.hovered ? "#3a2610" : "#24180a"
            border.color: "#6a4518"
            HoverHandler { id: unsafeShutdownDismissHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: window.uncleanShutdownWarningVisible = false }

            Text {
                anchors.centerIn: parent
                text: "x"
                color: "#b78b4a"
                font.pixelSize: 11
                font.bold: true
            }
        }
    }
}

// ── Audio device fallback notification ───────────────────────────────────
Rectangle {
    id: audioFallbackToast
    anchors.bottom: parent.bottom
    anchors.bottomMargin: 20
    anchors.horizontalCenter: parent.horizontalCenter
    width: Math.min(parent.width * 0.9, 600)
    height: toastCol.implicitHeight + 20
    radius: 6
    color: "#1a1200"
    border.color: "#7a4800"
    z: 998
    visible: opacity > 0
    opacity: 0.0

    property string message: ""

    Connections {
        target: typeof deckA !== "undefined" && deckA ? deckA : null
        function onAudioDeviceFallbackChanged() {
            var msg = deckA ? deckA.audioDeviceFallbackMessage : ""
            if (msg) {
                audioFallbackToast.message = msg
                audioFallbackToast.opacity = 1.0
                audioFallbackDismissTimer.restart()
            } else {
                audioFallbackToast.opacity = 0.0
            }
        }
    }

    Timer {
        id: audioFallbackDismissTimer
        interval: 12000
        repeat: false
        onTriggered: audioFallbackToast.opacity = 0.0
    }

    Behavior on opacity {
        NumberAnimation { duration: 300; easing.type: Easing.InOutQuad }
    }

    Row {
        id: toastCol
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 10

        Text {
            text: "⚠"
            color: "#ff9900"
            font.pixelSize: 14
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            width: parent.width - 60
            text: audioFallbackToast.message
            color: "#ccaa66"
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            anchors.verticalCenter: parent.verticalCenter
        }

        Rectangle {
            width: 22; height: 22
            radius: 3
            anchors.verticalCenter: parent.verticalCenter
            color: dismissH.hovered ? "#2a0000" : "#1a0000"
            border.color: "#442222"
            HoverHandler { id: dismissH; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: audioFallbackToast.opacity = 0.0 }
            Text { anchors.centerIn: parent; text: "✕"; color: "#885555"; font.pixelSize: 10 }
        }
    }
}
}
