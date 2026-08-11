#pragma once

#include "WaveformLine.h"

#include <memory>
#include <cstdint>
#include <vector>

struct WaveformLineBlock final {
    int firstLine = 0;
    std::shared_ptr<const std::vector<WaveformLine>> lines;
};

using WaveformLineBatch = std::vector<WaveformLineBlock>;

struct WaveformLodBlock final {
    int level = 0;
    int canonicalLineStride = 1;
    int firstSample = 0;
    int totalSamples = 0;
    std::shared_ptr<const std::vector<WaveformLine>> lines;
};

using WaveformLodBatch = std::vector<WaveformLodBlock>;
