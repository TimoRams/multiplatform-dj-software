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
}

int main()
{
    bool ok = true;
    const auto main = read("src/qml/main.qml");
    const auto workspace = read("src/qml/performance/PerformanceWorkspace.qml");
    const auto shortcuts = read("src/qml/shared/UiShortcutManager.qml");
    const auto enlargedWaveform = read("src/qml/EnlargedWaveform.qml");
    const auto overallWaveform = read("src/qml/OverallWaveform.qml");
    const auto performancePads = read("src/qml/PerformancePads.qml");
    const auto flx10Mapping = read("src/controllers/mappings/midi/DDJ-FLX10.brockdj.xml");
    const auto engineHeader = read("src/engine/DjEngine.h");
    ok &= require(std::count(main.begin(), main.end(), '\n') < 600, "main.qml remains a compact shell");
    ok &= require(main.find("PerformanceWorkspace") != std::string::npos, "shell routes to performance workspace");
    ok &= require(workspace.find("DeckControl") != std::string::npos, "workspace uses shared deck component");
    ok &= require(workspace.find("MixerSection") != std::string::npos, "workspace uses shared mixer component");
    ok &= require(main.find("waveformZoomLevels") == std::string::npos, "legacy fixed zoom list removed");
    ok &= require(main.find("resizeThrottleCounter") == std::string::npos, "resize event counter removed");
    ok &= require(shortcuts.find("Ctrl+Shift+0") != std::string::npos
                  && shortcuts.find("Ctrl+0") != std::string::npos, "independent reset shortcuts exist");
    ok &= require(engineHeader.find("Q_PROPERTY(bool scratchVisualActive READ isScratchVisualActive NOTIFY scrubbingChanged)")
                      != std::string::npos,
                  "scratch visual activity is a reactive QML property");
    ok &= require(enlargedWaveform.find("root.engine.scratchVisualActive") != std::string::npos
                  && overallWaveform.find("root.engine.scratchVisualActive") != std::string::npos,
                  "waveform frame animations react to paused scratch state");
    ok &= require(performancePads.find("[\"HOT CUE\", \"PAD FX\", \"BEATJUMP\", \"SAMPLER\"]")
                      != std::string::npos
                  && performancePads.find("selectPerformancePadMode(root.deckId, index)")
                      != std::string::npos,
                  "performance pad tabs and FLX10 mode state stay synchronized");
    ok &= require(main.find("onLibraryViewToggleRequested") != std::string::npos,
                  "FLX10 View action toggles the visible library surface");
    ok &= require(flx10Mapping.find("paramId=\"deckA_slip_reverse\" status=\"0x90\" control=\"0x15\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"library_view_toggle\" status=\"0x96\" control=\"0x7A\"")
                      != std::string::npos,
                  "FLX10 Slip Reverse and View controls use the documented notes");
    ok &= require(flx10Mapping.find("paramId=\"beat_fx_on\" status=\"0x94\" control=\"0x46\" type=\"momentary\"")
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
    ok &= require(flx10Mapping.find("paramId=\"beat_fx_channel_deck_a\" status=\"0x94\" control=\"0x10\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_channel_deck_b\" status=\"0x94\" control=\"0x11\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_channel_master\" status=\"0x94\" control=\"0x14\"")
                      != std::string::npos,
                  "FLX10 Beat FX channel selector follows CH1, CH2 and Master notes");
    return ok ? 0 : 1;
}
