#pragma once

#include <atomic>

// One audio producer, one meter consumer. Retains peaks only until the next
// read; this is a sampling window, not a timed peak hold or release envelope.
class MeterPeakAccumulator final {
public:
    static_assert(std::atomic<float>::is_always_lock_free);

    void publish(float peak) noexcept
    {
        float previous = m_peak.load(std::memory_order_relaxed);
        while (peak > previous && !m_peak.compare_exchange_weak(
                   previous, peak, std::memory_order_relaxed)) {}
    }

    [[nodiscard]] float take() noexcept
    {
        return m_peak.exchange(0.0f, std::memory_order_relaxed);
    }

private:
    std::atomic<float> m_peak { 0.0f };
};
