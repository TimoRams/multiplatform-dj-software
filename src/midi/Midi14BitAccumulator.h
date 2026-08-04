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

} // namespace midi_internal
