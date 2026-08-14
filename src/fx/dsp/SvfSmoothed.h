#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <algorithm>

namespace dsp {

// 2-pole State Variable Filter with per-sample parameter smoothing.
//
// The old SVFState computed filter coefficients once per block → audible
// zipper artifacts when turning a filter knob during a block boundary.
// This class advances two JUCE SmoothedValues (fc, Q) every sample so
// coefficients track the target continuously with no stepping.
//
// Usage (same call sites as old applySCFilter / SVFState):
//   svf.prepare(sampleRate);           // once, from prepareToPlay
//   svf.setTargets(fc_hz, q);          // once per block, from process()
//   svf.process(buf, start, n, lpf);   // per block, inline in applySCFilter
//
// Real-time safe: no allocations in process().
class SvfSmoothed
{
    float m_s1[2] = {}, m_s2[2] = {};

    // Multiplicative smoother for fc: equal perceived speed at low and high Hz.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> m_fcSmooth;
    // Linear smoother for Q (resonance).
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_qSmooth;

    double m_sampleRate = 44100.0;
    float  m_maxFc = 20000.f;

public:
    SvfSmoothed() = default;

    // Filter response modes. LowPass/HighPass are the classic sound-color
    // sweeps; Notch/BandPass are used by the sweep effect.
    enum class Mode { LowPass, HighPass, BandPass, Notch };

    void prepare(double sampleRate)
    {
        m_sampleRate = sampleRate;
        const float sr = static_cast<float>(sampleRate);
        m_maxFc = std::max(1000.f, static_cast<float>(sampleRate) * 0.45f);
        m_fcSmooth.reset(sr, 0.010f);  // 10 ms ramp
        m_qSmooth .reset(sr, 0.010f);
        m_fcSmooth.setCurrentAndTargetValue(1000.f);
        m_qSmooth .setCurrentAndTargetValue(0.707f);
        reset();
    }

    // Reset filter state (not the smoothers) — call on effect activation.
    void reset() { m_s1[0] = m_s1[1] = m_s2[0] = m_s2[1] = 0.f; }

    // Set smooth targets for next block.  Coefficients ramp per-sample.
    void setTargets(float fc, float q)
    {
        m_fcSmooth.setTargetValue(std::clamp(fc, 10.f, m_maxFc));
        m_qSmooth .setTargetValue(std::clamp(q,  0.1f, 10.0f));
    }

    // Process buf[start … start+n) in-place.
    // lowpass=true → LP output, false → HP output.
    void process(juce::AudioBuffer<float>& buf, int start, int n, bool lowpass)
    {
        process(buf, start, n, lowpass ? Mode::LowPass : Mode::HighPass);
    }

    void process(juce::AudioBuffer<float>& buf, int start, int n, Mode mode)
    {
        const float pi = juce::MathConstants<float>::pi;
        const float sr = static_cast<float>(m_sampleRate);
        const int   nc = std::min(buf.getNumChannels(), 2);

        float* chL = buf.getWritePointer(0) + start;
        float* chR = nc > 1 ? buf.getWritePointer(1) + start : chL;

        for (int i = 0; i < n; ++i)
        {
            const float fc = m_fcSmooth.getNextValue();
            const float q  = m_qSmooth .getNextValue();

            // Topology-preserving transform (Zavalishin). The embedded
            // integrator gain is g = tan(π·fc/sr) — an earlier 2·tan() here put
            // every cutoff an octave above the requested frequency, which is why
            // the sound-color filters never reached the ends of their range.
            const float g  = std::tan(pi * std::min(fc, sr * 0.49f) / sr);
            const float k  = 1.f / q;
            const float a1 = 1.f / (1.f + g * (g + k));
            const float a2 = g  * a1;
            const float a3 = g  * a2;

            float* const chans[2] = { chL, chR };
            for (int ch = 0; ch < nc; ++ch)
            {
                const float x  = chans[ch][i];
                const float v3 = x - m_s2[ch];
                const float v1 = a1 * m_s1[ch] + a2 * v3;
                const float v2 = m_s2[ch] + a2 * m_s1[ch] + a3 * v3;
                m_s1[ch] = 2.f * v1 - m_s1[ch];
                m_s2[ch] = 2.f * v2 - m_s2[ch];

                switch (mode)
                {
                    case Mode::LowPass:  chans[ch][i] = v2; break;
                    case Mode::HighPass: chans[ch][i] = x - k * v1 - v2; break;
                    case Mode::BandPass: chans[ch][i] = v1; break;
                    case Mode::Notch:    chans[ch][i] = x - k * v1; break;
                }
            }
        }
    }
};

} // namespace dsp
