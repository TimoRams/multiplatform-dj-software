#include <algorithm>
#include <array>
#include <iostream>

namespace {
struct LayoutResult { int waveform; int deck; int library; int mixer; };
LayoutResult layout(int width, int height, double scale, bool fourDeck, bool libraryVisible, bool mixerVisible)
{
    const int toolbar = static_cast<int>(30 * scale);
    const int deck = static_cast<int>(244 * (static_cast<double>(width) / 1600.0) * scale);
    const int rows = fourDeck ? 2 : 1;
    const int library = libraryVisible ? static_cast<int>(180 * scale) : 0;
    const int waveform = std::max(0, height - toolbar - rows * deck - library - 8);
    const int mixer = mixerVisible ? static_cast<int>(308 * scale) : 0;
    return {waveform, deck, library, mixer};
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
    constexpr std::array sizes {std::pair{1024, 600}, std::pair{1280, 720}, std::pair{1280, 800},
        std::pair{1366, 768}, std::pair{1440, 900}, std::pair{1920, 1080},
        std::pair{2560, 1440}, std::pair{3840, 2160}};
    for (const auto [width, height] : sizes)
        for (const double scale : {0.8, 1.0, 1.4})
            for (const bool fourDeck : {false, true})
                for (const bool library : {false, true})
                    for (const bool mixer : {false, true}) {
                        const auto value = layout(width, height, scale, fourDeck, library, mixer);
                        ok &= require(value.waveform >= 0 && value.deck >= 0 && value.library >= 0 && value.mixer >= 0,
                                      "layout metrics must never become negative");
                        ok &= require(value.mixer <= static_cast<int>(width * 0.5),
                                      "mixer must not consume more than half the viewport");
                    }
    return ok ? 0 : 1;
}
