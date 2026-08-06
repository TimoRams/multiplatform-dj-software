#pragma once

#include <signalsmith-dsp/delay.h>

namespace dsp {

// Thin wrapper over signalsmith::delay::Delay<float>.
//
// write(x)       — advance buffer and store x (call once per sample)
// read(d)        — return sample d frames ago (integer, d >= 1)
// readFrac(d)    — fractional version, linear interpolation (d >= 1.0)
//
// Real-time safe: no allocations in write/read.  Call prepare() once from
// prepareToPlay(); the internal buffer is a std::vector.
class SsDelay
{
    signalsmith::delay::Delay<float> m_impl;

public:
    void prepare(int maxSamples)
    {
        m_impl.resize(maxSamples, 0.f);
    }

    void reset() { m_impl.reset(0.f); }

    // Write a sample and advance the buffer.
    void write(float x) { m_impl.write(x); }

    // Integer read: returns sample d frames ago (d >= 1).
    float read(int d) const
    {
        return m_impl.read(static_cast<float>(d));
    }

    // Fractional read: d may be non-integer (d >= 1.0).
    // Linear interpolation via Signalsmith kernel.
    float readFrac(float d) const
    {
        return m_impl.read(d);
    }
};

} // namespace dsp
