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
    prepareDelay();
    prepareSpiral();
    prepareFlanger();
    prepareEnigma();

    // Roll/Slip/Mobius/Nobius buffers are stack-allocated (fixed size), just reset positions
    m_rollState   = RollState{};
    m_mobiusState = MobiusState{};
    m_stretchState = StretchState{};

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
    else if (type == EffectType::Echo)
    {
        processEcho(wetBuf, 0, numSamples, amount, false);
    }
    else if (type == EffectType::LowCutEcho)
    {
        processEcho(wetBuf, 0, numSamples, amount, true);
    }
    else if (type == EffectType::MtDelay)
    {
        processMtDelay(wetBuf, 0, numSamples, amount);
    }
    else if (type == EffectType::Spiral)
    {
        processSpiral(wetBuf, 0, numSamples, amount);
    }
    else if (type == EffectType::Flanger)
    {
        processFlanger(wetBuf, 0, numSamples, amount);
    }
    else if (type == EffectType::Phaser)
    {
        processPhaser(wetBuf, 0, numSamples, amount);
    }
    else if (type == EffectType::Trans)
    {
        processTrans(wetBuf, 0, numSamples, amount);
    }
    else if (type == EffectType::EnigmaJet)
    {
        processEnigmaJet(wetBuf, 0, numSamples, amount);
    }
    else if (type == EffectType::Stretch)
    {
        processStretch(wetBuf, 0, numSamples, amount);
    }
    else if (type == EffectType::SlipRoll)
    {
        processRoll(wetBuf, 0, numSamples, amount, true);
    }
    else if (type == EffectType::Roll)
    {
        processRoll(wetBuf, 0, numSamples, amount, false);
    }
    else if (type == EffectType::Nobius)
    {
        processMobius(wetBuf, 0, numSamples, amount, true);
    }
    else if (type == EffectType::Mobius)
    {
        processMobius(wetBuf, 0, numSamples, amount, false);
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

// ─────────────────────────────────────────────────────────────────────────────
// DelayLine helper
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::DelayLine::prepare(int maxSamples)
{
    buf.assign(static_cast<size_t>(maxSamples), 0.f);
    writePos = 0;
}

float FxProcessor::DelayLine::read(int delaySamples) const
{
    const int sz = static_cast<int>(buf.size());
    if (sz == 0) return 0.f;
    delaySamples = std::clamp(delaySamples, 0, sz - 1);
    int pos = writePos - delaySamples;
    if (pos < 0) pos += sz;
    return buf[static_cast<size_t>(pos)];
}

void FxProcessor::DelayLine::write(float sample)
{
    const int sz = static_cast<int>(buf.size());
    if (sz == 0) return;
    buf[static_cast<size_t>(writePos)] = sample;
    if (++writePos >= sz) writePos = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Echo / Low-Cut Echo
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareDelay()
{
    m_delayState.lineL.prepare(kMaxDelaySamples);
    m_delayState.lineR.prepare(kMaxDelaySamples);
    m_delayState.hpStateL = 0.f;
    m_delayState.hpStateR = 0.f;
}

void FxProcessor::processEcho(juce::AudioBuffer<float>& wet,
                              int start, int n, float amount, bool lowCut)
{
    // Delay time: amount 0→0 = ~100ms, amount 1 = ~600ms
    const int delaySamples = static_cast<int>(m_sampleRate * (0.1 + amount * 0.5));
    // Feedback: amount 0 = 0.2, amount 1 = 0.75
    const float feedback  = 0.2f + amount * 0.55f;
    // High-pass cutoff for Low-Cut Echo (1-pole IIR): ~200 Hz
    const float hpCoeff   = lowCut ? std::exp(-2.0f * juce::MathConstants<float>::pi
                                               * 200.f / static_cast<float>(m_sampleRate))
                                   : 0.f;

    auto process1ch = [&](float* data, DelayLine& line, float& hpState)
    {
        for (int i = 0; i < n; ++i)
        {
            float delayed = line.read(delaySamples);
            if (lowCut)
            {
                // 1-pole high-pass: y[n] = x[n] - x[n-1] + coeff*y[n-1]
                float hp = delayed - hpState + hpCoeff * hpState;
                hpState  = delayed;
                delayed  = hp;
            }
            float out = data[i] + delayed * feedback;
            line.write(out);
            data[i] = out;
        }
    };

    process1ch(wet.getWritePointer(0) + start, m_delayState.lineL, m_delayState.hpStateL);
    if (wet.getNumChannels() > 1)
        process1ch(wet.getWritePointer(1) + start, m_delayState.lineR, m_delayState.hpStateR);
}

// ─────────────────────────────────────────────────────────────────────────────
// MT Delay (Multi-Tap: 3 taps at 1/4, 1/2, 3/4 of max delay)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processMtDelay(juce::AudioBuffer<float>& wet,
                                  int start, int n, float amount)
{
    const int maxDelay    = static_cast<int>(m_sampleRate * (0.1 + amount * 0.5));
    const float feedback  = 0.15f + amount * 0.35f;

    // 3 taps at 25%, 50%, 75% of max delay time
    const int tap1 = maxDelay / 4;
    const int tap2 = maxDelay / 2;
    const int tap3 = (maxDelay * 3) / 4;

    auto process1ch = [&](float* data, DelayLine& line)
    {
        for (int i = 0; i < n; ++i)
        {
            const float t1 = line.read(tap1);
            const float t2 = line.read(tap2);
            const float t3 = line.read(tap3);
            const float tapMix = (t1 * 0.5f + t2 * 0.35f + t3 * 0.25f);
            const float out = data[i] + tapMix * feedback;
            line.write(out);
            data[i] = out;
        }
    };

    process1ch(wet.getWritePointer(0) + start, m_delayState.lineL);
    if (wet.getNumChannels() > 1)
        process1ch(wet.getWritePointer(1) + start, m_delayState.lineR);
}

// ─────────────────────────────────────────────────────────────────────────────
// Spiral (chorus-style modulated delay)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareSpiral()
{
    const int bufSize = static_cast<int>(m_sampleRate * 0.05); // 50ms max
    m_spiralState.bufL.assign(static_cast<size_t>(bufSize), 0.f);
    m_spiralState.bufR.assign(static_cast<size_t>(bufSize), 0.f);
    m_spiralState.writePos = 0;
    m_spiralState.lfoPhase = 0.f;
}

void FxProcessor::processSpiral(juce::AudioBuffer<float>& wet,
                                 int start, int n, float amount)
{
    // LFO rate: 0.2 Hz to 3 Hz
    const float lfoRate  = 0.2f + amount * 2.8f;
    const float lfoInc   = lfoRate / static_cast<float>(m_sampleRate);
    // Modulation depth in samples: 5ms to 20ms
    const float modDepth = static_cast<float>(m_sampleRate) * (0.005f + amount * 0.015f);
    // Base delay: ~15ms
    const float baseDelay = static_cast<float>(m_sampleRate) * 0.015f;
    const int   bufSize   = static_cast<int>(m_spiralState.bufL.size());

    auto process1ch = [&](float* data, std::vector<float>& buf, float lfoOffset)
    {
        float phase = m_spiralState.lfoPhase + lfoOffset;
        for (int i = 0; i < n; ++i)
        {
            buf[static_cast<size_t>(m_spiralState.writePos) % static_cast<size_t>(bufSize)] = data[i];

            float lfo = std::sin(2.f * juce::MathConstants<float>::pi * phase);
            int   delSamples = static_cast<int>(baseDelay + lfo * modDepth);
            delSamples = std::clamp(delSamples, 0, bufSize - 1);

            int readPos = m_spiralState.writePos - delSamples;
            if (readPos < 0) readPos += bufSize;

            data[i] = data[i] * 0.7f + buf[static_cast<size_t>(readPos)] * 0.5f;
            phase += lfoInc;
            if (phase >= 1.f) phase -= 1.f;
        }
    };

    process1ch(wet.getWritePointer(0) + start, m_spiralState.bufL, 0.f);
    if (wet.getNumChannels() > 1)
        process1ch(wet.getWritePointer(1) + start, m_spiralState.bufR, 0.25f);

    // Advance write position and LFO phase together
    for (int i = 0; i < n; ++i)
    {
        m_spiralState.writePos = (m_spiralState.writePos + 1) % bufSize;
        m_spiralState.lfoPhase += lfoInc;
        if (m_spiralState.lfoPhase >= 1.f) m_spiralState.lfoPhase -= 1.f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Flanger
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareFlanger()
{
    const int bufSize = static_cast<int>(m_sampleRate * 0.015); // 15ms max
    m_flangerState.bufL.assign(static_cast<size_t>(bufSize), 0.f);
    m_flangerState.bufR.assign(static_cast<size_t>(bufSize), 0.f);
    m_flangerState.writePos = 0;
    m_flangerState.lfoPhase = 0.f;
}

void FxProcessor::processFlanger(juce::AudioBuffer<float>& wet,
                                  int start, int n, float amount)
{
    // LFO: 0.1 Hz to 2 Hz
    const float lfoRate  = 0.1f + amount * 1.9f;
    const float lfoInc   = lfoRate / static_cast<float>(m_sampleRate);
    // Mod depth: 1ms to 7ms
    const float modDepth = static_cast<float>(m_sampleRate) * (0.001f + amount * 0.006f);
    const float baseDelay = static_cast<float>(m_sampleRate) * 0.003f; // 3ms base
    const float feedback  = 0.4f + amount * 0.3f;
    const int   bufSize   = static_cast<int>(m_flangerState.bufL.size());

    auto process1ch = [&](float* data, std::vector<float>& buf, float lfoOff)
    {
        float phase = m_flangerState.lfoPhase + lfoOff;
        for (int i = 0; i < n; ++i)
        {
            const int wp = m_flangerState.writePos % bufSize;
            float lfo     = std::sin(2.f * juce::MathConstants<float>::pi * phase);
            int   del     = static_cast<int>(baseDelay + lfo * modDepth);
            del = std::clamp(del, 0, bufSize - 1);
            int   rp      = wp - del;
            if (rp < 0) rp += bufSize;

            const float delayed = buf[static_cast<size_t>(rp)];
            buf[static_cast<size_t>(wp)] = data[i] + delayed * feedback;
            data[i] = data[i] + delayed;

            phase += lfoInc;
            if (phase >= 1.f) phase -= 1.f;
        }
    };

    process1ch(wet.getWritePointer(0) + start, m_flangerState.bufL, 0.f);
    if (wet.getNumChannels() > 1)
        process1ch(wet.getWritePointer(1) + start, m_flangerState.bufR, 0.5f);

    for (int i = 0; i < n; ++i)
    {
        m_flangerState.writePos = (m_flangerState.writePos + 1) % bufSize;
        m_flangerState.lfoPhase += lfoInc;
        if (m_flangerState.lfoPhase >= 1.f) m_flangerState.lfoPhase -= 1.f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Phaser (4-stage all-pass chain with LFO-swept frequency)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processPhaser(juce::AudioBuffer<float>& wet,
                                 int start, int n, float amount)
{
    // LFO: 0.1 Hz to 4 Hz
    const float lfoRate = 0.1f + amount * 3.9f;
    const float lfoInc  = lfoRate / static_cast<float>(m_sampleRate);
    // All-pass frequency sweep: 200 Hz to 4 kHz
    const float fMin = 200.f, fMax = 4000.f;
    const float feedback = 0.5f;

    const float twoPiOverSr = 2.f * juce::MathConstants<float>::pi
                              / static_cast<float>(m_sampleRate);

    for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
    {
        float* data  = wet.getWritePointer(ch) + start;
        float  phase = m_phaserState.lfoPhase;

        for (int i = 0; i < n; ++i)
        {
            float lfo = 0.5f * (1.f + std::sin(2.f * juce::MathConstants<float>::pi * phase));
            float freq = fMin + lfo * (fMax - fMin);
            float coeff = (std::tan(twoPiOverSr * freq * 0.5f) - 1.f)
                        / (std::tan(twoPiOverSr * freq * 0.5f) + 1.f);

            float x = data[i];
            for (int s = 0; s < 4; ++s)
            {
                float y = coeff * x + m_phaserState.ap[ch][s]
                        - coeff * m_phaserState.ap[ch][s];
                // 1st order all-pass: y[n] = coeff*(x[n] - y[n-1]) + x[n-1]
                float yn = coeff * (x - m_phaserState.ap[ch][s]) + m_phaserState.ap[ch][s];
                (void)y;
                m_phaserState.ap[ch][s] = yn;
                x = yn;
            }
            data[i] = data[i] + x * feedback;

            phase += lfoInc;
            if (phase >= 1.f) phase -= 1.f;
        }
    }

    for (int i = 0; i < n; ++i)
    {
        m_phaserState.lfoPhase += lfoInc;
        if (m_phaserState.lfoPhase >= 1.f) m_phaserState.lfoPhase -= 1.f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Trans (Tremolo – amplitude LFO)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processTrans(juce::AudioBuffer<float>& wet,
                                int start, int n, float amount)
{
    // LFO rate: amount 0 = 2 Hz, amount 1 = 20 Hz (sync-able in future)
    const float lfoRate = 2.f + amount * 18.f;
    const float lfoInc  = lfoRate / static_cast<float>(m_sampleRate);
    // Depth: amount 0 = mild (0.3), amount 1 = hard chop (1.0)
    const float depth   = 0.3f + amount * 0.7f;

    float phase = m_transState.lfoPhase;
    for (int i = 0; i < n; ++i)
    {
        float lfo = 0.5f * (1.f + std::sin(2.f * juce::MathConstants<float>::pi * phase));
        float gain = 1.f - depth + lfo * depth; // 0 to 1

        for (int ch = 0; ch < wet.getNumChannels(); ++ch)
            wet.getWritePointer(ch)[start + i] *= gain;

        phase += lfoInc;
        if (phase >= 1.f) phase -= 1.f;
    }
    m_transState.lfoPhase = phase;
}

// ─────────────────────────────────────────────────────────────────────────────
// Enigma Jet (deep phaser with 8 stages + small pitch-detune)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareEnigma()
{
    const int bufSize = static_cast<int>(m_sampleRate * 0.02); // 20ms
    m_enigmaState.detBufL.assign(static_cast<size_t>(bufSize), 0.f);
    m_enigmaState.detBufR.assign(static_cast<size_t>(bufSize), 0.f);
    m_enigmaState.detWritePos = 0;
    m_enigmaState.lfoPhase    = 0.f;
    m_enigmaState.detLfo      = 0.f;
}

void FxProcessor::processEnigmaJet(juce::AudioBuffer<float>& wet,
                                    int start, int n, float amount)
{
    // Deep phaser: 8 all-pass stages, very slow LFO (jet sweep)
    const float phaseLfoRate = 0.05f + amount * 0.45f; // 0.05–0.5 Hz
    const float phaseLfoInc  = phaseLfoRate / static_cast<float>(m_sampleRate);
    const float twoPiOverSr  = 2.f * juce::MathConstants<float>::pi
                               / static_cast<float>(m_sampleRate);
    const float fMin = 80.f, fMax = 8000.f;
    const float feedback = 0.7f;

    // Detune: small LFO-modulated delay (chorus-like pitch wobble)
    const float detLfoRate = 0.3f + amount * 1.2f;
    const float detLfoInc  = detLfoRate / static_cast<float>(m_sampleRate);
    const float detDepth   = static_cast<float>(m_sampleRate) * 0.005f; // 5ms
    const int   detBufSize = static_cast<int>(m_enigmaState.detBufL.size());

    for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
    {
        float* data  = wet.getWritePointer(ch) + start;
        std::vector<float>& detBuf = (ch == 0) ? m_enigmaState.detBufL : m_enigmaState.detBufR;
        float phase = m_enigmaState.lfoPhase;
        float detPhase = m_enigmaState.detLfo;

        for (int i = 0; i < n; ++i)
        {
            // 8-stage all-pass phaser
            float lfo   = 0.5f * (1.f + std::sin(2.f * juce::MathConstants<float>::pi * phase));
            float freq  = fMin + lfo * (fMax - fMin);
            float coeff = (std::tan(twoPiOverSr * freq * 0.5f) - 1.f)
                        / (std::tan(twoPiOverSr * freq * 0.5f) + 1.f);

            float x = data[i];
            for (int s = 0; s < 8; ++s)
            {
                float yn = coeff * (x - m_enigmaState.ap[ch][s]) + m_enigmaState.ap[ch][s];
                m_enigmaState.ap[ch][s] = yn;
                x = yn;
            }
            float phased = data[i] + x * feedback;

            // Detune via chorus delay
            detBuf[static_cast<size_t>(m_enigmaState.detWritePos) % static_cast<size_t>(detBufSize)] = phased;
            float detLfo = std::sin(2.f * juce::MathConstants<float>::pi * detPhase);
            int   del    = static_cast<int>(detDepth * 0.5f * (1.f + detLfo));
            del = std::clamp(del, 0, detBufSize - 1);
            int   rp     = m_enigmaState.detWritePos - del;
            if (rp < 0) rp += detBufSize;

            data[i] = phased * 0.6f + detBuf[static_cast<size_t>(rp)] * 0.4f;

            phase    += phaseLfoInc;
            if (phase    >= 1.f) phase    -= 1.f;
            detPhase += detLfoInc;
            if (detPhase >= 1.f) detPhase -= 1.f;

            m_enigmaState.detWritePos = (m_enigmaState.detWritePos + 1) % detBufSize;
        }
    }

    for (int i = 0; i < n; ++i)
    {
        m_enigmaState.lfoPhase += phaseLfoInc;
        if (m_enigmaState.lfoPhase >= 1.f) m_enigmaState.lfoPhase -= 1.f;
        m_enigmaState.detLfo += detLfoInc;
        if (m_enigmaState.detLfo >= 1.f) m_enigmaState.detLfo -= 1.f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stretch (granular freeze – slows read pointer to stretch audio in time)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processStretch(juce::AudioBuffer<float>& wet,
                                  int start, int n, float amount)
{
    // amount 0 = 1x (normal), amount 1 = 0.25x (4x slower)
    const double readSpeed = 1.0 - static_cast<double>(amount) * 0.75;
    const int    bufLen    = kStretchBuf;

    for (int i = 0; i < n; ++i)
    {
        // Write to buffer (both channels)
        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
            m_stretchState.buf[ch][m_stretchState.writePos % bufLen]
                = wet.getReadPointer(ch)[start + i];

        // Linear-interpolated read
        int    rp   = static_cast<int>(m_stretchState.readPos);
        double frac = m_stretchState.readPos - rp;
        int    rp0  = rp % bufLen;
        int    rp1  = (rp + 1) % bufLen;

        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
        {
            float s0 = m_stretchState.buf[ch][rp0];
            float s1 = m_stretchState.buf[ch][rp1];
            wet.getWritePointer(ch)[start + i] = s0 + static_cast<float>(frac) * (s1 - s0);
        }

        m_stretchState.writePos = (m_stretchState.writePos + 1) % bufLen;
        m_stretchState.readPos += readSpeed;
        if (static_cast<int>(m_stretchState.readPos) >= bufLen)
            m_stretchState.readPos -= bufLen;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Roll (stutter beat-repeat) / Slip Roll (with reverse slipping)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processRoll(juce::AudioBuffer<float>& wet,
                               int start, int n, float amount, bool slip)
{
    // Loop length: amount 0 = ~half bar (~512ms at 120BPM), amount 1 = 1/32 bar (~16ms)
    const int targetLen = static_cast<int>(m_sampleRate
                          * std::pow(2.f, -amount * 5.f) * 0.5f); // 500ms → 16ms
    const int loopLen   = std::max(64, std::min(targetLen, kRollBuf / 2));

    auto& st = m_rollState;

    for (int i = 0; i < n; ++i)
    {
        // Always record into rolling buffer
        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
            st.buf[ch][st.writePos % kRollBuf] = wet.getReadPointer(ch)[start + i];

        if (!st.loopActive || st.loopLen != loopLen)
        {
            // Capture a new loop start point
            st.loopStart  = st.writePos;
            st.loopLen    = loopLen;
            st.readPos    = st.loopStart;
            st.loopActive = true;
            st.stepCounter = 0;
        }

        // Read from frozen loop
        const int rp = st.readPos % kRollBuf;
        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
            wet.getWritePointer(ch)[start + i] = st.buf[ch][rp];

        st.writePos = (st.writePos + 1) % kRollBuf;
        ++st.stepCounter;

        if (slip)
        {
            // Slip roll: every loop iteration slips back one step
            const int slipAmount = std::max(1, loopLen / 16);
            if (st.stepCounter >= loopLen)
            {
                st.stepCounter = 0;
                st.loopStart   = (st.loopStart - slipAmount + kRollBuf) % kRollBuf;
                st.readPos     = st.loopStart;
            }
            else
            {
                st.readPos = (st.loopStart + st.stepCounter) % kRollBuf;
            }
        }
        else
        {
            // Regular roll: seamless loop repeat
            st.readPos = (st.loopStart + (st.stepCounter % st.loopLen)) % kRollBuf;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mobius / Nobius (forward + reverse bidir loop)
// Mobius:  continuous forward+reverse ping-pong loop
// Nobius:  like Mobius but with a slightly shifted (half-loop-offset) phase,
//          creating a phase-cancellation / notch-style wash effect
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processMobius(juce::AudioBuffer<float>& wet,
                                 int start, int n, float amount, bool nobius)
{
    // Loop length: 100ms → 1s based on amount
    const int loopLen = std::max(512,
        static_cast<int>(m_sampleRate * (0.1f + amount * 0.9f)));
    const int clampedLen = std::min(loopLen, kMobiusBuf);

    auto& st = m_mobiusState;
    if (st.loopLen != clampedLen)
    {
        st.loopLen = clampedLen;
        st.readPos = 0.0;
        st.forward = true;
    }

    for (int i = 0; i < n; ++i)
    {
        // Record
        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
            st.buf[ch][st.writePos % kMobiusBuf] = wet.getReadPointer(ch)[start + i];

        // Read position for forward/reverse
        int rp0 = static_cast<int>(st.readPos) % kMobiusBuf;
        int rp1 = (rp0 + 1) % kMobiusBuf;
        float frac = static_cast<float>(st.readPos - std::floor(st.readPos));

        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
        {
            float s0 = st.buf[ch][rp0];
            float s1 = st.buf[ch][rp1];
            float sample = s0 + frac * (s1 - s0);

            if (nobius)
            {
                // Nobius: blend in a half-loop-offset read for phasing effect
                int rpN = (rp0 + clampedLen / 2) % kMobiusBuf;
                sample  = sample * 0.5f + st.buf[ch][rpN] * 0.5f;
            }
            wet.getWritePointer(ch)[start + i] = sample;
        }

        st.writePos = (st.writePos + 1) % kMobiusBuf;

        // Ping-pong read direction
        if (st.forward)
        {
            st.readPos += 1.0;
            if (static_cast<int>(st.readPos) >= clampedLen)
            {
                st.readPos = clampedLen - 1.0;
                st.forward = false;
            }
        }
        else
        {
            st.readPos -= 1.0;
            if (st.readPos < 0.0)
            {
                st.readPos = 0.0;
                st.forward = true;
            }
        }
    }
}
