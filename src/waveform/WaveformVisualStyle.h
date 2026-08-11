#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace waveform_visual {

struct BandEnergy final {
    float low = 0.0f;
    float lowMid = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
    float rms = 0.0f;
};

using Rgb8 = std::array<std::uint8_t, 3>;

inline Rgb8 color(const BandEnergy& energy) noexcept
{
    const float wLow = std::pow(std::clamp(energy.low, 0.0f, 1.0f), 2.8f);
    const float wLowMid = std::pow(std::clamp(energy.lowMid, 0.0f, 1.0f), 2.5f);
    const float wMid = std::pow(std::clamp(energy.mid, 0.0f, 1.0f), 2.2f);
    const float wHigh = std::pow(std::clamp(energy.high, 0.0f, 1.0f), 1.6f);
    const float sum = wLow + wLowMid + wMid + wHigh;
    if (sum <= 1.0e-7f)
        return {150, 170, 190};

    const float brightness = 0.58f + 0.42f
        * std::pow(std::clamp(energy.rms, 0.0f, 1.0f), 0.35f);
    const float red = ((wLow * 255.0f + wLowMid * 255.0f + wMid * 210.0f) / sum)
        * brightness;
    const float green = ((wLow * 20.0f + wLowMid * 130.0f + wMid * 255.0f
                          + wHigh * 185.0f) / sum) * brightness;
    const float blue = ((wLow * 20.0f + wHigh * 255.0f) / sum) * brightness;
    return {
        static_cast<std::uint8_t>(std::lround(std::clamp(red, 0.0f, 255.0f))),
        static_cast<std::uint8_t>(std::lround(std::clamp(green, 0.0f, 255.0f))),
        static_cast<std::uint8_t>(std::lround(std::clamp(blue, 0.0f, 255.0f)))
    };
}

inline float foldedEnergy(float meanAmplitude, float peakAmplitude) noexcept
{
    return std::clamp(meanAmplitude * 0.68f + peakAmplitude * 0.32f,
                      0.0f, 1.0f);
}

inline float logarithmicAmplitude(float energy) noexcept
{
    return std::log1p(std::clamp(energy, 0.0f, 1.0f) * 10.0f)
        / std::log1p(10.0f);
}

inline float verticalPixelCoverage(double top,
                                   double bottom,
                                   int physicalPixelY) noexcept
{
    if (!std::isfinite(top) || !std::isfinite(bottom) || bottom <= top)
        return 0.0f;
    const double pixelTop = static_cast<double>(physicalPixelY);
    const double covered = std::min(bottom, pixelTop + 1.0)
        - std::max(top, pixelTop);
    return static_cast<float>(std::clamp(covered, 0.0, 1.0));
}

} // namespace waveform_visual
