// Vendored subset of Signalsmith DSP — delay.h
// Original: https://github.com/Signalsmith-Audio/signalsmith-dsp
// License:  MIT (Copyright 2021 Signalsmith Audio Ltd.)
//
// Included: Delay<Sample, Interpolator> and the Linear interpolator.
#pragma once

#include <vector>
#include <algorithm>

namespace signalsmith {
namespace delay {

struct Linear {};
struct Cubic  {};

// Circular delay buffer.
//   at(0) = most-recently pushed sample
//   at(1) = the one before that
//   at(1.5) = linear interpolation between those two
//
// Real-time safe: no allocation in push() / at().
template<typename Sample, class Interpolator = Linear>
struct Delay
{
    Delay()        { resize(1);   }
    Delay(int cap) { resize(cap); }

    void resize(int capacity, Sample fill = Sample(0))
    {
        m_buf.assign(static_cast<std::size_t>(capacity), fill);
        m_capacity = capacity;
        m_pos      = 0;
    }

    void push(Sample v)
    {
        if (--m_pos < 0) m_pos = m_capacity - 1;
        m_buf[static_cast<std::size_t>(m_pos)] = v;
    }

    Sample at(Sample index) const
    {
        const Sample clamped = std::max(Sample(0),
                               std::min(index, static_cast<Sample>(m_capacity - 1)));
        Sample physPos = static_cast<Sample>(m_pos) + clamped;
        if (physPos >= static_cast<Sample>(m_capacity))
            physPos -= static_cast<Sample>(m_capacity);
        return interpolate(physPos, Interpolator{});
    }

    Sample operator[](int index) const
    {
        int pos = m_pos + index;
        if (pos >= m_capacity) pos -= m_capacity;
        if (pos < 0)           pos += m_capacity;
        return m_buf[static_cast<std::size_t>(pos)];
    }

private:
    std::vector<Sample> m_buf;
    int                 m_capacity = 1;
    int                 m_pos      = 0;

    Sample interpolate(Sample physPos, Linear) const
    {
        const int    i0 = static_cast<int>(physPos);
        const Sample f  = physPos - static_cast<Sample>(i0);
        const Sample s0 = m_buf[static_cast<std::size_t>(i0)];
        // Wrap i1 instead of relying on a guard zone.
        const int    i1 = (i0 + 1 < m_capacity) ? i0 + 1 : 0;
        const Sample s1 = m_buf[static_cast<std::size_t>(i1)];
        return s0 + f * (s1 - s0);
    }

    Sample interpolate(Sample physPos, Cubic) const
    {
        return interpolate(physPos, Linear{});
    }
};

} // namespace delay
} // namespace signalsmith
