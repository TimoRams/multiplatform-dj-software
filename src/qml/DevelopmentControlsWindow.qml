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
    minimumWidth: 880
    minimumHeight: 340
    visible: appWindow && appWindow.showDevelopmentControls
    title: "BrockDJ — Development Controls"
    color: UiTheme.bgDeep

    // FxBar historically derives its height from the containing window.
    readonly property int fxBarHeight: UiMetrics.px(90)

    onVisibleChanged: {
        if (!visible && appWindow)
            appWindow.showDevelopmentControls = false
    }

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
                    Layout.preferredHeight: 138
                    Layout.minimumHeight: 138
                    Layout.maximumHeight: 138
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
                        Layout.fillHeight: true
                        Layout.preferredWidth: UiMetrics.mixerPreferredWidth
                        Layout.minimumWidth: UiMetrics.mixerPreferredWidth
                        Layout.maximumWidth: UiMetrics.mixerPreferredWidth
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
                    Layout.preferredHeight: visible ? 138 : 0
                    Layout.minimumHeight: visible ? 138 : 0
                    Layout.maximumHeight: visible ? 138 : 0
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
                        Layout.fillHeight: true
                        Layout.preferredWidth: UiMetrics.mixerPreferredWidth
                        Layout.minimumWidth: UiMetrics.mixerPreferredWidth
                        Layout.maximumWidth: UiMetrics.mixerPreferredWidth
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
                    Layout.preferredHeight: UiMetrics.px(36)
                    Layout.minimumHeight: UiMetrics.px(36)
                    Layout.maximumHeight: UiMetrics.px(36)
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
