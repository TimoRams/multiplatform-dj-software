import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Window {
    id: settingsWindow
    title: "Settings"
    width: 800
    height: 600
    minimumWidth: 600
    minimumHeight: 400
    visible: false
    color: "#1e1e19"
    flags: Qt.Dialog

    // ── Navigation categories ────────────────────────────────────────────────
    property int selectedCategory: 0

    readonly property var categories: [
        { label: "Audio Setup",     icon: "♪" },
        { label: "MIDI Controller", icon: "⎘" },
        { label: "Library",         icon: "☰" },
    ]

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── LEFT SIDEBAR ────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            color: "#141414"

            // Top: app / window title
            Rectangle {
                id: sidebarHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 48
                color: "#0f0f0f"

                Text {
                    anchors.centerIn: parent
                    text: "SETTINGS"
                    color: "#ff9900"
                    font.pixelSize: 13
                    font.bold: true
                    font.letterSpacing: 2
                }
            }

            // Separator
            Rectangle {
                anchors.top: sidebarHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: "#2a2a2a"
            }

            // Category list
            Column {
                anchors.top: sidebarHeader.bottom
                anchors.topMargin: 12
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 2

                Repeater {
                    model: settingsWindow.categories

                    delegate: Rectangle {
                        required property var modelData
                        required property int index

                        width: parent.width
                        height: 40
                        color: settingsWindow.selectedCategory === index
                               ? "#2a2a2a"
                               : containsMouse ? "#1e1e1e" : "transparent"
                        property bool containsMouse: false

                        // Active indicator bar (left edge)
                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            radius: 1
                            color: "#ff9900"
                            visible: settingsWindow.selectedCategory === index
                        }

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 18
                            spacing: 10

                            Text {
                                text: modelData.icon
                                color: settingsWindow.selectedCategory === index
                                       ? "#ff9900" : "#666"
                                font.pixelSize: 14
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Text {
                                text: modelData.label
                                color: settingsWindow.selectedCategory === index
                                       ? "#f0f0f0" : "#888"
                                font.pixelSize: 12
                                font.bold: settingsWindow.selectedCategory === index
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            hoverEnabled: true
                            onEntered: parent.containsMouse = true
                            onExited:  parent.containsMouse = false
                            onClicked: settingsWindow.selectedCategory = index
                        }
                    }
                }
            }

            // Bottom: version tag
            Text {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Ramsbrock DJ Engine"
                color: "#333"
                font.pixelSize: 10
                font.family: "monospace"
            }
        }

        // Sidebar / content separator
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: "#2a2a2a"
        }

        // ── RIGHT CONTENT AREA ───────────────────────────────────────────────
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: settingsWindow.selectedCategory

            // ── Page 0: Audio Setup ────────────────────────────────────────
            Item {
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"

                    ColumnLayout {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 30
                        spacing: 20

                        Text {
                            text: "Audio Setup"
                            color: "#f0f0f0"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: "#2a2a2a"
                        }

                        // Output Device row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: "Output Device"
                                color: "#aaa"
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 32
                                color: "#252525"
                                border.color: "#3a3a3a"
                                radius: 4

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 12
                                    text: "System Default"
                                    color: "#ccc"
                                    font.pixelSize: 12
                                }
                            }
                        }

                        // Sample Rate row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: "Sample Rate"
                                color: "#aaa"
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 32
                                color: "#252525"
                                border.color: "#3a3a3a"
                                radius: 4

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 12
                                    text: "44100 Hz"
                                    color: "#ccc"
                                    font.pixelSize: 12
                                }
                            }
                        }

                        // Buffer Size row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Text {
                                text: "Buffer Size"
                                color: "#aaa"
                                font.pixelSize: 12
                                Layout.preferredWidth: 130
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 32
                                color: "#252525"
                                border.color: "#3a3a3a"
                                radius: 4

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 12
                                    text: "512 samples  (≈ 11.6 ms)"
                                    color: "#ccc"
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }
            }

            // ── Page 1: MIDI Controller ────────────────────────────────────
            Item {
                ColumnLayout {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 30
                    spacing: 20

                    Text {
                        text: "MIDI Controller"
                        color: "#f0f0f0"
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#2a2a2a"
                    }

                    // Placeholder device list
                    Repeater {
                        model: ["No MIDI devices detected"]
                        delegate: Rectangle {
                            required property string modelData
                            Layout.fillWidth: true
                            height: 36
                            color: "#252525"
                            border.color: "#333"
                            radius: 4

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                text: modelData
                                color: "#555"
                                font.pixelSize: 12
                                font.family: "monospace"
                            }
                        }
                    }
                }
            }

            // ── Page 2: Library ────────────────────────────────────────────
            Item {
                ColumnLayout {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 30
                    spacing: 20

                    Text {
                        text: "Library"
                        color: "#f0f0f0"
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#2a2a2a"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Text {
                            text: "Music Folder"
                            color: "#aaa"
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 32
                            color: "#252525"
                            border.color: "#3a3a3a"
                            radius: 4

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                text: "~/Music"
                                color: "#ccc"
                                font.pixelSize: 12
                                font.family: "monospace"
                            }
                        }

                        Rectangle {
                            width: 70
                            height: 32
                            color: "#2a2a2a"
                            border.color: "#444"
                            radius: 4

                            Text {
                                anchors.centerIn: parent
                                text: "Browse"
                                color: "#bbb"
                                font.pixelSize: 11
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: console.log("Browse library folder")
                            }
                        }
                    }
                }
            }
        }
    }
}
