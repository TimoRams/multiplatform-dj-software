#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

class TempoTimeline {
public:
    static constexpr std::int64_t kCapacity = 65536;

    void reset(double position, double rate) noexcept
    {
        m_frame = 0;
        m_end = 0;
        m_origin = position;
        m_initialRate = rate;
        m_tailRate = rate;
        m_target = rate;
        m_remaining = 0;
        m_step = 0.0;
        m_positions[0] = position;
    }

    void setTarget(double rate, int rampSamples) noexcept
    {
        if (rate == m_target)
            return;
        m_target = rate;
        m_remaining = std::max(1, rampSamples);
        m_step = (rate - m_tailRate) / m_remaining;
    }

    void ensure(int offset) noexcept
    {
        const auto end = m_frame + std::clamp(offset, 0, static_cast<int>(kCapacity / 2));
        while (m_end < end) {
            if (m_remaining > 0) {
                m_tailRate += m_step;
                if (--m_remaining == 0)
                    m_tailRate = m_target;
            }
            const auto index = static_cast<std::size_t>(m_end % kCapacity);
            m_rates[index] = m_tailRate;
            const double position = m_positions[index] + m_tailRate;
            ++m_end;
            m_positions[static_cast<std::size_t>(m_end % kCapacity)] = position;
        }
    }

    double position(int offset) noexcept
    {
        ensure(offset);
        const auto frame = m_frame + offset;
        if (frame < 0)
            return m_origin + frame * m_initialRate;
        return m_positions[static_cast<std::size_t>(frame % kCapacity)];
    }

    double rate(int offset) noexcept
    {
        ensure(offset + 1);
        const auto frame = m_frame + offset;
        if (frame < 0)
            return m_initialRate;
        return m_rates[static_cast<std::size_t>(frame % kCapacity)];
    }

    void advance(int frames) noexcept
    {
        ensure(frames);
        m_frame += frames;
    }

private:
    std::array<double, kCapacity> m_positions {};
    std::array<double, kCapacity> m_rates {};
    std::int64_t m_frame = 0;
    std::int64_t m_end = 0;
    double m_origin = 0.0;
    double m_initialRate = 1.0;
    double m_tailRate = 1.0;
    double m_target = 1.0;
    double m_step = 0.0;
    int m_remaining = 0;
};
