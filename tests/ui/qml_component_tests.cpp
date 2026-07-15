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
    ok &= require(std::count(main.begin(), main.end(), '\n') < 600, "main.qml remains a compact shell");
    ok &= require(main.find("PerformanceWorkspace") != std::string::npos, "shell routes to performance workspace");
    ok &= require(workspace.find("DeckControl") != std::string::npos, "workspace uses shared deck component");
    ok &= require(workspace.find("MixerSection") != std::string::npos, "workspace uses shared mixer component");
    ok &= require(main.find("waveformZoomLevels") == std::string::npos, "legacy fixed zoom list removed");
    ok &= require(main.find("resizeThrottleCounter") == std::string::npos, "resize event counter removed");
    ok &= require(shortcuts.find("Ctrl+Shift+0") != std::string::npos
                  && shortcuts.find("Ctrl+0") != std::string::npos, "independent reset shortcuts exist");
    return ok ? 0 : 1;
}
