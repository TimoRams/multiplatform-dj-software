import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import DJSoftware

Item {
    id: root
    required property var appWindow
    readonly property var window: appWindow
    property alias librarySection: librarySection

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            Layout.minimumHeight: 1
            Layout.preferredHeight: 1
            Layout.maximumHeight: 1
            color: UiTheme.divider
        }

        Item {
            id: waveformViewport
            Layout.fillWidth: true
            Layout.minimumHeight: window.showWaveforms && !window.allInOnePanelActive && !window.sourcePageActive ? window.waveformMinimumHeight : 0
            Layout.preferredHeight: window.showWaveforms && !window.allInOnePanelActive && !window.sourcePageActive ? window.adaptiveWaveformHeight : 0
            Layout.maximumHeight: window.showWaveforms && !window.allInOnePanelActive && !window.sourcePageActive ? window.adaptiveWaveformHeight : 0
            visible: window.showWaveforms && !window.allInOnePanelActive && !window.sourcePageActive
            clip: true

            Item {
                id: waveformCanvas
                anchors.fill: parent

                Item {
                    id: waveformSection
                    anchors.fill: parent
                    readonly property real renderDpr: window._dpr()
                    readonly property real physicalRowHeight:
                        Math.floor(height * renderDpr * 0.25) / renderDpr

                    Loader {
                        id: twoDeckWaveformLoader
                        anchors.fill: parent
                        active: !window.fourDeckMode
                        sourceComponent: Component {
                            PerformanceWaveformScreen {
                                deckAEngine: deckA
                                deckBEngine: deckB
                                fx: fxManager
                                waveformZoom: window.waveformZoom
                            }
                        }
                    }

                    Loader {
                        id: fourDeckWaveformLoader
                        anchors.fill: parent
                        active: window.fourDeckMode
                        sourceComponent: Component {
                            Item {
                                EnlargedWaveform {
                                    x: 0
                                    y: 0
                                    width: parent.width
                                    height: waveformSection.physicalRowHeight
                                    deckName: "C"
                                    engine: deckC
                                    sameTrackDoubleHint: window.isDuplicatePlayingTrack(deckC)
                                    backgroundColor: UiTheme.bgDisplay
                                    waveformZoom: window.waveformZoom
                                }

                                EnlargedWaveform {
                                    x: 0
                                    y: waveformSection.physicalRowHeight
                                    width: parent.width
                                    height: waveformSection.physicalRowHeight
                                    deckName: "A"
                                    engine: deckA
                                    sameTrackDoubleHint: window.isDuplicatePlayingTrack(deckA)
                                    backgroundColor: UiTheme.bgDisplay
                                    waveformZoom: window.waveformZoom
                                }

                                EnlargedWaveform {
                                    x: 0
                                    y: waveformSection.physicalRowHeight * 2
                                    width: parent.width
                                    height: waveformSection.physicalRowHeight
                                    deckName: "B"
                                    engine: deckB
                                    sameTrackDoubleHint: window.isDuplicatePlayingTrack(deckB)
                                    backgroundColor: UiTheme.bgDisplay
                                    waveformZoom: window.waveformZoom
                                }

                                EnlargedWaveform {
                                    x: 0
                                    y: waveformSection.physicalRowHeight * 3
                                    width: parent.width
                                    height: Math.max(0, parent.height - y)
                                    deckName: "D"
                                    engine: deckD
                                    sameTrackDoubleHint: window.isDuplicatePlayingTrack(deckD)
                                    backgroundColor: UiTheme.bgDisplay
                                    waveformZoom: window.waveformZoom
                                }
                            }
                        }
                    }

                }
            }
        }

        Rectangle {
            visible: window.showWaveforms && !window.allInOnePanelActive && !window.sourcePageActive && (window.primaryDeckRowVisible || window.secondaryDeckRowVisible || window.crossfaderVisible || window.fxVisible || window.effectiveLibraryVisible)
            Layout.fillWidth: true
            Layout.minimumHeight: visible ? 1 : 0
            Layout.preferredHeight: visible ? 1 : 0
            Layout.maximumHeight: visible ? 1 : 0
            color: UiTheme.divider
        }

        Item {
            id: deckMixerViewport
            Layout.fillWidth: true
            Layout.minimumHeight: window.primaryDeckRowVisible ? window.scaledDeckMixerHeight : 0
            Layout.preferredHeight: window.primaryDeckRowVisible ? window.scaledDeckMixerHeight : 0
            Layout.maximumHeight: window.primaryDeckRowVisible ? window.scaledDeckMixerHeight : 0
            visible: window.primaryDeckRowVisible
            clip: true

            readonly property real designWidth: window.baseUiWidth
            readonly property real designHeight: window.baseDeckMixerHeight
            readonly property real uniformScaleRaw: Math.min(
                width / Math.max(1, designWidth),
                height / Math.max(1, designHeight)
            )
            readonly property real uniformScale: window._snapScaleToPhysicalPixels(Math.max(0.1, uniformScaleRaw))

            Item {
                id: deckMixerCanvas
                width: deckMixerViewport.designWidth
                height: deckMixerViewport.designHeight
                anchors.centerIn: parent
                scale: deckMixerViewport.uniformScale
                transformOrigin: Item.Center

                RowLayout {
                    id: deckRow
                    anchors.fill: parent
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    spacing: 0

                    DeckControl {
                        deckName: "A"
                        visible: window.showDeckA
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckA
                        hostWindow: window
                        mixerControl: mixerControl
                        channelId: "deckA"
                    }

                    DeckControl {
                        deckName: "B"
                        visible: window.showDeckB
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        engine: deckB
                        hostWindow: window
                        mixerControl: mixerControl
                        channelId: "deckB"
                    }
                }
            }
        }

        // ── Second deck row (4-deck mode) ─────────────────────────────────
        Item {
            id: deckMixerViewport2
            property bool _vis: window.secondaryDeckRowVisible
            Layout.fillWidth: true
            Layout.minimumHeight: _vis ? window.scaledDeckMixerHeight : 0
            Layout.preferredHeight: _vis ? window.scaledDeckMixerHeight : 0
            Layout.maximumHeight: _vis ? window.scaledDeckMixerHeight : 0
            visible: _vis
            clip: true

            readonly property real designWidth: window.baseUiWidth
            readonly property real designHeight: window.baseDeckMixerHeight
            readonly property real uniformScaleRaw: Math.min(
                width  / Math.max(1, designWidth),
                height / Math.max(1, designHeight)
            )
            readonly property real uniformScale: window._snapScaleToPhysicalPixels(Math.max(0.1, uniformScaleRaw))

            Loader {
                active: window.fourDeckMode
                width: deckMixerViewport2.designWidth
                height: deckMixerViewport2.designHeight
                anchors.centerIn: parent
                scale: deckMixerViewport2.uniformScale
                transformOrigin: Item.Center
                sourceComponent: Component {
                    Item {
                        RowLayout {
                            anchors.fill: parent
                            spacing: 0

                            DeckControl {
                                deckName: "C"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                engine: deckC
                                hostWindow: window
                                mixerControl: mixerControl
                                channelId: "deckC"
                            }

                            DeckControl {
                                deckName: "D"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                engine: deckD
                                hostWindow: window
                                mixerControl: mixerControl
                                channelId: "deckD"
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            visible: window.fourDeckMode && !window.libraryExpanded && !window.allInOnePanelActive
            Layout.fillWidth: true
            Layout.minimumHeight: visible ? 1 : 0
            Layout.preferredHeight: visible ? 1 : 0
            Layout.maximumHeight: visible ? 1 : 0
            color: UiTheme.divider
        }

        CrossfaderBar {
            id: crossfaderBar
            property bool _vis: window.crossfaderVisible
            Layout.fillWidth: true
            Layout.minimumHeight:  _vis ? window.crossfaderBarHeight : 0
            Layout.preferredHeight: _vis ? window.crossfaderBarHeight : 0
            Layout.maximumHeight:  _vis ? window.crossfaderBarHeight : 0
            visible: _vis
            mc: mixerControl
            engineA: deckA
            engineB: deckB
            engineC: deckC
            engineD: deckD
            fourDeckMode: window.fourDeckMode
        }

        Rectangle {
            property bool _vis: window.crossfaderVisible
            visible: _vis
            Layout.fillWidth: true
            Layout.minimumHeight:  _vis ? 1 : 0
            Layout.preferredHeight: _vis ? 1 : 0
            Layout.maximumHeight:  _vis ? 1 : 0
            color: UiTheme.divider
        }

        FxBar {
            id: fxBarSection
            Layout.fillWidth: true
            Layout.minimumHeight: window.fxVisible ? window.fxBarHeight : 0
            Layout.preferredHeight: window.fxVisible ? window.fxBarHeight : 0
            Layout.maximumHeight: window.fxVisible ? window.fxBarHeight : 0
            visible: window.fxVisible
        }

        Loader {
            id: settingsSectionLoader
            Layout.fillWidth: true
            Layout.fillHeight: window.settingsPanelActive
            Layout.minimumHeight: window.settingsPanelActive ? 1 : 0
            Layout.preferredHeight: 0
            Layout.maximumHeight: window.settingsPanelActive ? window.height : 0
            visible: window.settingsPanelActive
            active: window.settingsPanelActive
            asynchronous: true
            sourceComponent: Component { SettingsPanel { } }
        }

        Loader {
            id: sourcePageLoader
            Layout.fillWidth: true
            Layout.fillHeight: window.sourcePageActive
            Layout.minimumHeight: window.sourcePageActive ? 1 : 0
            Layout.preferredHeight: 0
            Layout.maximumHeight: window.sourcePageActive ? window.height : 0
            visible: window.sourcePageActive
            active: window.sourcePageActive
            asynchronous: true
            sourceComponent: Component {
                SourcePage {
                    appWindow: window
                    activeSourceType: librarySection.activeTab === "usb" ? "usb" : "local"
                    activeDeviceId: deviceLibraryManager ? deviceLibraryManager.selectedDeviceId : ""
                    onLibraryRequested: {
                        librarySection.activeTab = "library"
                        window.activeMainTab = "library"
                        window.showLibrary = true
                    }
                    onUsbLibraryRequested: {
                        librarySection.resetUsbNavigation()
                        librarySection.activeTab = "usb"
                        window.activeMainTab = "library"
                        window.showLibrary = true
                    }
                    onSourceBackRequested: {
                        window.activeMainTab = "performance"
                    }
                }
            }
        }

        Library {
            id: librarySection
            Layout.fillWidth: true
            Layout.fillHeight: window.effectiveLibraryVisible && !window.sourcePageActive
            Layout.minimumHeight: window.effectiveLibraryVisible && !window.sourcePageActive ? 1 : 0
            Layout.preferredHeight: 0
            Layout.maximumHeight: window.effectiveLibraryVisible && !window.sourcePageActive ? window.height : 0
            visible: window.effectiveLibraryVisible && !window.sourcePageActive
        }
    }
}
