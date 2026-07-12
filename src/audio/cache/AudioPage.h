#pragma once

#include <cstdint>
#include <vector>

struct AudioPage final {
    static constexpr std::int64_t kSamplesPerChannel = 16384;

    std::uint64_t trackId = 0;
    std::uint64_t generation = 0;
    std::int64_t pageIndex = 0;
    std::int64_t firstSample = 0;
    std::uint32_t validSampleCount = 0;
    std::uint16_t channelCount = 0;
    std::vector<float> planarPcm;

    [[nodiscard]] const float* channelData(unsigned channel) const noexcept
    {
        return channel < channelCount
            ? planarPcm.data() + static_cast<size_t>(channel) * kSamplesPerChannel : nullptr;
    }

    [[nodiscard]] std::uint64_t byteSize() const noexcept
    {
        return static_cast<std::uint64_t>(planarPcm.size() * sizeof(float));
    }

    [[nodiscard]] static constexpr std::int64_t pageIndexForSample(std::int64_t sample) noexcept
    {
        return sample < 0 ? -1 : sample / kSamplesPerChannel;
    }

    [[nodiscard]] static constexpr std::int64_t firstSampleForPage(std::int64_t page) noexcept
    {
        return page < 0 ? -1 : page * kSamplesPerChannel;
    }
};
