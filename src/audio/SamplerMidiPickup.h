#pragma once
#include <cmath>

class SamplerMidiPickup {
public:
    void arm() noexcept { m_acquired = false; }
    void disconnect() noexcept { arm(); m_previous = -1; }
    bool pending() const noexcept { return !m_acquired; }
    bool accept(double value, double target) noexcept {
        constexpr double tolerance = 1.0 / 127.0;
        if (!std::isfinite(value) || value < 0 || value > 1) return false;
        if (!m_acquired && (std::abs(value - target) <= tolerance
            || (m_previous >= 0 && (std::abs(m_previous - target) <= tolerance
                || (m_previous < target && value >= target) || (m_previous > target && value <= target)))))
            m_acquired = true;
        m_previous = value;
        return m_acquired;
    }
private:
    bool m_acquired = false;
    double m_previous = -1;
};
