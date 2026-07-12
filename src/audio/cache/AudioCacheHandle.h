#pragma once

#include <cstdint>

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
