#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace brockdj::stems {

// Serato-style stem set. Melody is not stored: it is the mix minus the three
// stored stems, so all stems always sum back to the original mix exactly.
enum class Stem : int { Vocals = 0, Melody = 1, Bass = 2, Drums = 3 };
inline constexpr int kStemCount = 4;

enum class StoredStem : int { Vocals = 0, Bass = 1, Drums = 2 };
inline constexpr int kStoredStemCount = 3;
inline constexpr int kStemChannels = 2;

// Readiness is tracked per block so the audio thread can fall back to the
// untouched mix wherever separation has not finished yet.
inline constexpr std::int64_t kReadyBlockSamples = 4096;
// Two times headroom over full scale keeps hot masters from clipping.
inline constexpr float kStorageScale = 16384.0f;
// Waveform envelope resolution: one RMS value per stem every 64 samples
// (~1.5 ms at 44.1 kHz), finer than a waveform column at any DJ zoom level.
inline constexpr std::int64_t kEnvelopeBinSamples = 64;
inline constexpr std::int64_t kEnvelopeBinsPerBlock = kReadyBlockSamples / kEnvelopeBinSamples;
inline constexpr float kEnvelopeScale = 16384.0f;
static_assert(kReadyBlockSamples % kEnvelopeBinSamples == 0);

enum class StemJobState : int { Idle = 0, Queued, Separating, Ready, Failed };

// Control-thread view of a separation job (scheduling hints in, status out).
struct StemJobStatus {
    std::atomic<std::int64_t> playheadSample { 0 };
    std::atomic<bool> playing { false };
    std::atomic<StemJobState> state { StemJobState::Queued };
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> fromCache { false };
    std::atomic<bool> cacheStored { false };

    void setError(std::string error)
    {
        std::lock_guard lock(m_errorMutex);
        m_error = std::move(error);
    }

    [[nodiscard]] std::string error() const
    {
        std::lock_guard lock(m_errorMutex);
        return m_error;
    }

private:
    mutable std::mutex m_errorMutex;
    std::string m_error;
};

class StemTrackData final
{
public:
    StemTrackData(std::uint64_t trackId, double sampleRate, std::int64_t lengthSamples)
        : m_trackId(trackId)
        , m_sampleRate(sampleRate)
        , m_length(std::max<std::int64_t>(0, lengthSamples))
        , m_samples(static_cast<std::size_t>(m_length) * kStoredStemCount * kStemChannels, 0)
        , m_blockCount(static_cast<std::size_t>((m_length + kReadyBlockSamples - 1) / kReadyBlockSamples))
        , m_readyCounts(std::make_unique<std::atomic<std::uint32_t>[]>(m_blockCount))
        , m_envelopeBinCount(static_cast<std::size_t>((m_length + kEnvelopeBinSamples - 1) / kEnvelopeBinSamples))
        , m_envelope(m_envelopeBinCount * kStemCount, 0)
        , m_envelopeReady(std::make_unique<std::atomic<std::uint8_t>[]>(m_blockCount))
    {
        for (std::size_t i = 0; i < m_blockCount; ++i) {
            m_readyCounts[i].store(0, std::memory_order_relaxed);
            m_envelopeReady[i].store(0, std::memory_order_relaxed);
        }
    }

    StemTrackData(const StemTrackData&) = delete;
    StemTrackData& operator=(const StemTrackData&) = delete;

    [[nodiscard]] std::uint64_t trackId() const noexcept { return m_trackId; }
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] std::int64_t length() const noexcept { return m_length; }

    // ---- realtime readers ----------------------------------------------
    [[nodiscard]] bool isReady(std::int64_t sample) const noexcept
    {
        if (sample < 0 || sample >= m_length)
            return false;
        const auto block = static_cast<std::size_t>(sample / kReadyBlockSamples);
        return m_readyCounts[block].load(std::memory_order_acquire) == blockLength(block);
    }

    [[nodiscard]] float stored(StoredStem stem, int channel, std::int64_t sample) const noexcept
    {
        return static_cast<float>(m_samples[index(stem, channel, sample)]) * (1.0f / kStorageScale);
    }

    // ---- writer (single separation worker) -------------------------------
    // Each sample range must be finalised exactly once; readiness counts the
    // finalised samples per block, so a block becomes visible only when every
    // one of its samples has its final value.
    void write(StoredStem stem, int channel, std::int64_t sample, float value) noexcept
    {
        if (sample < 0 || sample >= m_length)
            return;
        const float scaled = std::clamp(value * kStorageScale, -32767.0f, 32767.0f);
        m_samples[index(stem, channel, sample)] = static_cast<std::int16_t>(std::lrint(scaled));
    }

    void markFinal(std::int64_t first, std::int64_t endExclusive) noexcept
    {
        first = std::max<std::int64_t>(0, first);
        endExclusive = std::min(endExclusive, m_length);
        while (first < endExclusive) {
            const auto block = first / kReadyBlockSamples;
            const auto blockEnd = std::min(endExclusive, (block + 1) * kReadyBlockSamples);
            m_readyCounts[static_cast<std::size_t>(block)].fetch_add(
                static_cast<std::uint32_t>(blockEnd - first), std::memory_order_release);
            m_finalSamples.fetch_add(blockEnd - first, std::memory_order_release);
            first = blockEnd;
        }
        m_readyEpoch.fetch_add(1, std::memory_order_release);
    }

    void markAllFinal() noexcept { markFinal(0, m_length); }

    [[nodiscard]] double readyFraction() const noexcept
    {
        return m_length > 0
            ? static_cast<double>(m_finalSamples.load(std::memory_order_relaxed)) / static_cast<double>(m_length)
            : 0.0;
    }
    [[nodiscard]] bool complete() const noexcept
    {
        return m_finalSamples.load(std::memory_order_acquire) >= m_length;
    }
    // Bumped whenever new audio becomes ready; lets windowed consumers (the
    // scratch source window) know their cached copy is stale.
    [[nodiscard]] std::uint64_t readyEpoch() const noexcept
    {
        return m_readyEpoch.load(std::memory_order_acquire);
    }

    // ---- waveform envelope ---------------------------------------------
    // Written by the separation worker, read by waveform rasterizers. A block's
    // bins are published together, independently of audio readiness, because a
    // cache load makes the audio ready before the mix has been re-analysed.
    //
    // Squared amplitudes (mean of both channels) for one sample. Every sample
    // must be accumulated exactly once.
    void accumulateEnvelope(std::int64_t sample, const std::array<float, kStemCount>& squares)
    {
        if (sample < 0 || sample >= m_length)
            return;
        if (m_envelopeSums.empty()) {
            m_envelopeSums.assign(m_envelopeBinCount * kStemCount, 0.0f);
            m_envelopeFill.assign(m_envelopeBinCount, 0);
            m_envelopeBlockBins.assign(m_blockCount, 0);
        }
        const auto bin = static_cast<std::size_t>(sample / kEnvelopeBinSamples);
        for (std::size_t stem = 0; stem < kStemCount; ++stem)
            m_envelopeSums[bin * kStemCount + stem] += squares[stem];
        if (++m_envelopeFill[bin] < envelopeBinLength(bin))
            return;
        const float length = static_cast<float>(envelopeBinLength(bin));
        for (std::size_t stem = 0; stem < kStemCount; ++stem) {
            const float rms = std::sqrt(std::max(0.0f, m_envelopeSums[bin * kStemCount + stem]) / length);
            m_envelope[bin * kStemCount + stem]
                = static_cast<std::uint16_t>(std::clamp(std::lrint(rms * kEnvelopeScale), 0L, 65535L));
        }
        const auto block = bin / static_cast<std::size_t>(kEnvelopeBinsPerBlock);
        if (++m_envelopeBlockBins[block] < envelopeBinsInBlock(block))
            return;
        m_envelopeReady[block].store(1, std::memory_order_release);
        if (m_envelopeBlocksDone.fetch_add(1, std::memory_order_acq_rel) + 1 == m_blockCount) {
            m_envelopeSums = {};
            m_envelopeFill = {};
            m_envelopeBlockBins = {};
        }
        m_envelopeEpoch.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] bool envelopeReady(std::int64_t sample) const noexcept
    {
        if (sample < 0 || sample >= m_length)
            return false;
        return m_envelopeReady[static_cast<std::size_t>(sample / kReadyBlockSamples)]
                   .load(std::memory_order_acquire) != 0;
    }
    [[nodiscard]] bool envelopeComplete() const noexcept
    {
        return m_envelopeBlocksDone.load(std::memory_order_acquire) >= m_blockCount;
    }
    [[nodiscard]] std::uint64_t envelopeEpoch() const noexcept
    {
        return m_envelopeEpoch.load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::vector<std::uint16_t>& rawEnvelope() const noexcept { return m_envelope; }
    [[nodiscard]] std::vector<std::uint16_t>& rawEnvelopeForLoad() noexcept { return m_envelope; }
    void markAllEnvelopeFinal() noexcept
    {
        for (std::size_t block = 0; block < m_blockCount; ++block)
            m_envelopeReady[block].store(1, std::memory_order_release);
        m_envelopeBlocksDone.store(m_blockCount, std::memory_order_release);
        m_envelopeEpoch.fetch_add(1, std::memory_order_release);
    }

    // Mean energy per stem (Stem order) over [first, end). False when any part
    // of the range has no envelope yet; that audio still plays as the mix.
    [[nodiscard]] bool envelopeEnergy(std::int64_t first, std::int64_t end,
                                      std::array<float, kStemCount>& energy) const noexcept
    {
        first = std::max<std::int64_t>(0, first);
        end = std::min(end, m_length);
        if (end <= first)
            return false;
        const auto firstBin = static_cast<std::size_t>(first / kEnvelopeBinSamples);
        const auto lastBin = static_cast<std::size_t>((end - 1) / kEnvelopeBinSamples);
        constexpr auto binsPerBlock = static_cast<std::size_t>(kEnvelopeBinsPerBlock);
        for (auto block = firstBin / binsPerBlock; block <= lastBin / binsPerBlock; ++block)
            if (m_envelopeReady[block].load(std::memory_order_acquire) == 0)
                return false;
        energy.fill(0.0f);
        for (auto bin = firstBin; bin <= lastBin; ++bin)
            for (std::size_t stem = 0; stem < kStemCount; ++stem) {
                const float rms = static_cast<float>(m_envelope[bin * kStemCount + stem]) * (1.0f / kEnvelopeScale);
                energy[stem] += rms * rms;
            }
        const float bins = static_cast<float>(lastBin - firstBin + 1);
        for (auto& value : energy)
            value /= bins;
        return true;
    }

    // Raw planar storage for the disk cache: [stem][channel][sample].
    [[nodiscard]] const std::vector<std::int16_t>& rawSamples() const noexcept { return m_samples; }
    [[nodiscard]] std::vector<std::int16_t>& rawSamplesForLoad() noexcept { return m_samples; }

private:
    [[nodiscard]] std::size_t index(StoredStem stem, int channel, std::int64_t sample) const noexcept
    {
        return (static_cast<std::size_t>(static_cast<int>(stem) * kStemChannels + channel)
                * static_cast<std::size_t>(m_length))
            + static_cast<std::size_t>(sample);
    }
    [[nodiscard]] std::uint32_t blockLength(std::size_t block) const noexcept
    {
        const auto start = static_cast<std::int64_t>(block) * kReadyBlockSamples;
        return static_cast<std::uint32_t>(std::min(kReadyBlockSamples, m_length - start));
    }
    [[nodiscard]] std::uint16_t envelopeBinLength(std::size_t bin) const noexcept
    {
        const auto start = static_cast<std::int64_t>(bin) * kEnvelopeBinSamples;
        return static_cast<std::uint16_t>(std::min(kEnvelopeBinSamples, m_length - start));
    }
    [[nodiscard]] std::uint8_t envelopeBinsInBlock(std::size_t block) const noexcept
    {
        const auto first = block * static_cast<std::size_t>(kEnvelopeBinsPerBlock);
        return static_cast<std::uint8_t>(std::min<std::size_t>(static_cast<std::size_t>(kEnvelopeBinsPerBlock),
                                                               m_envelopeBinCount - first));
    }

    const std::uint64_t m_trackId;
    const double m_sampleRate;
    const std::int64_t m_length;
    std::vector<std::int16_t> m_samples;
    const std::size_t m_blockCount;
    std::unique_ptr<std::atomic<std::uint32_t>[]> m_readyCounts;
    std::atomic<std::int64_t> m_finalSamples { 0 };
    std::atomic<std::uint64_t> m_readyEpoch { 0 };
    const std::size_t m_envelopeBinCount;
    std::vector<std::uint16_t> m_envelope; // [bin][Stem], RMS * kEnvelopeScale
    std::unique_ptr<std::atomic<std::uint8_t>[]> m_envelopeReady;
    std::atomic<std::size_t> m_envelopeBlocksDone { 0 };
    std::atomic<std::uint64_t> m_envelopeEpoch { 0 };
    // Worker-only accumulation state, released once the envelope is complete.
    std::vector<float> m_envelopeSums;
    std::vector<std::uint16_t> m_envelopeFill;
    std::vector<std::uint8_t> m_envelopeBlockBins;
};

} // namespace brockdj::stems
