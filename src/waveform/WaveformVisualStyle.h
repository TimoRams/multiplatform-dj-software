#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace waveform_visual {

// This is deliberately a rendering-only choice.  TrackData and the waveform
// cache keep neutral geometry/RMS/bass/mid/treble values and never need to be
// revisited when the user changes this setting.
enum class WaveformRenderStyle : std::uint8_t {
    SpectralRgb = 0,
    ThreeBand = 1,
};

inline constexpr WaveformRenderStyle kDefaultRenderStyle
    = WaveformRenderStyle::SpectralRgb;

// Bump this when the visual interpretation for an existing style changes.
// The style value is deliberately part of the revision, so a cached RGB tile
// can coexist with its 3-band counterpart and is reused on a switch back.
inline constexpr std::uint32_t kStyleAlgorithmRevision = 2;

[[nodiscard]] constexpr WaveformRenderStyle normalizeStyle(int value) noexcept
{
    return value == static_cast<int>(WaveformRenderStyle::ThreeBand)
        ? WaveformRenderStyle::ThreeBand : kDefaultRenderStyle;
}

[[nodiscard]] constexpr std::uint32_t revision(
    WaveformRenderStyle style) noexcept
{
    return (kStyleAlgorithmRevision << 8U)
        | static_cast<std::uint32_t>(style);
}

struct BandEnergy final {
    float bass = 0.0f;
    float mid = 0.0f;
    float treble = 0.0f;
    float rms = 0.0f;
};

using Rgb8 = std::array<std::uint8_t, 3>;

// A component occupies a fixed range of the waveform silhouette.  This makes
// ThreeBand a real three-component representation, instead of a recoloured
// RGB stroke.  The opacity comes directly from the shared-scale band energy;
// it is never independently normalised per frequency band.
struct VisualComponent final {
    Rgb8 color{};
    float begin = 0.0f;
    float end = 1.0f;
    float opacity = 0.0f;
};

struct WaveformVisual final {
    Rgb8 geometryColor{};
    float geometryOpacity = 0.0f;
    std::array<VisualComponent, 3> components{};
    std::uint8_t componentCount = 0;
};

inline Rgb8 color(const BandEnergy& energy) noexcept
{
    const float wBass = std::pow(std::clamp(energy.bass, 0.0f, 1.0f), 2.5f);
    const float wMid = std::pow(std::clamp(energy.mid, 0.0f, 1.0f), 2.2f);
    const float wTreble = std::pow(std::clamp(energy.treble, 0.0f, 1.0f), 1.6f);
    const float sum = wBass + wMid + wTreble;
    if (sum <= 1.0e-7f)
        return {150, 170, 190};

    const float brightness = 0.58f + 0.42f
        * std::pow(std::clamp(energy.rms, 0.0f, 1.0f), 0.35f);
    const float red = ((wBass * 255.0f + wMid * 210.0f) / sum)
        * brightness;
    const float green = ((wBass * 35.0f + wMid * 255.0f
                          + wTreble * 185.0f) / sum) * brightness;
    const float blue = ((wBass * 20.0f + wTreble * 255.0f) / sum) * brightness;
    return {
        static_cast<std::uint8_t>(std::lround(std::clamp(red, 0.0f, 255.0f))),
        static_cast<std::uint8_t>(std::lround(std::clamp(green, 0.0f, 255.0f))),
        static_cast<std::uint8_t>(std::lround(std::clamp(blue, 0.0f, 255.0f)))
    };
}

[[nodiscard]] inline WaveformVisual map(
    WaveformRenderStyle style, const BandEnergy& energy) noexcept
{
    const float bass = std::clamp(energy.bass, 0.0f, 1.0f);
    const float mid = std::clamp(energy.mid, 0.0f, 1.0f);
    const float treble = std::clamp(energy.treble, 0.0f, 1.0f);
    const float rms = std::clamp(energy.rms, 0.0f, 1.0f);

    if (style == WaveformRenderStyle::SpectralRgb) {
        WaveformVisual visual;
        visual.geometryColor = color({bass, mid, treble, rms});
        visual.geometryOpacity = 0.0f;
        visual.components[0] = {visual.geometryColor, 0.0f, 1.0f,
                                248.0f / 255.0f};
        visual.componentCount = 1;
        return visual;
    }

    // The quiet neutral silhouette carries the min/max geometry.  Each band
    // then occupies its own stable vertical lane: high, mid, bass.  A quiet
    // high-frequency contribution remains quiet/transparent rather than being
    // stretched to a saturated blue lane.
    WaveformVisual visual;
    visual.geometryColor = {132, 142, 154};
    visual.geometryOpacity = 0.16f + 0.24f * rms;
    visual.components[0] = {{67, 145, 255}, 0.0f, 1.0f / 3.0f, treble};
    visual.components[1] = {{70, 218, 112}, 1.0f / 3.0f, 2.0f / 3.0f, mid};
    visual.components[2] = {{244, 75, 72}, 2.0f / 3.0f, 1.0f, bass};
    visual.componentCount = 3;
    return visual;
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
