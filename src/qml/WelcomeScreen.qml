import QtQuick
import QtQuick.Layouts

Item {
    id: welcomeScreen
    anchors.fill: parent
    z: 999

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
        opacity: 0.78
        MouseArea { anchors.fill: parent }
    }

    // ── Welcome card ─────────────────────────────────────────────────────────
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width * 0.92, 620)
        height: cardContent.implicitHeight + 56
        color: "#18181a"
        border.color: "#2a2a2a"
        radius: 8

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
                GradientStop { position: 0.6; color: "#e07000" }
                GradientStop { position: 1.0; color: "#5a2d00" }
            }
        }

        ColumnLayout {
            id: cardContent
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 40
            anchors.leftMargin: 40
            anchors.rightMargin: 40
            spacing: 0

            // App name
            Text {
                Layout.fillWidth: true
                text: "BrockDJ"
                color: "#ff9900"
                font.pixelSize: 28
                font.bold: true
                font.letterSpacing: 1.2
                horizontalAlignment: Text.AlignHCenter
            }

            Item { Layout.preferredHeight: 10 }

            // Status badges
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: badgeRow.implicitHeight

                Row {
                    id: badgeRow
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 8

                    Rectangle {
                        height: 20
                        width: preAlphaText.implicitWidth + 14
                        color: "#1f0d00"
                        border.color: "#7a3d00"
                        radius: 3
                        Text {
                            id: preAlphaText
                            anchors.centerIn: parent
                            text: "PRE-ALPHA"
                            color: "#e07000"
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 1.0
                        }
                    }

                    Rectangle {
                        height: 20
                        width: devBuildText.implicitWidth + 14
                        color: "#111a11"
                        border.color: "#2a4a2a"
                        radius: 3
                        Text {
                            id: devBuildText
                            anchors.centerIn: parent
                            text: "DEVELOPER BUILD"
                            color: "#3a8a3a"
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 1.0
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: 20 }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#242424" }

            Item { Layout.preferredHeight: 20 }

            // Vision paragraph
            Text {
                Layout.fillWidth: true
                text: "BrockDJ is being built to become a full-featured, performance-grade DJ platform — designed for live venues, club sets, and professional hardware integration. The goal is a fast, modern tool that holds up under real-world stage conditions."
                color: "#aaaaaa"
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Item { Layout.preferredHeight: 20 }

            // ── Warning box ──────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                // contentHeight is the reliable rendered height for word-wrapped
                // text; avoids Column/implicitHeight timing issues with scaling.
                height: warnTitle.contentHeight + warnBody.contentHeight + 64
                color: "#120c00"
                border.color: "#3d2500"
                radius: 5

                // Left accent stripe — positioned without bottom-anchor to
                // avoid a circular dependency on parent.height
                Rectangle {
                    x: 0
                    y: 6
                    width: 3
                    height: parent.height - 12
                    radius: 2
                    color: "#cc6600"
                }

                Text {
                    id: warnTitle
                    x: 18
                    y: 12
                    width: parent.width - 32
                    text: "⚠  Ultra-Early State — Not Suitable for Live Use"
                    color: "#cc7700"
                    font.pixelSize: 12
                    font.bold: true
                    wrapMode: Text.WordWrap
                }

                Text {
                    id: warnBody
                    x: 18
                    y: warnTitle.y + warnTitle.contentHeight + 8
                    width: parent.width - 32
                    text: "This software is in an extremely early stage of development. Core features are still actively being built, APIs are unstable, and crashes or audio glitches are to be expected. Do not use this for live events, professional performances, or any situation where reliability matters."
                    color: "#886644"
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }

            Item { Layout.preferredHeight: 18 }

            // Feedback + GitHub
            Text {
                Layout.fillWidth: true
                text: "Your testing and feedback directly shape what this project becomes."
                color: "#606060"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Item { Layout.preferredHeight: 10 }

            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: 8

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Source & updates:"
                    color: "#505050"
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

            Item { Layout.preferredHeight: 24 }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#242424" }

            Item { Layout.preferredHeight: 18 }

            // Bottom row: checkbox + button
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

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
                        color: "#555"
                        font.pixelSize: 12
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: checkBox.dontShowAgain = !checkBox.dontShowAgain }
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 120; height: 36
                    radius: 4
                    color: startHov.hovered ? "#d08000" : "#ff9900"
                    HoverHandler { id: startHov; cursorShape: Qt.PointingHandCursor }

                    Text {
                        anchors.centerIn: parent
                        text: "Let's Go"
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
