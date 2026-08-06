#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::audio {

enum class SampleEdgeMode { Clamp, Mirror };

inline float sampleAtClamp(std::span<const float> buffer, int index) noexcept
{
    if (buffer.empty())
        return 0.0f;
    if (index < 0)
        index = 0;
    else if (index >= static_cast<int>(buffer.size()))
        index = static_cast<int>(buffer.size()) - 1;
    return buffer[static_cast<size_t>(index)];
}

inline float sampleAtMirror(std::span<const float> buffer, int index) noexcept
{
    if (buffer.empty())
        return 0.0f;
    const int len = static_cast<int>(buffer.size());
    if (index < 0)
        index = -index;
    if (index >= len)
        index = (len - 1) - (index - (len - 1));
    if (index < 0)
        index = 0;
    if (index >= len)
        index = len - 1;
    return buffer[static_cast<size_t>(index)];
}

inline float sampleAt(std::span<const float> buffer, int index, SampleEdgeMode mode) noexcept
{
    return mode == SampleEdgeMode::Mirror
        ? sampleAtMirror(buffer, index)
        : sampleAtClamp(buffer, index);
}

inline float cubicHermite(float y0, float y1, float y2, float y3, float t) noexcept
{
    const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c = -0.5f * y0 + 0.5f * y2;
    const float d = y1;
    return ((a * t + b) * t + c) * t + d;
}

inline float readHermite(std::span<const float> buffer, double position, SampleEdgeMode mode) noexcept
{
    if (buffer.empty())
        return 0.0f;

    const int base = static_cast<int>(std::floor(position));
    const float t = static_cast<float>(position - static_cast<double>(base));
    const float y0 = sampleAt(buffer, base - 1, mode);
    const float y1 = sampleAt(buffer, base, mode);
    const float y2 = sampleAt(buffer, base + 1, mode);
    const float y3 = sampleAt(buffer, base + 2, mode);
    return cubicHermite(y0, y1, y2, y3, t);
}

} // namespace engine::audio
