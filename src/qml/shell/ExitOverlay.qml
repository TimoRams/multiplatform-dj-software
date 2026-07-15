import QtQuick
import QtQuick.Controls
import DJSoftware

Rectangle {
    id: exitOverlay
    required property var appWindow
    readonly property var window: appWindow
    anchors.fill: parent
    z: 1000
    visible: window.exitPromptVisible
    color: window.unifiedGray
    focus: visible

    MouseArea {
        anchors.fill: parent
    }

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width * 0.82, 460)
        spacing: 14

        Text {
            width: parent.width
            text: window.exitShutdownInProgress
                  ? "Closing..."
                  : "Are you sure you want to quit?"
            color: "#e5e5e5"
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: window.sp(20)
            font.bold: true
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10
            visible: !window.exitShutdownInProgress

            Rectangle {
                width: 118
                height: 38
                radius: 0
                color: yesArea.pressed ? "#3b3b3b" : "#353535"
                border.width: 1
                border.color: UiTheme.separator

                Text {
                    anchors.centerIn: parent
                    text: "OK"
                    color: "#f0f0f0"
                    font.pixelSize: window.sp(13)
                    font.bold: true
                }

                MouseArea {
                    id: yesArea
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: window.confirmAppClose()
                }
            }

            Rectangle {
                width: 118
                height: 38
                radius: 0
                color: noArea.pressed ? "#3b3b3b" : "#353535"
                border.width: 1
                border.color: UiTheme.separator

                Text {
                    anchors.centerIn: parent
                    text: "Cancel"
                    color: "#f0f0f0"
                    font.pixelSize: window.sp(13)
                    font.bold: true
                }

                MouseArea {
                    id: noArea
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: window.cancelAppClosePrompt()
                }
            }
        }

        Rectangle {
            width: parent.width
            height: 6
            color: "#1f1f1f"
            border.width: 1
            border.color: UiTheme.separator
            visible: window.exitShutdownInProgress

            Rectangle {
                width: parent.width * window.exitProgress
                height: parent.height
                color: "#6f6f6f"
            }
        }

        Text {
            width: parent.width
            visible: window.exitShutdownInProgress
            text: "Saving database and settings..."
            color: "#cfcfcf"
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: window.sp(11)
        }

        Text {
            width: parent.width
            visible: !window.exitShutdownInProgress
            text: (typeof libraryDb !== "undefined" && libraryDb !== null)
                  ? (libraryDb.mirroredDatabaseStatus ? libraryDb.mirroredDatabaseStatus : "DB A: unknown | DB B: unknown")
                  : "DB A: unknown | DB B: unknown"
            color: "#b8b8b8"
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: window.sp(11)
        }

        CheckBox {
            id: manualBackupCheck
            visible: !window.exitShutdownInProgress
            checked: false
            padding: 0
            spacing: 8
            text: "Save manual database backup"
            indicator: Rectangle {
                implicitWidth: 16
                implicitHeight: 16
                radius: 2
                border.width: 1
                border.color: parent.checked ? "#9a9a9a" : "#5a5a5a"
                color: parent.checked ? "#4c4c4c" : "#232323"

                Rectangle {
                    anchors.centerIn: parent
                    width: 8
                    height: 8
                    radius: 1
                    color: parent.visible && parent.parent.checked ? "#e5e5e5" : "transparent"
                    visible: parent.parent.checked
                }
            }
            contentItem: Text {
                text: manualBackupCheck.text
                color: "#f0f0f0"
                font.pixelSize: window.sp(12)
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                leftPadding: manualBackupCheck.indicator.width + manualBackupCheck.spacing
            }
            onCheckedChanged: {
                if (window.exitShutdownInProgress)
                    return
                window.exitManualBackupRequested = checked
            }
        }

        Text {
            width: parent.width
            visible: !window.exitShutdownInProgress
            text: "When enabled, a separate backup file is saved on exit, only updated through this option."
            color: "#a8a8a8"
            font.pixelSize: window.sp(10)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }
}
