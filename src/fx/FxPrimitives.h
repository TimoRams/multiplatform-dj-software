#pragma once

#include <juce_dsp/juce_dsp.h>
#include <signalsmith-dsp/delay.h>

#include <algorithm>
#include <cmath>

namespace dsp {

// Thin wrapper over signalsmith::delay::Delay<float>.
//
// write(x)       — advance buffer and store x (call once per sample)
// read(d)        — return sample d frames ago (integer, d >= 1)
// readFrac(d)    — fractional version, linear interpolation (d >= 1.0)
//
// Real-time safe: no allocations in write/read. Call prepare() once from
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
        m_qSmooth.reset(sr, 0.010f);
        m_fcSmooth.setCurrentAndTargetValue(1000.f);
        m_qSmooth.setCurrentAndTargetValue(0.707f);
        reset();
    }

    // Reset filter state (not the smoothers) — call on effect activation.
    void reset() { m_s1[0] = m_s1[1] = m_s2[0] = m_s2[1] = 0.f; }

    // Set smooth targets for next block. Coefficients ramp per-sample.
    void setTargets(float fc, float q)
    {
        m_fcSmooth.setTargetValue(std::clamp(fc, 10.f, m_maxFc));
        m_qSmooth.setTargetValue(std::clamp(q, 0.1f, 10.0f));
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
        const int nc = std::min(buf.getNumChannels(), 2);

        float* chL = buf.getWritePointer(0) + start;
        float* chR = nc > 1 ? buf.getWritePointer(1) + start : chL;

        for (int i = 0; i < n; ++i)
        {
            const float fc = m_fcSmooth.getNextValue();
            const float q  = m_qSmooth.getNextValue();

            // Topology-preserving transform (Zavalishin). The embedded
            // integrator gain is g = tan(π·fc/sr) — an earlier 2·tan() here put
            // every cutoff an octave above the requested frequency, which is why
            // the sound-color filters never reached the ends of their range.
            const float g  = std::tan(pi * std::min(fc, sr * 0.49f) / sr);
            const float k  = 1.f / q;
            const float a1 = 1.f / (1.f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;

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
