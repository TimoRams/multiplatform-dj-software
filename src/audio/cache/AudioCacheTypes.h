#pragma once

#include <cstdint>
#include <vector>

class AudioPageCache;

class AudioCacheHandle final
{
public:
    [[nodiscard]] bool isValid() const noexcept { return m_token && m_trackId != 0; }
    [[nodiscard]] std::uint64_t id() const noexcept { return m_trackId; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] std::int64_t lengthInSamples() const noexcept { return m_lengthInSamples; }
    [[nodiscard]] int channelCount() const noexcept { return m_channelCount; }
    [[nodiscard]] std::int64_t pageCount() const noexcept
    {
        return m_lengthInSamples <= 0 ? 0
            : (m_lengthInSamples + 16383) / 16384;
    }

private:
    friend class AudioPageCache;
    const void* m_token = nullptr;
    std::uint64_t m_trackId = 0;
    std::uint64_t m_generation = 0;
    double m_sampleRate = 0.0;
    std::int64_t m_lengthInSamples = 0;
    int m_channelCount = 0;
};

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
