#include "FxProcessor.h"
#include <cmath>
#include <algorithm>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// PitchShifterImpl  –  simple ring-buffer pitch shifter (no FFT required).
//
// Algorithm: dual-head read-back through a delay ring buffer.
//   • A write pointer advances at sample rate.
//   • Two read pointers advance at  sampleRate * pitchRatio.
//   • A raised-cosine (Hann) crossfade window blends the two readers so there
//     are no audible discontinuities when one pointer wraps.
//
// Range: ±12 semitones (ratio 0.5 … 2.0).
// Latency: ~bufLen/2 samples (≈ 46 ms at 44.1 kHz with bufLen 4096).
//
// TODO: For production quality, replace with SoundTouch or Rubber Band:
//   #include <SoundTouch.h>
//   soundtouch::SoundTouch st;
//   st.setSampleRate(sampleRate);
//   st.setChannels(numChannels);
//   st.setPitchSemiTones(semitones);
//   // feed: st.putSamples(in, n);
//   // drain: st.receiveSamples(out, n);
// ─────────────────────────────────────────────────────────────────────────────
struct FxProcessor::PitchShifterImpl
{
    static constexpr int kBufLen = 4096;   // must be power-of-2

    // Per-channel ring buffer + fractional read positions
    struct Chan {
        float  buf[kBufLen] = {};
        double readPos0     = 0.0;
        double readPos1     = kBufLen / 2.0; // offset by half buffer
        int    writePos     = 0;
    };

    std::vector<Chan> chans;
    double pitchRatio = 1.0; // 2^(semitones/12)

    void prepare(int numChannels) {
        chans.assign(numChannels, Chan{});
    }

    void setPitchRatio(double ratio) { pitchRatio = ratio; }

    // Hann window value at normalised position t ∈ [0,1)
    static float hann(double t) {
        return 0.5f * (1.0f - static_cast<float>(
            std::cos(2.0 * juce::MathConstants<double>::pi * t)));
    }

    void process(juce::AudioBuffer<float>& buf, int start, int n)
    {
        for (int ch = 0; ch < (int)chans.size() && ch < buf.getNumChannels(); ++ch)
        {
            auto& c = chans[ch];
            float* data = buf.getWritePointer(ch) + start;

            for (int i = 0; i < n; ++i)
            {
                // Write dry sample into ring buffer
                c.buf[c.writePos & (kBufLen - 1)] = data[i];

                // ── Reader 0 ──────────────────────────────────────────────────
                int   r0i  = static_cast<int>(c.readPos0) & (kBufLen - 1);
                float frac0 = static_cast<float>(c.readPos0 - std::floor(c.readPos0));
                int   r0i1 = (r0i + 1) & (kBufLen - 1);
                float s0   = c.buf[r0i] + frac0 * (c.buf[r0i1] - c.buf[r0i]); // lerp

                // ── Reader 1 ──────────────────────────────────────────────────
                int   r1i  = static_cast<int>(c.readPos1) & (kBufLen - 1);
                float frac1 = static_cast<float>(c.readPos1 - std::floor(c.readPos1));
                int   r1i1 = (r1i + 1) & (kBufLen - 1);
                float s1   = c.buf[r1i] + frac1 * (c.buf[r1i1] - c.buf[r1i]);

                // ── Crossfade windows based on fractional distance to write ptr ──
                double dist0 = std::fmod(
                    static_cast<double>(c.writePos) - c.readPos0 + kBufLen, kBufLen);
                double t0    = dist0 / kBufLen;
                float w0     = hann(t0);

                double dist1 = std::fmod(
                    static_cast<double>(c.writePos) - c.readPos1 + kBufLen, kBufLen);
                double t1    = dist1 / kBufLen;
                float w1     = hann(t1);

                // Normalise so the two windows always sum to 1 (avoids amplitude flutter)
                float wSum = w0 + w1;
                if (wSum > 1e-6f) { w0 /= wSum; w1 /= wSum; }

                data[i] = s0 * w0 + s1 * w1;

                // Advance read positions at pitch speed
                c.readPos0 += pitchRatio;
                c.readPos1 += pitchRatio;
                // Wrap fractional positions within buffer
                c.readPos0 = std::fmod(c.readPos0, static_cast<double>(kBufLen));
                c.readPos1 = std::fmod(c.readPos1, static_cast<double>(kBufLen));

                ++c.writePos;
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// FxProcessor
// ─────────────────────────────────────────────────────────────────────────────

FxProcessor::FxProcessor()
    : m_pitchShifter(std::make_unique<PitchShifterImpl>())
{
}

// Defined here so the compiler sees the complete PitchShifterImpl definition
FxProcessor::~FxProcessor() = default;

void FxProcessor::prepare(double sampleRate, int maxBlockSize, int numChannels)
{
    m_sampleRate   = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_numChannels  = numChannels;

    // Smoothed wet/dry: ~20 ms ramp time to eliminate clicks
    const float rampSamples = static_cast<float>(sampleRate * 0.020);
    m_wetSmooth.reset(sampleRate, 0.020);
    m_drySmooth.reset(sampleRate, 0.020);
    m_wetSmooth.setCurrentAndTargetValue(0.0f);
    m_drySmooth.setCurrentAndTargetValue(1.0f);

    prepareReverb();

    m_bcState.assign(static_cast<size_t>(numChannels), BitcrusherState{});

    preparePitchShifter();

    (void)rampSamples;
}

void FxProcessor::process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    // ── Snapshot current targets from atomics ─────────────────────────────────
    const auto  type   = static_cast<EffectType>(m_typeAtomic.load(std::memory_order_relaxed));
    const float amount = m_amountAtomic.load(std::memory_order_relaxed);

    // Update smoothed targets
    const float targetWet = amount;
    const float targetDry = 1.0f - amount;
    m_wetSmooth.setTargetValue(targetWet);
    m_drySmooth.setTargetValue(targetDry);

    // If fully bypassed and smoother has settled → nothing to do
    if (type == EffectType::None && !m_wetSmooth.isSmoothing() && amount < 1e-4f)
        return;

    // ── Build wet buffer (copy of dry, then process in-place) ─────────────────
    juce::AudioBuffer<float> wetBuf(m_numChannels, numSamples);
    copyToWet(buffer, wetBuf, startSample, numSamples);

    if (type == EffectType::Reverb)
    {
        updateReverbParams(amount);
        processReverb(wetBuf, 0, numSamples);
    }
    else if (type == EffectType::Bitcrusher)
    {
        processBitcrusher(wetBuf, 0, numSamples, amount);
    }
    else if (type == EffectType::PitchShifter)
    {
        processPitchShifter(wetBuf, 0, numSamples, amount);
    }
    // EffectType::None → wet buffer stays as dry copy, mixed at `amount` weight

    // ── Per-sample smoothed wet/dry mix back into the main buffer ─────────────
    for (int s = 0; s < numSamples; ++s)
    {
        const float wet = m_wetSmooth.getNextValue();
        const float dry = m_drySmooth.getNextValue();

        for (int ch = 0; ch < std::min(buffer.getNumChannels(), m_numChannels); ++ch)
        {
            float* mainPtr = buffer.getWritePointer(ch) + startSample + s;
            const float wetSample = wetBuf.getReadPointer(ch)[s];
            *mainPtr = (*mainPtr) * dry + wetSample * wet;
        }
    }
}

void FxProcessor::setEffectType(EffectType type)
{
    // When switching effect, ramp wet to 0 first would require a state machine;
    // instead we simply reset the smoother so the crossfade handles any transient.
    m_typeAtomic.store(static_cast<int>(type), std::memory_order_relaxed);

    // Also reset pitch shifter read positions so we don't get a stale delayed burst
    if (type == EffectType::PitchShifter && m_pitchShifter)
        m_pitchShifter->prepare(m_numChannels);
}

void FxProcessor::setAmount(float amount)
{
    m_amountAtomic.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reverb
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareReverb()
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = m_sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(m_maxBlockSize);
    spec.numChannels      = static_cast<juce::uint32>(m_numChannels);
    m_reverb.prepare(spec);
    m_reverb.reset();
}

void FxProcessor::updateReverbParams(float amount)
{
    // amount 0 → subtle room; amount 1 → huge wet hall
    juce::dsp::Reverb::Parameters p;
    p.roomSize   = 0.2f + amount * 0.78f;   // 0.2 … 1.0
    p.damping    = 0.3f + amount * 0.50f;   // 0.3 … 0.8
    p.wetLevel   = amount;                   // 0 … 1  (wet/dry handled externally too)
    p.dryLevel   = 0.0f;                     // dry mixed externally via SmoothedValue
    p.width      = 0.5f + amount * 0.5f;    // 0.5 … 1.0 (stereo width)
    p.freezeMode = 0.0f;
    m_reverb.setParameters(p);
}

void FxProcessor::processReverb(juce::AudioBuffer<float>& wet, int start, int n)
{
    juce::dsp::AudioBlock<float> block(wet);
    auto sub = block.getSubBlock(static_cast<size_t>(start), static_cast<size_t>(n));
    juce::dsp::ProcessContextReplacing<float> ctx(sub);
    m_reverb.process(ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bitcrusher
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processBitcrusher(juce::AudioBuffer<float>& wet,
                                    int start, int n, float amount)
{
    // ── Bit depth reduction ───────────────────────────────────────────────────
    // amount 0.0 → 16 bit (65536 steps), amount 1.0 → 2 bit (4 steps)
    // Exponential mapping so the middle of the knob (~8 bit) sounds interesting.
    const float t         = amount;                          // 0 … 1
    const float bitDepth  = 16.0f * std::pow(2.0f / 16.0f, t);  // 16 … 2 bits
    const float steps     = std::pow(2.0f, bitDepth);       // 65536 … 4
    const float invSteps  = 1.0f / steps;

    // ── Sample-rate reduction (Sample-and-Hold) ───────────────────────────────
    // amount 0.0 → hold 1 sample (no reduction), amount 1.0 → hold 32 samples
    const int holdLen = 1 + static_cast<int>(amount * 31.0f);

    for (int ch = 0; ch < wet.getNumChannels() && ch < (int)m_bcState.size(); ++ch)
    {
        float* data = wet.getWritePointer(ch) + start;
        auto&  st   = m_bcState[static_cast<size_t>(ch)];

        for (int i = 0; i < n; ++i)
        {
            // Sample-and-hold
            if (st.holdCounter <= 0)
            {
                // Quantise to `steps` levels, symmetric around 0
                float q = std::floor(data[i] * steps * 0.5f + 0.5f) * invSteps * 2.0f;
                q = std::clamp(q, -1.0f, 1.0f);
                st.holdSample  = q;
                st.holdCounter = holdLen;
            }
            data[i] = st.holdSample;
            --st.holdCounter;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pitch Shifter
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::preparePitchShifter()
{
    m_pitchShifter->prepare(m_numChannels);
}

void FxProcessor::processPitchShifter(juce::AudioBuffer<float>& wet,
                                      int start, int n, float amount)
{
    // amount 0.0 → -12 semitones (pitchRatio 0.5)
    // amount 0.5 → 0  semitones  (pitchRatio 1.0)
    // amount 1.0 → +12 semitones (pitchRatio 2.0)
    const double semitones  = (static_cast<double>(amount) - 0.5) * 24.0; // -12 … +12
    const double pitchRatio = std::pow(2.0, semitones / 12.0);
    m_pitchShifter->setPitchRatio(pitchRatio);
    m_pitchShifter->process(wet, start, n);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::copyToWet(const juce::AudioBuffer<float>& src,
                            juce::AudioBuffer<float>& wet,
                            int start, int n)
{
    for (int ch = 0; ch < std::min(src.getNumChannels(), wet.getNumChannels()); ++ch)
        wet.copyFrom(ch, 0, src, ch, start, n);
}

void FxProcessor::mixWetDry(juce::AudioBuffer<float>& buffer,
                            const juce::AudioBuffer<float>& wetBuf,
                            int start, int n,
                            float wetGain, float dryGain)
{
    for (int ch = 0; ch < buffer.getNumChannels() && ch < wetBuf.getNumChannels(); ++ch)
    {
        float* dst       = buffer.getWritePointer(ch) + start;
        const float* wet = wetBuf.getReadPointer(ch);

        for (int i = 0; i < n; ++i)
            dst[i] = dst[i] * dryGain + wet[i] * wetGain;
    }
}
