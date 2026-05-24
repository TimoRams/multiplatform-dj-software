#pragma once

#include "dsp/delay.h"

namespace dsp {

// Thin wrapper over signalsmith::delay::Delay<float, Linear>.
//
// Semantics match the old hand-rolled DelayLine (read-before-write), so call
// sites are unchanged.  The new readFrac(float) overload enables smooth,
// click-free delay time transitions — pass it a JUCE SmoothedValue target
// instead of an integer sample count.
//
// Mapping to Signalsmith internals (derivation):
//   Old  read(d)  before write(x)  ≡  impl.at(d - 1)  before push(x)
//   because Signalsmith at(0) = most-recently-pushed sample.
//
// Real-time safe: no allocations in write/read.  Call prepare() once from
// prepareToPlay(); the internal buffer is a std::vector.
class SsDelay
{
    signalsmith::delay::Delay<float, signalsmith::delay::Linear> m_impl;

public:
    void prepare(int maxSamples)
    {
        m_impl.resize(maxSamples, 0.f);
    }

    // Write a sample and advance the buffer.
    void write(float x) { m_impl.push(x); }

    // Integer read: returns sample d frames ago (d >= 1).
    // Equivalent to the old DelayLine::read(int).
    float read(int d) const
    {
        return m_impl.at(static_cast<float>(d) - 1.0f);
    }

    // Fractional read: d may be non-integer (d >= 1.0).
    // Linear interpolation via Signalsmith kernel — no stepping artifacts
    // when delay time changes smoothly between blocks.
    float readFrac(float d) const
    {
        return m_impl.at(d - 1.0f);
    }
};

} // namespace dsp
