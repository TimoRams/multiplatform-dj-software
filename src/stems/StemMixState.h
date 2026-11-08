#pragma once

#include "stems/StemTrackData.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace brockdj::stems {

// out = gM·mix + (gV−gM)·vocals + (gB−gM)·bass + (gD−gM)·drums
// With melody = mix − vocals − bass − drums this is exactly Σ gain·stem, and
// all-unity gains return the mix bit-for-bit.
[[nodiscard]] inline float remixSample(float mix, float vocals, float bass, float drums,
                                       const std::array<float, kStemCount>& gains) noexcept
{
    const float melodyGain = gains[static_cast<int>(Stem::Melody)];
    return melodyGain * mix
        + (gains[static_cast<int>(Stem::Vocals)] - melodyGain) * vocals
        + (gains[static_cast<int>(Stem::Bass)] - melodyGain) * bass
        + (gains[static_cast<int>(Stem::Drums)] - melodyGain) * drums;
}

[[nodiscard]] inline bool isUnity(const std::array<float, kStemCount>& gains) noexcept
{
    for (float gain : gains)
        if (gain != 1.0f)
            return false;
    return true;
}

// Per-deck stem state shared by the control thread, the separation worker and
// the audio thread. The audio thread never touches shared_ptr reference counts:
// it registers as a reader, loads a raw pointer, and the publisher waits for
// readers to drain before releasing a retired track.
class StemMixState final
{
public:
    StemMixState()
    {
        for (auto& gain : m_targetGains)
            gain.store(1.0f, std::memory_order_relaxed);
    }

    void publish(std::shared_ptr<StemTrackData> data)
    {
        std::shared_ptr<StemTrackData> retired;
        {
            std::lock_guard lock(m_publishMutex);
            retired = std::move(m_owner);
            m_owner = std::move(data);
            m_current.store(m_owner.get(), std::memory_order_seq_cst);
            while (m_readers.load(std::memory_order_seq_cst) != 0)
                std::this_thread::yield();
        }
        m_epoch.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] std::shared_ptr<StemTrackData> current() const
    {
        std::lock_guard lock(m_publishMutex);
        return m_owner;
    }

    void setGain(Stem stem, float gain) noexcept
    {
        m_targetGains[static_cast<int>(stem)].store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
        m_epoch.fetch_add(1, std::memory_order_release);
    }
    [[nodiscard]] float gain(Stem stem) const noexcept
    {
        return m_targetGains[static_cast<int>(stem)].load(std::memory_order_relaxed);
    }
    void resetGains() noexcept
    {
        for (auto& gain : m_targetGains)
            gain.store(1.0f, std::memory_order_relaxed);
        m_epoch.fetch_add(1, std::memory_order_release);
    }
    [[nodiscard]] std::array<float, kStemCount> targetGains() const noexcept
    {
        std::array<float, kStemCount> gains {};
        for (int i = 0; i < kStemCount; ++i)
            gains[static_cast<std::size_t>(i)] = m_targetGains[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        return gains;
    }
    // Changes on any gain edit or track publication.
    [[nodiscard]] std::uint64_t epoch() const noexcept { return m_epoch.load(std::memory_order_acquire); }

private:
    friend class StemReadScope;
    mutable std::mutex m_publishMutex;
    std::shared_ptr<StemTrackData> m_owner;
    std::atomic<StemTrackData*> m_current { nullptr };
    std::atomic<std::uint32_t> m_readers { 0 };
    std::array<std::atomic<float>, kStemCount> m_targetGains;
    std::atomic<std::uint64_t> m_epoch { 0 };
};

// Consumer-owned gain smoother: ramps towards the target gains so stem mutes
// fade over ~15 ms instead of clicking.
struct StemGainSmoother {
    std::array<float, kStemCount> gains { 1.0f, 1.0f, 1.0f, 1.0f };
    float step = 1.0f / 660.0f;
    float availability = 0.0f;

    void prepare(double sampleRate) noexcept
    {
        step = static_cast<float>(1.0 / std::max(1.0, sampleRate * 0.015));
    }
    void snap(const std::array<float, kStemCount>& targets) noexcept { gains = targets; }
    void advance(const std::array<float, kStemCount>& targets) noexcept
    {
        for (std::size_t i = 0; i < gains.size(); ++i)
            gains[i] += std::clamp(targets[i] - gains[i], -step, step);
    }
    void advanceAvailability(bool ready) noexcept
    {
        availability = std::clamp(availability + (ready ? step : -step), 0.0f, 1.0f);
    }
};

// Realtime, lock-free view used for one audio block.
class StemReadScope final
{
public:
    StemReadScope(StemMixState* state, StemGainSmoother& smoother, std::uint64_t trackId) noexcept
        : m_state(state), m_smoother(smoother)
    {
        if (!m_state)
            return;
        m_state->m_readers.fetch_add(1, std::memory_order_seq_cst);
        m_data = m_state->m_current.load(std::memory_order_seq_cst);
        if (m_data && m_data->trackId() != trackId)
            m_data = nullptr;
        if (m_data) {
            m_allReady = m_data->complete();
            m_edgeFadeSamples = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(m_data->sampleRate() * 0.015), 1, kReadyBlockSamples);
        }
        m_targets = m_state->targetGains();
        if (!m_data) {
            m_smoother.snap(m_targets);
            m_smoother.availability = 0.0f;
        }
        m_active = m_data && !(isUnity(m_targets) && isUnity(m_smoother.gains));
    }
    ~StemReadScope()
    {
        if (m_state)
            m_state->m_readers.fetch_sub(1, std::memory_order_release);
    }
    StemReadScope(const StemReadScope&) = delete;
    StemReadScope& operator=(const StemReadScope&) = delete;

    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] const StemTrackData* data() const noexcept { return m_data; }

    // Once per output frame while active.
    void advance(std::int64_t sample) noexcept
    {
        m_smoother.advance(m_targets);
        m_smoother.advanceAvailability(m_data && m_data->isReady(sample));
    }
    // Scratch windows are copied in bulk; they use the target gains directly.
    void snapToTargets() noexcept
    {
        m_smoother.snap(m_targets);
        m_smoother.availability = 1.0f;
    }

    [[nodiscard]] float apply(std::int64_t sample, int channel, float mix) const noexcept
    {
        if (!m_active || !m_data->isReady(sample))
            return mix;
        const int ch = channel > 0 ? 1 : 0;
        float weight = m_smoother.availability;
        if (!m_allReady) {
            const auto blockStart = (sample / kReadyBlockSamples) * kReadyBlockSamples;
            const auto blockEnd = std::min(blockStart + kReadyBlockSamples, m_data->length());
            if (sample - blockStart < m_edgeFadeSamples && blockStart > 0
                && !m_data->isReady(blockStart - 1)) {
                weight = std::min(weight, static_cast<float>(sample - blockStart)
                                          / static_cast<float>(m_edgeFadeSamples));
            }
            if (blockEnd - sample <= m_edgeFadeSamples && blockEnd < m_data->length()
                && !m_data->isReady(blockEnd)) {
                weight = std::min(weight, static_cast<float>(blockEnd - sample)
                                          / static_cast<float>(m_edgeFadeSamples));
            }
        }
        const float separated = remixSample(mix,
                                             m_data->stored(StoredStem::Vocals, ch, sample),
                                             m_data->stored(StoredStem::Bass, ch, sample),
                                             m_data->stored(StoredStem::Drums, ch, sample),
                                             m_smoother.gains);
        return std::lerp(mix, separated, weight);
    }

private:
    StemMixState* m_state = nullptr;
    StemGainSmoother& m_smoother;
    const StemTrackData* m_data = nullptr;
    std::array<float, kStemCount> m_targets {};
    std::int64_t m_edgeFadeSamples = 1;
    bool m_allReady = false;
    bool m_active = false;
};

} // namespace brockdj::stems
