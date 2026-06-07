#pragma once

namespace engine::audio {

inline float cubicHermite(float y0, float y1, float y2, float y3, float t) noexcept
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float m1 = 0.5f * (y2 - y0);
    const float m2 = 0.5f * (y3 - y1);
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * y1
         + (t3 - 2.0f * t2 + t) * m1
         + (-2.0f * t3 + 3.0f * t2) * y2
         + (t3 - t2) * m2;
}

} // namespace engine::audio
