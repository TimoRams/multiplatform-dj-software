#include "waveform/WaveformLineStore.h"

#include <algorithm>
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
