import QtQuick
import QtQuick.Layouts

Item {
    id: welcomeScreen
    anchors.fill: parent
    z: 999

    // Filled from outside — set to true once the loading screen has finished
    property bool active: false

    visible: active
    opacity: active ? 1.0 : 0.0

    Behavior on opacity {
        NumberAnimation { duration: 280; easing.type: Easing.InOutQuad }
    }

    // Dim backdrop
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.72
        MouseArea { anchors.fill: parent }
    }

    // ── Welcome card ─────────────────────────────────────────────────────────
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width * 0.88, 560)
        height: cardContent.implicitHeight + 52
        color: "#18181580" // semi-transparent dark
        border.color: "#ff990044"
        radius: 6

        // Solid background so text is readable
        Rectangle {
            anchors.fill: parent
            color: "#18181a"
            radius: parent.radius
        }

        // Top accent line
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 3
            radius: 3
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#ff9900" }
                GradientStop { position: 1.0; color: "#cc6600" }
            }
        }

        ColumnLayout {
            id: cardContent
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 36
            anchors.leftMargin: 36
            anchors.rightMargin: 36
            spacing: 0

            // Title
            Text {
                Layout.fillWidth: true
                text: "Welcome to RamsbrockDJ"
                color: "#ff9900"
                font.pixelSize: 22
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }

            Item { Layout.preferredHeight: 12 }

            // Subtitle
            Text {
                Layout.fillWidth: true
                text: "An open-source DJ software built for experimentation and real-world use."
                color: "#aaaaaa"
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Item { Layout.preferredHeight: 24 }

            // Divider
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: "#2a2a2a"
            }

            Item { Layout.preferredHeight: 20 }

            // Body text
            Text {
                Layout.fillWidth: true
                text: "This is early-stage developer software. Features are actively being built and may change between versions. Your feedback helps shape the direction of the project."
                color: "#888888"
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                lineHeight: 1.5
            }

            Item { Layout.preferredHeight: 20 }

            // GitHub link
            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: 8

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Source & updates:"
                    color: "#666"
                    font.pixelSize: 12
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    height: 24
                    width: ghLinkText.implicitWidth + 16
                    color: ghHov.hovered ? "#1a1000" : "transparent"
                    border.color: ghHov.hovered ? "#ff9900" : "transparent"
                    radius: 3

                    Text {
                        id: ghLinkText
                        anchors.centerIn: parent
                        text: "github.com/timorams"
                        color: ghHov.hovered ? "#ff9900" : "#cc7700"
                        font.pixelSize: 12
                        font.underline: ghHov.hovered
                    }

                    HoverHandler { id: ghHov; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: Qt.openUrlExternally("https://github.com/timorams") }
                }
            }

            Item { Layout.preferredHeight: 28 }

            // Divider
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: "#2a2a2a"
            }

            Item { Layout.preferredHeight: 18 }

            // Bottom row: checkbox + Start button
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                // Don't show again checkbox
                Row {
                    spacing: 8
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        id: checkBox
                        width: 16; height: 16
                        radius: 2
                        color: dontShowAgain ? "#ff9900" : "#1e1e1e"
                        border.color: dontShowAgain ? "#ff9900" : "#444"
                        anchors.verticalCenter: parent.verticalCenter

                        property bool dontShowAgain: true

                        Text {
                            anchors.centerIn: parent
                            text: "✓"
                            color: "#000"
                            font.pixelSize: 11
                            font.bold: true
                            visible: parent.dontShowAgain
                        }

                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: checkBox.dontShowAgain = !checkBox.dontShowAgain }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Don't show again on startup"
                        color: "#666"
                        font.pixelSize: 12
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: checkBox.dontShowAgain = !checkBox.dontShowAgain }
                    }
                }

                Item { Layout.fillWidth: true }

                // Start button
                Rectangle {
                    width: 110; height: 36
                    radius: 4
                    color: startHov.hovered ? "#d08000" : "#ff9900"
                    HoverHandler { id: startHov; cursorShape: Qt.PointingHandCursor }

                    Text {
                        anchors.centerIn: parent
                        text: "Start"
                        color: "#000"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    TapHandler {
                        onTapped: {
                            if (appConfig)
                                appConfig.completeFirstRun(checkBox.dontShowAgain)
                            welcomeScreen.active = false
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: 4 }
        }
    }
}
