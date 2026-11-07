#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace engine::scratch {

// Timestamped trajectory of an on-screen scratch (waveform drag, virtual
// platter). A pointer reports once per display frame, rounded to whole pixels
// and handed to the GUI thread with a few milliseconds of jitter. Chasing each
// report makes the read head sprint when an event arrives and stall before the
// next one: a rate pulse at the frame rate that sounds like a digital stutter
// on every drag.
//
// The audio thread instead renders the pointer path a short, adaptive delay
// behind real time, where every sample it needs has already arrived, and fits
// a locally weighted line through the surrounding reports. Position and speed
// come from the same fit, so they cannot disagree, and the fit is continuous in
// time: reports only ever enter the window at its leading edge with zero
// weight. A pointer that stops sending is known to be resting, which is
// represented by hold samples, so the head settles exactly on the last report.
//
// Single writer (the GUI/control thread), single reader (the audio callback).
// The reader copies the recent tail under a seqlock and keeps its previous copy
// if a write is in progress, so it never waits, locks or allocates.
class PointerScratchTrajectory final {
public:
    struct Estimate {
        double positionSamples = 0.0;
        double velocitySamplesPerSecond = 0.0;
        double delaySeconds = 0.0;
    };

    static constexpr int kCapacity = 128;
    static constexpr int kReadTail = 48;
    // Margin for the GUI thread delivering a report late.
    static constexpr double kDeliveryJitterSeconds = 0.006;
    static constexpr double kHoldGridSeconds = 0.004;
    static constexpr double kDefaultIntervalSeconds = 1.0 / 60.0;
    static constexpr double kMinIntervalSeconds = 0.004;
    static constexpr double kMaxIntervalSeconds = 0.034;

    static_assert(std::atomic<double>::is_always_lock_free,
                  "Pointer scratch trajectory requires lock-free doubles");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "Pointer scratch trajectory requires lock-free counters");

    // Writer: a new grab at timestampSeconds with the hand at positionSamples.
    void reset(double timestampSeconds, double positionSamples) noexcept
    {
        beginWrite();
        m_generation.fetch_add(1, std::memory_order_relaxed);
        m_count.store(0, std::memory_order_relaxed);
        storeSample(timestampSeconds, positionSamples);
        m_pointerDriven.store(false, std::memory_order_relaxed);
        endWrite();
    }

    // Writer: the pointer reported positionSamples at timestampSeconds.
    void push(double timestampSeconds, double positionSamples) noexcept
    {
        if (!std::isfinite(timestampSeconds) || !std::isfinite(positionSamples))
            return;
        beginWrite();
        const auto count = m_count.load(std::memory_order_relaxed);
        if (count > 0) {
            const auto last = static_cast<std::size_t>((count - 1) % kCapacity);
            timestampSeconds = std::max(
                timestampSeconds, m_times[last].load(std::memory_order_relaxed));
        }
        storeSample(timestampSeconds, positionSamples);
        m_pointerDriven.store(true, std::memory_order_relaxed);
        endWrite();
    }

    // Writer: this scratch is no longer pointer driven.
    void clear() noexcept
    {
        beginWrite();
        m_generation.fetch_add(1, std::memory_order_relaxed);
        m_count.store(0, std::memory_order_relaxed);
        m_pointerDriven.store(false, std::memory_order_relaxed);
        endWrite();
    }

    // Reader (audio thread). Returns false while the scratch is not driven by
    // a pointer, in which case the caller keeps its existing target.
    [[nodiscard]] bool evaluate(double nowSeconds, Estimate& estimate) noexcept
    {
        refreshTail();
        if (!m_local.pointerDriven || m_local.count <= 0 || !std::isfinite(nowSeconds))
            return false;

        const int n = m_local.count;
        const double* t = m_local.times.data();
        const double* p = m_local.positions.data();

        const double measuredInterval = medianInterval();
        if (m_localGeneration != m_smoothedGeneration) {
            m_smoothedGeneration = m_localGeneration;
            m_intervalSeconds = measuredInterval;
        } else {
            // Per callback. Moving the render delay in steps would shift the
            // rendered position by speed × step.
            m_intervalSeconds += (measuredInterval - m_intervalSeconds) * 0.05;
        }
        const double interval = std::clamp(m_intervalSeconds,
                                           kMinIntervalSeconds,
                                           kMaxIntervalSeconds);
        // The future half of the window spans the render delay, so a report
        // enters at its leading edge with zero weight. One and a half
        // intervals average out most of the delivery jitter on the timestamps
        // without the lag becoming noticeable.
        const double delay = 1.5 * interval + kDeliveryJitterSeconds;
        // At least 40 ms of history, so the fit still spans several distinct
        // pixel positions when a fast pointer reports a slow drag.
        const double pastHalfWidth = std::max(2.0 * interval, 0.040);
        const double holdAfter = interval + kDeliveryJitterSeconds;
        const double renderTime = std::max(nowSeconds - delay, t[0]);
        const double windowStart = renderTime - pastHalfWidth;
        const double reference = p[n - 1];

        double s0 = 0.0, s1 = 0.0, s2 = 0.0, q0 = 0.0, q1 = 0.0;
        const auto accumulate = [&](double time, double position) {
            const double x = time - renderTime;
            const double u = x < 0.0 ? x / pastHalfWidth : x / delay;
            if (u <= -1.0 || u >= 1.0)
                return;
            const double k = 1.0 - u * u;
            const double w = k * k;
            const double y = position - reference;
            s0 += w;
            s1 += w * x;
            s2 += w * x * x;
            q0 += w * y;
            q1 += w * x * y;
        };
        // A pointer reports only while it moves. Once a report is older than
        // one interval plus the delivery margin, the hand rested there until
        // the next report (or until now).
        const auto accumulateHold = [&](double from, double to, double position) {
            from = std::max(from, windowStart);
            to = std::min(to, nowSeconds);
            if (to <= from)
                return;
            double grid = std::ceil(from / kHoldGridSeconds) * kHoldGridSeconds;
            for (int guard = 0; grid <= to && guard < 64; ++guard, grid += kHoldGridSeconds)
                accumulate(grid, position);
        };

        for (int i = 0; i < n; ++i) {
            if (t[i] >= windowStart && t[i] <= nowSeconds)
                accumulate(t[i], p[i]);
            const double nextTime = i + 1 < n
                ? t[i + 1] - kDeliveryJitterSeconds
                : nowSeconds;
            if (nextTime > windowStart)
                accumulateHold(t[i] + holdAfter, nextTime, p[i]);
        }

        estimate.delaySeconds = delay;
        if (s0 <= 1.0e-9) {
            // Nothing inside the window: the newest report at or before the
            // render time is where the hand is.
            int index = 0;
            while (index + 1 < n && t[index + 1] <= renderTime)
                ++index;
            estimate.positionSamples = p[index];
            estimate.velocitySamplesPerSecond = 0.0;
            return true;
        }
        const double determinant = s0 * s2 - s1 * s1;
        if (determinant <= 1.0e-12 * std::max(1.0e-30, s0 * s2)) {
            estimate.positionSamples = reference + q0 / s0;
            estimate.velocitySamplesPerSecond = 0.0;
            return true;
        }
        estimate.positionSamples = reference + (s2 * q0 - s1 * q1) / determinant;
        estimate.velocitySamplesPerSecond = (s0 * q1 - s1 * q0) / determinant;
        return true;
    }

    [[nodiscard]] bool pointerDriven() const noexcept
    {
        return m_pointerDriven.load(std::memory_order_relaxed);
    }

private:
    struct LocalTail {
        std::array<double, kReadTail> times {};
        std::array<double, kReadTail> positions {};
        int count = 0;
        bool pointerDriven = false;
    };

    void storeSample(double timestampSeconds, double positionSamples) noexcept
    {
        const auto count = m_count.load(std::memory_order_relaxed);
        const auto index = static_cast<std::size_t>(count % kCapacity);
        m_times[index].store(timestampSeconds, std::memory_order_relaxed);
        m_positions[index].store(positionSamples, std::memory_order_relaxed);
        m_count.store(count + 1, std::memory_order_relaxed);
    }

    void beginWrite() noexcept { m_sequence.fetch_add(1, std::memory_order_acq_rel); }
    void endWrite() noexcept { m_sequence.fetch_add(1, std::memory_order_release); }

    void refreshTail() noexcept
    {
        const auto before = m_sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U)
            return;
        const auto generation = m_generation.load(std::memory_order_relaxed);
        const auto count = m_count.load(std::memory_order_relaxed);
        const bool pointerDriven = m_pointerDriven.load(std::memory_order_relaxed);
        if (before == m_lastSequence)
            return;
        const auto copied = static_cast<int>(std::min<std::uint64_t>(count, kReadTail));
        for (int i = 0; i < copied; ++i) {
            const auto index = static_cast<std::size_t>((count - copied + i) % kCapacity);
            m_scratch.times[static_cast<std::size_t>(i)] =
                m_times[index].load(std::memory_order_relaxed);
            m_scratch.positions[static_cast<std::size_t>(i)] =
                m_positions[index].load(std::memory_order_relaxed);
        }
        const auto after = m_sequence.load(std::memory_order_acquire);
        if (after != before)
            return;
        m_scratch.count = copied;
        m_scratch.pointerDriven = pointerDriven;
        m_local = m_scratch;
        m_localGeneration = generation;
        m_lastSequence = before;
    }

    [[nodiscard]] double medianInterval() const noexcept
    {
        // Only intervals between motion reports describe the pointer's rate;
        // the first sample is the grab, and pauses are holds, not intervals.
        std::array<double, 8> intervals {};
        int used = 0;
        for (int i = m_local.count - 1; i >= 2 && used < static_cast<int>(intervals.size()); --i) {
            const double dt = m_local.times[static_cast<std::size_t>(i)]
                - m_local.times[static_cast<std::size_t>(i - 1)];
            if (dt > 0.0 && dt <= 0.060)
                intervals[static_cast<std::size_t>(used++)] = dt;
        }
        if (used == 0)
            return kDefaultIntervalSeconds;
        std::sort(intervals.begin(), intervals.begin() + used);
        return intervals[static_cast<std::size_t>(used / 2)];
    }

    std::atomic<std::uint64_t> m_sequence { 0 };
    std::atomic<std::uint64_t> m_generation { 0 };
    std::atomic<std::uint64_t> m_count { 0 };
    std::atomic<bool> m_pointerDriven { false };
    std::array<std::atomic<double>, kCapacity> m_times {};
    std::array<std::atomic<double>, kCapacity> m_positions {};

    // Reader state, audio thread only.
    LocalTail m_local;
    LocalTail m_scratch;
    std::uint64_t m_lastSequence = ~std::uint64_t { 0 };
    std::uint64_t m_localGeneration = 0;
    std::uint64_t m_smoothedGeneration = ~std::uint64_t { 0 };
    double m_intervalSeconds = kDefaultIntervalSeconds;
};

} // namespace engine::scratch
