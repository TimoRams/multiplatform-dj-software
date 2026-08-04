#pragma once

#include <algorithm>
#include <optional>

namespace midi_internal {

class Midi14BitAccumulator
{
public:
    void pushMsb(int value) noexcept
    {
        m_msb = std::clamp(value, 0, 127);
    }

    void pushLsb(int value) noexcept
    {
        m_lsb = std::clamp(value, 0, 127);
    }

    [[nodiscard]] std::optional<int> takeValue() noexcept
    {
        if (!m_msb || !m_lsb)
            return std::nullopt;

        const int value = (*m_msb << 7) | *m_lsb;
        m_msb.reset();
        m_lsb.reset();
        return value;
    }

    void reset() noexcept
    {
        m_msb.reset();
        m_lsb.reset();
    }

private:
    std::optional<int> m_msb;
    std::optional<int> m_lsb;
};

// Some controllers expose a nominal 14-bit control but occasionally publish
// only its MSB when a port is opened. Hold that first coarse sample until a
// matching LSB arrives or actual movement proves the MSB stream is live.
class MidiUnpairedMsbGate
{
public:
    [[nodiscard]] bool shouldPublish(int value) noexcept
    {
        const int clamped = std::clamp(value, 0, 127);
        if (m_confirmed)
            return true;

        if (!m_baseline) {
            m_baseline = clamped;
            return false;
        }

        if (*m_baseline == clamped)
            return false;

        m_confirmed = true;
        return true;
    }

    void confirmPair() noexcept
    {
        m_confirmed = true;
    }

    void reset() noexcept
    {
        m_baseline.reset();
        m_confirmed = false;
    }

private:
    std::optional<int> m_baseline;
    bool m_confirmed = false;
};

} // namespace midi_internal
