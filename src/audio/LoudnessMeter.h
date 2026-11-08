#pragma once

// ITU-R BS.1770-4 / EBU R128 loudness measurement, split in two halves:
//  - KWeightedBlockMeter runs on the audio thread. It K-weights the master and
//    emits the mean-square energy of every 100 ms block (no allocation, no locks).
//  - LoudnessAccumulator runs on the UI thread and turns those blocks into
//    momentary (400 ms), short-term (3 s) and gated integrated loudness.
// LoudnessBlockQueue is the single-producer/single-consumer hand-over.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace brockdj::audio {

inline constexpr double kLufsFloor = -70.0;   // absolute gate; also "silence" for display

[[nodiscard]] inline double energyToLufs(double energy) noexcept
{
    return energy > 0.0 ? std::max(kLufsFloor, -0.691 + 10.0 * std::log10(energy)) : kLufsFloor;
}

[[nodiscard]] inline double lufsToEnergy(double lufs) noexcept
{
    return std::pow(10.0, (lufs + 0.691) / 10.0);
}

class KWeightedBlockMeter final {
public:
    void prepare(double sampleRate) noexcept
    {
        const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        // Stage 1: high-shelf (+4 dB) modelling the head.
        {
            const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
            const double k = std::tan(std::numbers::pi * f0 / fs);
            const double vh = std::pow(10.0, gainDb / 20.0);
            const double vb = std::pow(vh, 0.4996667741545416);
            const double a0 = 1.0 + k / q + k * k;
            m_shelf = {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                       2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
        }
        // Stage 2: RLB high-pass.
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const double k = std::tan(std::numbers::pi * f0 / fs);
            const double a0 = 1.0 + k / q + k * k;
            m_highPass = {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
        }
        m_blockLength = std::max(1, static_cast<int>(std::lround(fs * 0.1)));
        reset();
    }

    void reset() noexcept
    {
        m_state = {};
        m_sum = 0.0;
        m_count = 0;
    }

    // `sink(double energy)` is called once per completed 100 ms block.
    template <typename Sink>
    void process(const float* left, const float* right, int samples, Sink&& sink) noexcept
    {
        if (!left)
            return;
        if (!right)
            right = left;
        for (int i = 0; i < samples; ++i) {
            const double l = filter(1, filter(0, left[i]));
            const double r = filter(3, filter(2, right[i]));
            m_sum += l * l + r * r;
            if (++m_count >= m_blockLength) {
                const double energy = m_sum / m_count;
                m_sum = 0.0;
                m_count = 0;
                sink(std::isfinite(energy) ? energy : 0.0);
            }
        }
    }

private:
    struct Coefficients { double b0, b1, b2, a1, a2; };
    struct State { double z1 = 0.0, z2 = 0.0; };

    // Even stage indices are the shelf, odd ones the high-pass; 0/1 left, 2/3 right.
    double filter(int stage, double x) noexcept
    {
        const Coefficients& c = (stage & 1) ? m_highPass : m_shelf;
        State& s = m_state[static_cast<std::size_t>(stage)];
        const double y = c.b0 * x + s.z1;
        s.z1 = c.b1 * x - c.a1 * y + s.z2;
        s.z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    Coefficients m_shelf {1, 0, 0, 0, 0};
    Coefficients m_highPass {1, 0, 0, 0, 0};
    std::array<State, 4> m_state {};
    double m_sum = 0.0;
    int m_count = 0;
    int m_blockLength = 4800;
};

class LoudnessBlockQueue final {
public:
    static constexpr std::size_t kCapacity = 256;   // 25.6 s of blocks

    bool push(float energy) noexcept
    {
        const auto head = m_head.load(std::memory_order_relaxed);
        const auto next = (head + 1) % kCapacity;
        if (next == m_tail.load(std::memory_order_acquire))
            return false;   // consumer stalled; dropping is better than blocking audio
        m_items[head] = energy;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    template <typename Consumer>
    int drain(Consumer&& consumer) noexcept
    {
        int count = 0;
        auto tail = m_tail.load(std::memory_order_relaxed);
        const auto head = m_head.load(std::memory_order_acquire);
        while (tail != head) {
            consumer(m_items[tail]);
            tail = (tail + 1) % kCapacity;
            ++count;
        }
        m_tail.store(tail, std::memory_order_release);
        return count;
    }

private:
    std::array<float, kCapacity> m_items {};
    std::atomic<std::size_t> m_head {0};
    std::atomic<std::size_t> m_tail {0};
};

class LoudnessAccumulator final {
public:
    void reset() noexcept
    {
        m_recent = {};
        m_recentCount = 0;
        m_recentPos = 0;
        m_bins = {};
        m_maxShortTerm = kLufsFloor;
    }

    void addBlock(double energy) noexcept
    {
        m_recent[m_recentPos] = std::max(0.0, energy);
        m_recentPos = (m_recentPos + 1) % m_recent.size();
        m_recentCount = std::min(m_recentCount + 1, m_recent.size());
        if (m_recentCount < 4)
            return;
        // Every 100 ms yields one 400 ms gating block (75 % overlap).
        const double gatingEnergy = meanOfLast(4);
        const double lufs = energyToLufs(gatingEnergy);
        if (lufs > kLufsFloor) {
            Bin& bin = m_bins[binIndex(lufs)];
            ++bin.count;
            bin.energy += gatingEnergy;
        }
        if (m_recentCount >= 30)
            m_maxShortTerm = std::max(m_maxShortTerm, shortTermLufs());
    }

    [[nodiscard]] double momentaryLufs() const noexcept
    {
        return m_recentCount >= 4 ? energyToLufs(meanOfLast(4)) : kLufsFloor;
    }

    [[nodiscard]] double shortTermLufs() const noexcept
    {
        return m_recentCount > 0 ? energyToLufs(meanOfLast(std::min<std::size_t>(30, m_recentCount))) : kLufsFloor;
    }

    [[nodiscard]] double maxShortTermLufs() const noexcept { return m_maxShortTerm; }

    [[nodiscard]] double integratedLufs() const noexcept
    {
        double energy = 0.0;
        double count = 0.0;
        for (const Bin& bin : m_bins) {
            energy += bin.energy;
            count += static_cast<double>(bin.count);
        }
        if (count <= 0.0)
            return kLufsFloor;
        const double relativeGate = energyToLufs(energy / count) - 10.0;
        energy = 0.0;
        count = 0.0;
        for (std::size_t i = binIndex(relativeGate); i < m_bins.size(); ++i) {
            energy += m_bins[i].energy;
            count += static_cast<double>(m_bins[i].count);
        }
        return count > 0.0 ? energyToLufs(energy / count) : kLufsFloor;
    }

    [[nodiscard]] bool hasIntegrated() const noexcept
    {
        return std::any_of(m_bins.begin(), m_bins.end(), [](const Bin& bin) { return bin.count > 0; });
    }

private:
    struct Bin { long long count = 0; double energy = 0.0; };
    static constexpr double kBinWidth = 0.1;
    static constexpr std::size_t kBinCount = 900;   // -70 .. +20 LUFS

    static std::size_t binIndex(double lufs) noexcept
    {
        const double position = (lufs - kLufsFloor) / kBinWidth;
        return static_cast<std::size_t>(std::clamp(position, 0.0, static_cast<double>(kBinCount - 1)));
    }

    double meanOfLast(std::size_t blocks) const noexcept
    {
        double sum = 0.0;
        for (std::size_t i = 1; i <= blocks; ++i)
            sum += m_recent[(m_recentPos + m_recent.size() - i) % m_recent.size()];
        return sum / static_cast<double>(blocks);
    }

    std::array<double, 30> m_recent {};
    std::size_t m_recentCount = 0;
    std::size_t m_recentPos = 0;
    std::array<Bin, kBinCount> m_bins {};
    double m_maxShortTerm = kLufsFloor;
};

} // namespace brockdj::audio
