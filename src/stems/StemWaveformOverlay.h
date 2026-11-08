#pragma once

#include "stems/StemTrackData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace brockdj::stems {

// Immutable per-frame view the waveform renderer uses to grey out muted stems.
// Available at unity too, so renderers prepare energy data before the first mute.
struct StemWaveformOverlay final {
    std::shared_ptr<const StemTrackData> data;
    std::array<float, kStemCount> gains { 1.0f, 1.0f, 1.0f, 1.0f };
    // Non-zero; changes with the gains or newly available envelope blocks so
    // cached waveform tiles are re-rasterised exactly when their look changes.
    std::uint64_t revision = 0;
};

class StemWaveformTransition final {
public:
    using Gains = std::array<float, kStemCount>;
    static constexpr double kDurationMs = 180.0;

    [[nodiscard]] Gains value(double nowMs) const noexcept
    {
        const float t = static_cast<float>(std::clamp((nowMs - m_startedMs) / kDurationMs, 0.0, 1.0));
        const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        Gains gains {};
        for (std::size_t i = 0; i < gains.size(); ++i)
            gains[i] = m_from[i] + (m_target[i] - m_from[i]) * eased;
        return gains;
    }

    void retarget(const Gains& target, double nowMs) noexcept
    {
        if (target == m_target)
            return;
        m_from = value(nowMs);
        m_target = target;
        m_startedMs = nowMs;
    }

    [[nodiscard]] bool active(double nowMs) const noexcept
    {
        return m_from != m_target && nowMs < m_startedMs + kDurationMs;
    }

private:
    Gains m_from {1.0f, 1.0f, 1.0f, 1.0f};
    Gains m_target = m_from;
    double m_startedMs = 0.0;
};

[[nodiscard]] inline std::uint64_t stemWaveformOverlayRevision(
    std::uint64_t trackId, const std::array<float, kStemCount>& gains, std::uint64_t envelopeEpoch) noexcept
{
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6U) + (hash >> 2U);
    };
    mix(trackId);
    for (float gain : gains)
        mix(static_cast<std::uint64_t>(std::lround(std::clamp(gain, 0.0f, 2.0f) * 1000.0f)));
    mix(envelopeEpoch);
    return hash == 0 ? 1 : hash;
}

// Fraction of the mix amplitude that is still audible, from per-stem mean
// energies. Stems are treated as uncorrelated, which matches how they render
// closely enough for display.
[[nodiscard]] inline float stemAudibleAmplitudeRatio(const std::array<float, kStemCount>& energy,
                                                     const std::array<float, kStemCount>& gains) noexcept
{
    float total = 0.0f;
    float audible = 0.0f;
    for (std::size_t i = 0; i < energy.size(); ++i) {
        total += energy[i];
        audible += gains[i] * gains[i] * energy[i];
    }
    if (!(total > 1e-12f))
        return 1.0f;
    return std::clamp(std::sqrt(audible / total), 0.0f, 1.0f);
}

} // namespace brockdj::stems
