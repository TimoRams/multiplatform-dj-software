#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef BROCKDJ_SOURCE_DIR
#error BROCKDJ_SOURCE_DIR is required
#endif

namespace {
std::string read(const char* relative)
{
    std::ifstream file(std::string(BROCKDJ_SOURCE_DIR) + "/" + relative);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
std::size_t occurrences(const std::string& value, const std::string& needle)
{
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}
}

int main()
{
    bool ok = true;
    const auto main = read("src/qml/main.qml");
    const auto topHeader = read("src/qml/shell/TopHeader.qml");
    const auto deckControl = read("src/qml/deck/DeckControl.qml");
    const auto mixerSection = read("src/qml/mixer/MixerSection.qml");
    const auto workspace = read("src/qml/performance/PerformanceWorkspace.qml");
    const auto shortcuts = read("src/qml/components/UiShortcutManager.qml");
    const auto enlargedWaveform = read("src/qml/waveform/EnlargedWaveform.qml");
    const auto overallWaveform = read("src/qml/waveform/OverallWaveform.qml");
    const auto turntableIndicator = read("src/qml/deck/TurntableIndicator.qml");
    const auto settingsPanel = read("src/qml/settings/SettingsPanel.qml");
    const auto settingsWindow = read("src/qml/settings/SettingsWindow.qml");
    const auto applicationBootstrap = read("src/app/ApplicationBootstrap.cpp");
    const auto performancePads = read("src/qml/performance/PerformancePads.qml");
    const auto library = read("src/qml/library/Library.qml");
    const auto sourcePage = read("src/qml/library/SourcePage.qml");
    const auto waveformScreen = read("src/qml/performance/PerformanceWaveformScreen.qml");
    const auto beatFxPanel = read("src/qml/performance/PerformanceBeatFxPanel.qml");
    const auto deckQuickPanel = read("src/qml/deck/PerformanceDeckQuickPanel.qml");
    const auto developmentControls = read("src/qml/development/DevelopmentControlsWindow.qml");
    const auto flx10Mapping = read("src/controllers/mappings/midi/DDJ-FLX10.brockdj.xml");
    const auto flx10MidiBridge = read("src/controllers/flx10/Flx10MidiBridge.cpp");
    const auto engineHeader = read("src/deck/DjEngine.h");
    const auto midiManagerHeader = read("src/controllers/midi/MidiControllerManager.h");
    ok &= require(std::count(main.begin(), main.end(), '\n') < 600, "main.qml remains a compact shell");
    ok &= require(main.find("PerformanceWorkspace") != std::string::npos, "shell routes to performance workspace");
    ok &= require(workspace.find("DeckControl") != std::string::npos, "workspace uses shared deck component");
    ok &= require(workspace.find("MixerSection") == std::string::npos
                      && developmentControls.find("MixerSection") != std::string::npos,
                  "production workspace omits hidden mixer trees while development retains the mixer UI");
    ok &= require(workspace.find("id: twoDeckWaveformLoader") != std::string::npos
                      && workspace.find("id: fourDeckWaveformLoader") != std::string::npos
                      && workspace.find("active: window.fourDeckMode") != std::string::npos,
                  "mutually exclusive waveform and C/D deck trees load only in their active mode");
    ok &= require(workspace.find("id: settingsSectionLoader") != std::string::npos
                      && workspace.find("id: sourcePageLoader") != std::string::npos
                      && occurrences(workspace, "asynchronous: true") >= 2,
                  "infrequent AIO settings and source surfaces load asynchronously on demand");
    ok &= require(topHeader.find("settingsWindowFactory.createObject(null)") != std::string::npos
                      && topHeader.find("SettingsWindow { id: settingsWin }") == std::string::npos,
                  "desktop settings no longer retain a hidden startup window");
    ok &= require(deckControl.find("onAir: deck.engine ? deck.engine.onAir : false")
                      != std::string::npos
                  && deckControl.find("onAir: deck.engine && deck.engine.isPlaying")
                      == std::string::npos,
                  "ON AIR UI consumes the audio routing snapshot, not transport or level");
    ok &= require(topHeader.find("deckA.masterVuLevelL") != std::string::npos
                  && topHeader.find("deckA.masterVuLevelR") != std::string::npos
                  && topHeader.find("Math.max(deckA ? deckA.vuLevelL") == std::string::npos,
                  "master VU consumes the final master output snapshot");
    ok &= require(mixerSection.find("engineA.preFaderVuLevelL") != std::string::npos
                  && mixerSection.find("normalizedDb(levelLinear)") != std::string::npos,
                  "channel UI meter consumes pre-fader peaks with dBFS height mapping");
    ok &= require(settingsPanel.find("mappingEditorFactory.createObject(null)") != std::string::npos
                      && settingsWindow.find("mappingEditorFactory.createObject(null)") != std::string::npos
                      && settingsPanel.find("id: mappingEditorWindow") == std::string::npos
                      && settingsWindow.find("id: mappingEditorWindow") == std::string::npos,
                  "mapping editors are constructed only when opened");
    ok &= require(main.find("waveformZoomLevels") == std::string::npos, "legacy fixed zoom list removed");
    ok &= require(settingsPanel.find("settingsManager.setAudioConfiguration") != std::string::npos
                      && settingsWindow.find("settingsManager.setAudioConfiguration") != std::string::npos,
                  "audio settings use one atomic persistence operation");
    ok &= require(settingsPanel.find("firstRealOutput(outputOptions)") != std::string::npos
                      && settingsWindow.find("firstRealOutput(outputOptions)") != std::string::npos,
                  "device-list reconciliation never replaces a preference with None");
    ok &= require(settingsPanel.find("pairText === \"None\"") != std::string::npos
                      && settingsWindow.find("pairText === \"None\"") != std::string::npos
                      && settingsPanel.find("deckB.setOutputFirstChannel(masterFirstChannel)")
                          == std::string::npos
                      && settingsWindow.find("deckB.setOutputFirstChannel(masterFirstChannel)")
                          == std::string::npos,
                  "Master routing is normalized explicitly instead of by a hidden deck side effect");
    ok &= require(settingsPanel.find("audioUiSyncing = true\n        audioOutputDeviceOptions =")
                          != std::string::npos
                      && settingsWindow.find("audioUiSyncing = true\n        audioOutputDeviceOptions =")
                          != std::string::npos,
                  "ComboBox model changes are guarded before they can select None");
    const auto audioCallbackRegistration = applicationBootstrap.find(
        "runtime.audioEngine->registerCallback(runtime.audioDeviceService->manager())");
    const auto initialAudioApply = applicationBootstrap.find("const bool audioSettingsApplied");
    ok &= require(audioCallbackRegistration != std::string::npos
                      && initialAudioApply != std::string::npos
                      && audioCallbackRegistration < initialAudioApply,
                  "audio callback is registered before the startup device is opened");
    ok &= require(main.find("resizeThrottleCounter") == std::string::npos, "resize event counter removed");
    ok &= require(shortcuts.find("Ctrl+Shift+0") != std::string::npos
                  && shortcuts.find("Ctrl+0") != std::string::npos, "independent reset shortcuts exist");
    ok &= require(engineHeader.find("Q_PROPERTY(bool scratchVisualActive READ isScratchVisualActive NOTIFY scrubbingChanged)")
                      != std::string::npos,
                  "scratch visual activity is a reactive QML property");
    ok &= require(enlargedWaveform.find("root.engine.scratchVisualActive") != std::string::npos
                  && overallWaveform.find("root.engine.scratchVisualActive") != std::string::npos,
                  "waveform frame animations react to paused scratch state");
    ok &= require(enlargedWaveform.find("FrameAnimation {") != std::string::npos
                      && overallWaveform.find("FrameAnimation {") != std::string::npos
                      && turntableIndicator.find("FrameAnimation {") != std::string::npos
                      && enlargedWaveform.find("waveformMotionIntervalMs <= 17")
                          != std::string::npos
                      && overallWaveform.find("motionIntervalMs <= 17")
                          != std::string::npos
                      && turntableIndicator.find("motionIntervalMs <= 17")
                          != std::string::npos,
                  "moving deck visuals use the presentation clock at full quality");
    ok &= require(enlargedWaveform.find("waveformRasterWorkEnabled")
                          != std::string::npos
                      && enlargedWaveform.find("waveformMotionIntervalMs > 17")
                          != std::string::npos
                      && overallWaveform.find("motionIntervalMs > 17")
                          != std::string::npos
                      && turntableIndicator.find("motionIntervalMs > 17")
                          != std::string::npos,
                  "audio pressure can reduce animation and suspend tile raster work");
    ok &= require(performancePads.find("[\"HOT CUE\", \"PAD FX\", \"BEATJUMP\", \"SAMPLER\"]")
                      != std::string::npos
                  && performancePads.find("selectPerformancePadMode(root.deckId, index)")
                      != std::string::npos,
                  "performance pad tabs and FLX10 mode state stay synchronized");
    ok &= require(performancePads.find("PointerDevice.TouchScreen") != std::string::npos
                  && performancePads.find("root.beginPadPress(index)") != std::string::npos
                  && performancePads.find("root.endPadPress(index)") != std::string::npos,
                  "performance pad pages and hold actions accept native touch input");
    ok &= require(midiManagerHeader.find("setPerformancePadPressed") != std::string::npos
                  && midiManagerHeader.find("consumePerformancePadPlayLatch") != std::string::npos
                  && midiManagerHeader.find("performancePadStateChanged") != std::string::npos,
                  "touch and FLX10 pads share one controller state path");
    ok &= require(performancePads.find("nextMode === 4 ? 3 : nextMode") != std::string::npos
                  && performancePads.find("root.keyShiftMode && index === 3 ? \"KEY SHIFT\"")
                      != std::string::npos
                  && performancePads.find("root.engine.keySemitoneOffset - root.keyShiftValue(index)")
                      != std::string::npos
                  && flx10MidiBridge.find("engine->keySemitoneOffset()")
                      != std::string::npos
                  && flx10MidiBridge.find("&DjEngine::keySemitoneOffsetChanged")
                      != std::string::npos
                  && flx10MidiBridge.find("mode == MidiPadMode::KeyShift") != std::string::npos,
                  "touch and hardware pads highlight the currently selected Key Shift value");
    ok &= require(main.find("onLibraryViewToggleRequested") != std::string::npos,
                  "FLX10 View action toggles the visible library surface");
    ok &= require(library.find("if (!libraryRoot.visible)") != std::string::npos
                  && library.find("waveformZoomController.zoomIn()") != std::string::npos
                  && library.find("waveformZoomController.zoomOut()") != std::string::npos,
                  "FLX10 browse encoder controls waveform zoom while the library is hidden");
    ok &= require(library.find("tileLabel: \"SOURCE\"") == std::string::npos
                      && workspace.find("librarySection.activeTab = \"library\"")
                          != std::string::npos
                      && sourcePage.find("function syncCursorToActiveSource()")
                          != std::string::npos
                      && sourcePage.find("color: active ? \"#40d84b\" : \"#d6d8df\"")
                          != std::string::npos,
                  "standalone Source reflects the active Local or USB library");
    ok &= require(library.find("readonly property bool usbTrackViewVisible")
                          != std::string::npos
                      && occurrences(library, "visible: libraryRoot.usbTrackViewVisible") == 3,
                  "USB track chrome cannot reserve space above the playlist browser");
    ok &= require(library.find("function enterUsbPlaylistFolder(folder)")
                          != std::string::npos
                      && library.find("function updateUsbPlaylistPreview()")
                          != std::string::npos
                      && library.find("id: usbPlaylistFolderPreview")
                          != std::string::npos
                      && library.find("id: usbPlaylistPreviewTracks")
                          != std::string::npos
                      && library.find("fullTrackView: libraryRoot.usbTrackViewVisible")
                          != std::string::npos,
                  "USB playlists drill through the left pane, preview tracks on the right, and show loads only in full track views");
    ok &= require(library.find("deviceLibraryManager.mountDevice(modelData.id)")
                          != std::string::npos
                      && library.find("deviceLibraryManager.ejectDevice(modelData.id)")
                          != std::string::npos
                      && library.find("modelData.operationPending ? \"…\"")
                          != std::string::npos
                      && library.find("deviceLibraryManager.selectedDeviceReady")
                          != std::string::npos,
                  "USB device rows expose guarded mount/eject actions and mounted-only navigation");
    ok &= require(occurrences(library, "component SortHeader: Rectangle") == 1
                      && library.find("component PlSortHeader:") == std::string::npos,
                  "library and playlist tables share one sortable header component");
    ok &= require(library.find("readonly property string activeSortField")
                          != std::string::npos
                      && library.find("readonly property bool activeSortAscending")
                          != std::string::npos
                      && library.find("libraryRoot.togglePlaylistSort(sh.field)")
                          != std::string::npos
                      && library.find("libraryModel.toggleSort(sh.field)")
                          != std::string::npos,
                  "shared sort headers preserve both library and playlist dispatch paths");
    ok &= require(occurrences(library, "playlistMode: true") == 6,
                  "all six playlist columns opt into playlist sorting");
    ok &= require(library.find("component AioQuickBtn:") == std::string::npos,
                  "unused AIO quick-button component stays removed");
    ok &= require(main.find("function closeTopBarPullDown()") != std::string::npos
                  && main.find("function openTopBarPullDown()") != std::string::npos
                  && main.find("function toggleTopBarPullDown()") != std::string::npos,
                  "main.qml owns the quick-access tray lifecycle");
    ok &= require(main.find("if (window.topBarPullProgress > 0.0)") != std::string::npos
                  && main.find("window.closeTopBarPullDown()") != std::string::npos,
                  "Escape closes an open quick-access tray before other navigation");
    ok &= require(topHeader.find("id: quickPerformanceMouse") != std::string::npos
                  && topHeader.find("id: quickLibraryMouse") != std::string::npos
                  && topHeader.find("id: quickSettingsMouse") != std::string::npos
                  && topHeader.find("id: quickModeMouse") != std::string::npos
                  && topHeader.find("id: quickFullscreenMouse") != std::string::npos
                  && occurrences(topHeader, "root.Window.window.closeTopBarPullDown()") >= 6,
                  "quick-access navigation and fullscreen actions close the tray");
    ok &= require(flx10Mapping.find("paramId=\"deckA_slip_reverse\" status=\"0x90\" control=\"0x15\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"library_view_toggle\" status=\"0x96\" control=\"0x7A\"")
                      != std::string::npos,
                  "FLX10 Slip Reverse and View controls use the documented notes");
    // Note 0x46, confirmed against the hardware: the switch sends 127 when it
    // latches on and 0 when it releases, so both edges must be dispatched.
    ok &= require(flx10Mapping.find("paramId=\"beat_fx_on\" status=\"0x94\" control=\"0x46\" type=\"momentary\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_beat_minus\" status=\"0x94\" control=\"0x4A\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_beat_plus\" status=\"0x94\" control=\"0x4B\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"sound_color_fx_filter\" status=\"0x96\" control=\"0x05\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_sound_color\" status=\"0xB6\" control=\"0x17\"")
                      != std::string::npos,
                  "FLX10 Beat FX state and Sound Color controls are mapped");
    ok &= require(flx10Mapping.find("paramId=\"deckA_quantize\" status=\"0x90\" control=\"0x35\"")
                      != std::string::npos
                  && flx10Mapping.find("<DeckLed name=\"quantize\" control=\"0x35\"/>")
                      != std::string::npos,
                  "FLX10 Quantize input and LED feedback share the documented note");
    ok &= require(flx10Mapping.find("paramId=\"deckA_pad_mode_sampler\" status=\"0x90\" control=\"0x22\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_pad_mode_keyshift\" status=\"0x90\" control=\"0x6F\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_pad_mode_keyshift\" status=\"0x91\" control=\"0x6F\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckC_pad_mode_keyshift\" status=\"0x92\" control=\"0x6F\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckD_pad_mode_keyshift\" status=\"0x93\" control=\"0x6F\"")
                      != std::string::npos
                  && flx10MidiBridge.find("sendMappedNoteLed(prefix + QStringLiteral(\"keyshift\")")
                      != std::string::npos
                  && flx10MidiBridge.find("mode == MidiPadMode::Sampler || mode == MidiPadMode::KeyShift")
                      == std::string::npos
                  && flx10MidiBridge.find("mode == MidiPadMode::Sampler && shiftHeld")
                      == std::string::npos,
                  "FLX10 Sampler and shifted Key Shift mode are independent MIDI commands");
    ok &= require(flx10MidiBridge.find("if (padModeForDeck(deck) == MidiPadMode::KeyShift) {")
                      != std::string::npos
                  && flx10MidiBridge.find("handleKeyShiftPad(deck, deckEngine, padIndex,")
                      != std::string::npos,
                  "FLX10 stale Hot Cue pad packets cannot cancel an explicit Key Shift mode");
    const std::string midiParameterDispatch = read("src/controllers/midi/MidiParameterDispatch.cpp");
    ok &= require(midiParameterDispatch.find("decodeFlx10KeyShiftPadWireEvent")
                      != std::string::npos
                  && midiParameterDispatch.find("channel < 7 || channel > 14")
                      != std::string::npos
                  && midiParameterDispatch.find("note >= 0x70 && note <= 0x7f")
                      != std::string::npos
                  && midiParameterDispatch.find("handleKeyShiftPad(keyShiftPad.deck")
                      != std::string::npos,
                  "physical FLX10 Key Shift pads bypass the generic parameter-store route");
    ok &= require(flx10Mapping.find("paramId=\"deckA_keyshift_range_down\" status=\"0x90\" control=\"0x2B\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_keyshift_range_up\" status=\"0x90\" control=\"0x33\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keyshift_range_down\" status=\"0x91\" control=\"0x2B\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keyshift_range_up\" status=\"0x91\" control=\"0x33\"")
                      != std::string::npos,
                  "FLX10 Key Shift PAGE controls use their mode-specific deck notes");
    ok &= require(flx10Mapping.find("paramId=\"deckA_keyshift_pad1\" status=\"0x97\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_keyshift_pad1_shift\" status=\"0x98\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keyshift_pad1\" status=\"0x99\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keyshift_pad1_shift\" status=\"0x9A\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckC_keyshift_pad1\" status=\"0x9B\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckC_keyshift_pad1_shift\" status=\"0x9C\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckD_keyshift_pad1\" status=\"0x9D\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckD_keyshift_pad1_shift\" status=\"0x9E\" control=\"0x70\"")
                      != std::string::npos
                  && flx10MidiBridge.find("handleKeyShiftPad(deck, deckEngine, padIndex, value >= 0.5f,\n                                  shiftedKeyPad)")
                      != std::string::npos,
                  "FLX10 Key Shift pads preserve the hardware's normal/shift channel distinction");
    ok &= require(flx10Mapping.find("paramId=\"deckA_keylock\" status=\"0x90\" control=\"0x4A\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keylock\" status=\"0x91\" control=\"0x4A\"")
                      != std::string::npos,
                  "FLX10 Key Lock uses MIX POINT LINK rather than the Key Shift mode command");
    ok &= require(flx10Mapping.find("paramId=\"deckA_tempo\" status=\"0xB0\" control=\"0x00\" type=\"fader\"/")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_tempo\" status=\"0xB0\" control=\"0x20\" type=\"fader\"/")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_tempo\" status=\"0xB1\" control=\"0x00\" type=\"fader\"/")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_tempo\" status=\"0xB1\" control=\"0x20\" type=\"fader\"/")
                      != std::string::npos,
                  "FLX10 tempo faders map both non-inverted halves of the Pioneer 14-bit data");
    ok &= require(flx10Mapping.find("paramId=\"deckA_tempo_range_cycle\" status=\"0x90\" control=\"0x60\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_tempo_range_cycle\" status=\"0x91\" control=\"0x60\"")
                      != std::string::npos,
                  "FLX10 shifted Tempo Reset cycles the hardware tempo range");
    const auto has14BitPair = [&flx10Mapping](const char* paramId,
                                              const char* status,
                                              const char* msb,
                                              const char* lsb) {
        const std::string prefix = std::string("paramId=\"") + paramId
            + "\" status=\"" + status + "\" control=\"";
        return flx10Mapping.find(prefix + msb + "\"") != std::string::npos
            && flx10Mapping.find(prefix + lsb + "\"") != std::string::npos;
    };
    ok &= require(has14BitPair("beat_fx_level_depth", "0xB4", "0x02", "0x22"),
                  "FLX10 Beat FX LEVEL/DEPTH maps its complete documented 14-bit pair");
    ok &= require(has14BitPair("deckA_gain", "0xB0", "0x04", "0x24")
                  && has14BitPair("deckB_gain", "0xB1", "0x04", "0x24")
                  && has14BitPair("deckA_eqHigh", "0xB0", "0x07", "0x27")
                  && has14BitPair("deckB_eqHigh", "0xB1", "0x07", "0x27")
                  && has14BitPair("deckA_eqMid", "0xB0", "0x0B", "0x2B")
                  && has14BitPair("deckB_eqMid", "0xB1", "0x0B", "0x2B")
                  && has14BitPair("deckA_eqLow", "0xB0", "0x0F", "0x2F")
                  && has14BitPair("deckB_eqLow", "0xB1", "0x0F", "0x2F")
                  && has14BitPair("deckA_vol", "0xB0", "0x13", "0x33")
                  && has14BitPair("deckB_vol", "0xB1", "0x13", "0x33")
                  && has14BitPair("crossfader", "0xB6", "0x1F", "0x3F")
                  && has14BitPair("headphone_mix", "0xB6", "0x0C", "0x2C")
                  && has14BitPair("headphone_level", "0xB6", "0x0D", "0x2D")
                  && has14BitPair("deckA_sound_color", "0xB6", "0x17", "0x37")
                  && has14BitPair("deckB_sound_color", "0xB6", "0x18", "0x38"),
                  "FLX10 mixer faders and knobs map complete coherent 14-bit pairs");
    ok &= require(flx10Mapping.find("paramId=\"master_level\"") == std::string::npos,
                  "FLX10 hardware Master Level never changes the software master gain");
    ok &= require(flx10Mapping.find("paramId=\"master_cue\" status=\"0x96\" control=\"0x63\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_headphone_cue\" status=\"0x90\" control=\"0x54\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_headphone_cue\" status=\"0x91\" control=\"0x54\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckC_headphone_cue\" status=\"0x92\" control=\"0x54\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckD_headphone_cue\" status=\"0x93\" control=\"0x54\"")
                      != std::string::npos
                  && flx10Mapping.find("<DeckLed name=\"headphone_cue\" control=\"0x54\"/>")
                      != std::string::npos,
                  "FLX10 master and all four channel headphone CUE controls use the documented notes");
    ok &= require(flx10Mapping.find("paramId=\"beat_fx_channel_deck_a\" status=\"0x94\" control=\"0x10\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_channel_deck_b\" status=\"0x94\" control=\"0x11\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_channel_master\" status=\"0x94\" control=\"0x14\"")
                      != std::string::npos,
                  "FLX10 Beat FX channel selector follows CH1, CH2 and Master notes");
    ok &= require(waveformScreen.find("property string leftPanel: \"deck\"")
                      != std::string::npos
                  && waveformScreen.find("Math.min(230, Math.max(176, width * 0.155))")
                      != std::string::npos
                  && waveformScreen.find("readonly property real handleWidth: 22")
                      != std::string::npos,
                  "performance song information is open by default with compact side handles");
    ok &= require(waveformScreen.find("clip: true") != std::string::npos
                  && waveformScreen.find(
                         "x: -width * (1.0 - root.leftPanelReveal)")
                      != std::string::npos
                  && waveformScreen.find(
                         "x: root.width - width * root.rightPanelReveal")
                      != std::string::npos
                  && waveformScreen.find("Behavior on x") == std::string::npos,
                  "collapsed performance panels do not lag into view while resizing");
    ok &= require(deckQuickPanel.find("function loadedSourceLabel()") != std::string::npos
                  && deckQuickPanel.find("engine.externalSourceId") != std::string::npos
                  && deckQuickPanel.find("deviceLibraryManager.devices") != std::string::npos
                  && deckQuickPanel.find("text: \"SOURCE\"") != std::string::npos,
                  "compact deck side panel shows the loaded Local or USB source");
    // The neighbouring beat lengths are shown either side of the current one and
    // are selectable directly, the way a player prints them.
    ok &= require(beatFxPanel.find("function setDivisionIndex(index)") != std::string::npos
                  && beatFxPanel.find("fx.setBeatDivision(1, divisions[index].value)")
                      != std::string::npos
                  && beatFxPanel.find("root.setDivisionIndex(divIndex)") != std::string::npos,
                  "compact Beat FX panel exposes selectable beat lengths");
    ok &= require(deckQuickPanel.find("root.engine.ejectTrack()") != std::string::npos
                  && deckQuickPanel.find("!engine.isPlaying") != std::string::npos,
                  "deck side panel can eject a stopped deck");
    return ok ? 0 : 1;
}
