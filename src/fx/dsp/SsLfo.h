#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>

namespace dsp {

// LFO with per-sample smoothed rate transitions.
//
// Usage per sample:
//   lfo.tick();                      // advance phase (call once per sample)
//   float l = lfo.sine();            // read at current phase
//   float r = lfo.sine(0.5f);        // read 180° offset (stereo quadrature)
//   float u = lfo.sineUnipolar();    // 0..1 version
//
// Rate changes set via setRate() ramp over ~50 ms, eliminating the audible
// sweep artifact that occurs when lfoInc jumps between blocks.
class SsLfo
{
    double m_phase      = 0.0;
    double m_sampleRate = 44100.0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_rateSmooth;

public:
    // Call once from prepareToPlay (or any prepare*() function).
    void prepare(double sampleRate)
    {
        m_sampleRate = sampleRate;
        m_rateSmooth.reset(sampleRate, 0.05);
        m_rateSmooth.setCurrentAndTargetValue(0.f);
        m_phase = 0.0;
    }

    void reset() { m_phase = 0.0; }

    void setPhase(double phase)
    {
        m_phase = phase - std::floor(phase);
        if (m_phase < 0.0)
            m_phase += 1.0;
    }

    // Set target rate with 50 ms smooth ramp. Safe to call per block.
    void setRate(float hz)
    {
        m_rateSmooth.setTargetValue(std::max(0.0001f, hz));
    }

    // Set rate immediately — no ramp. Use at init time or on effect activation.
    void setRateImmediate(float hz)
    {
        m_rateSmooth.setCurrentAndTargetValue(std::max(0.0001f, hz));
    }

    // Advance phase by one sample. Call once at the top of each sample loop.
    void tick()
    {
        m_phase += static_cast<double>(m_rateSmooth.getNextValue()) / m_sampleRate;
        if (m_phase >= 1.0) m_phase -= 1.0;
    }

    // Read bipolar sine (-1..+1) at current phase + optional offset (0..1 = 0°..360°).
    float sine(float phaseOffset = 0.f) const
    {
        double p = m_phase + static_cast<double>(phaseOffset);
        if (p >= 1.0) p -= 1.0;
        return static_cast<float>(std::sin(6.283185307179586 * p));
    }

    // Unipolar (0..1).
    float sineUnipolar(float phaseOffset = 0.f) const
    {
        return 0.5f * (1.f + sine(phaseOffset));
    }
};

} // namespace dsp
