#include "waveform/WaveformLineStore.h"
#include "waveform/WaveformVisualStyle.h"

#include <algorithm>
#include <cmath>
#include <iostream>

int main()
{
    WaveformLineStore store;
    store.reset(1, 4096, 300, 4096);
    auto lines = std::make_shared<std::vector<WaveformLine>>(4096);
    (*lines)[10] = {.minimum = -300, .maximum = 700, .red = 40, .green = 160, .blue = 255, .flags = 1};
    if (store.publish({1, 0, 0, 4096, 4096, lines}) != WaveformLineStore::PublishResult::Accepted) {
        std::cerr << "FAIL: renderer fixture unavailable\n";
        return 1;
    }
    // Overview aggregation reads the exact same immutable chunk as scrolling.
    const auto chunk = store.snapshot()->chunkAt(0);
    const auto& line = (*chunk->lines)[10];
    const auto sharedColor = waveform_visual::color({0.9f, 0.0f, 0.0f, 0.0f, 0.75f});
    if (sharedColor[0] <= sharedColor[1] || sharedColor[0] <= sharedColor[2]) {
        std::cerr << "FAIL: shared visual style lost low-frequency colour identity\n";
        return 1;
    }
    if (std::abs(waveform_visual::verticalPixelCoverage(4.25, 9.75, 4)
                 - 0.75f) > 1.0e-6f
        || waveform_visual::verticalPixelCoverage(4.25, 9.75, 5) != 1.0f
        || std::abs(waveform_visual::verticalPixelCoverage(4.25, 9.75, 9)
                    - 0.75f) > 1.0e-6f) {
        std::cerr << "FAIL: subpixel envelope coverage is not symmetric\n";
        return 1;
    }
    const auto min = std::min_element(chunk->lines->cbegin(), chunk->lines->cend(),
        [](const auto& a, const auto& b) { return a.minimum < b.minimum; });
    const auto max = std::max_element(chunk->lines->cbegin(), chunk->lines->cend(),
        [](const auto& a, const auto& b) { return a.maximum < b.maximum; });
    if (line.minimum != -300 || line.maximum != 700 || min->minimum != -300 || max->maximum != 700) {
        std::cerr << "FAIL: canonical line aggregation changed geometry\n";
        return 1;
    }
    return 0;
}
