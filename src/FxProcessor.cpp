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
    prepareSCDelays();

    // SC crush per-channel state
    m_scCrushState.bc.assign(static_cast<size_t>(numChannels), BitcrusherState{});

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
    // ── Sound Color FX: use bipolar SC knob, NOT amount/SmoothedValue ─────────
    // These functions handle their own wet/dry internally and write directly
    // into `buffer` (not wetBuf), then return so the smoothed mix is skipped.
    else if (type == EffectType::SoundColorFilter)
    {
        const float knob = m_scKnobAtomic.load(std::memory_order_relaxed);
        processSC_Filter(buffer, startSample, numSamples, knob);
        return;
    }
    else if (type == EffectType::SoundColorDubEcho)
    {
        const float knob = m_scKnobAtomic.load(std::memory_order_relaxed);
        processSC_DubEcho(buffer, startSample, numSamples, knob);
        return;
    }
    else if (type == EffectType::SoundColorCrush)
    {
        const float knob = m_scKnobAtomic.load(std::memory_order_relaxed);
        processSC_Crush(buffer, startSample, numSamples, knob);
        return;
    }
    else if (type == EffectType::SoundColorSpace)
    {
        const float knob = m_scKnobAtomic.load(std::memory_order_relaxed);
        processSC_Space(buffer, startSample, numSamples, knob);
        return;
    }
    else if (type == EffectType::SoundColorPitch)
    {
        const float knob = m_scKnobAtomic.load(std::memory_order_relaxed);
        processSC_Pitch(buffer, startSample, numSamples, knob);
        return;
    }
    else if (type == EffectType::SoundColorNoise)
    {
        const float knob = m_scKnobAtomic.load(std::memory_order_relaxed);
        processSC_Noise(buffer, startSample, numSamples, knob);
        return;
    }
    else if (type == EffectType::SoundColorSweep)
    {
        const float knob = m_scKnobAtomic.load(std::memory_order_relaxed);
        const float param = m_scParamAtomic.load(std::memory_order_relaxed);
        processSC_Sweep(buffer, startSample, numSamples, knob, param);
        return;
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

void FxProcessor::setSCKnobValue(float knob)
{
    m_scKnobAtomic.store(std::clamp(knob, -1.0f, 1.0f), std::memory_order_relaxed);
}

void FxProcessor::setSCParamValue(float param)
{
    m_scParamAtomic.store(std::clamp(param, 0.0f, 1.0f), std::memory_order_relaxed);
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
    // Delay time: 100ms (amount 0) to 500ms (amount 1)
    const int delaySamples = std::clamp(
        static_cast<int>(m_sampleRate * (0.1 + amount * 0.4)),
        1, kMaxDelaySamples - 1);
    // Feedback: 0.25 to 0.65 — never unstable
    const float feedback = 0.25f + amount * 0.40f;
    // HP coefficient for low-cut variant
    const float hpAlpha = lowCut
        ? 1.f / (1.f + 2.f * juce::MathConstants<float>::pi * 200.f
                       / static_cast<float>(m_sampleRate))
        : 0.f;

    for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
    {
        float* data = wet.getWritePointer(ch) + start;
        DelayLine& line = (ch == 0) ? m_delayState.lineL : m_delayState.lineR;
        float& hpPrev = (ch == 0) ? m_delayState.hpStateL : m_delayState.hpStateR;

        for (int i = 0; i < n; ++i)
        {
            float delayed = line.read(delaySamples);

            if (lowCut)
            {
                // Simple 1-pole high-pass (removes bass from feedback loop)
                float filtered = hpAlpha * (hpPrev + delayed - hpPrev);
                // Actually: 1-pole HP: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
                // We need to store x[n-1] too. Simplified approach:
                float hp = delayed - hpPrev;
                hpPrev   = delayed;
                delayed  = hp;
            }

            // Soft-clip the feedback to prevent blowup
            float fb = delayed * feedback;
            fb = std::tanh(fb);

            float out = data[i] + fb;
            line.write(out);
            data[i] = out;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MT Delay (Multi-Tap: 3 taps at 1/4, 1/2, 3/4 of max delay)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processMtDelay(juce::AudioBuffer<float>& wet,
                                  int start, int n, float amount)
{
    const int maxDelay = std::clamp(
        static_cast<int>(m_sampleRate * (0.1 + amount * 0.4)),
        4, kMaxDelaySamples - 1);
    const float feedback = 0.15f + amount * 0.30f;

    const int tap1 = std::max(1, maxDelay / 4);
    const int tap2 = std::max(2, maxDelay / 2);
    const int tap3 = std::max(3, (maxDelay * 3) / 4);

    for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
    {
        float* data = wet.getWritePointer(ch) + start;
        DelayLine& line = (ch == 0) ? m_delayState.lineL : m_delayState.lineR;

        for (int i = 0; i < n; ++i)
        {
            const float t1 = line.read(tap1);
            const float t2 = line.read(tap2);
            const float t3 = line.read(tap3);
            const float tapMix = t1 * 0.45f + t2 * 0.35f + t3 * 0.25f;
            const float fb = std::tanh(tapMix * feedback);
            const float out = data[i] + fb;
            line.write(out);
            data[i] = out;
        }
    }
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
    if (bufSize == 0) return;

    const int numCh = std::min(wet.getNumChannels(), 2);
    float* data[2] = { wet.getWritePointer(0) + start,
                       numCh > 1 ? wet.getWritePointer(1) + start : nullptr };
    std::vector<float>* bufs[2] = { &m_spiralState.bufL, &m_spiralState.bufR };
    // Per-channel LFO offset for stereo width
    const float lfoOff[2] = { 0.f, 0.25f };

    for (int i = 0; i < n; ++i)
    {
        const int wp = m_spiralState.writePos;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& buf = *bufs[ch];
            buf[static_cast<size_t>(wp)] = data[ch][i];

            float phase = m_spiralState.lfoPhase + lfoOff[ch];
            if (phase >= 1.f) phase -= 1.f;
            float lfo = std::sin(2.f * juce::MathConstants<float>::pi * phase);

            // Fractional delay for smooth modulation
            float delF = baseDelay + lfo * modDepth;
            delF = std::clamp(delF, 0.f, static_cast<float>(bufSize - 1));
            int   delI = static_cast<int>(delF);
            float frac = delF - static_cast<float>(delI);

            int rp0 = wp - delI;
            if (rp0 < 0) rp0 += bufSize;
            int rp1 = rp0 - 1;
            if (rp1 < 0) rp1 += bufSize;

            float delayed = buf[static_cast<size_t>(rp0)] * (1.f - frac)
                          + buf[static_cast<size_t>(rp1)] * frac;

            data[ch][i] = data[ch][i] * 0.7f + delayed * 0.5f;
        }

        // Advance writePos and LFO ONCE per sample
        m_spiralState.writePos = (wp + 1) % bufSize;
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
    if (bufSize == 0) return;

    const int numCh = std::min(wet.getNumChannels(), 2);
    float* data[2] = { wet.getWritePointer(0) + start,
                       numCh > 1 ? wet.getWritePointer(1) + start : nullptr };
    std::vector<float>* bufs[2] = { &m_flangerState.bufL, &m_flangerState.bufR };
    const float lfoOff[2] = { 0.f, 0.5f };

    for (int i = 0; i < n; ++i)
    {
        const int wp = m_flangerState.writePos;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& buf = *bufs[ch];

            float phase = m_flangerState.lfoPhase + lfoOff[ch];
            if (phase >= 1.f) phase -= 1.f;
            float lfo = std::sin(2.f * juce::MathConstants<float>::pi * phase);

            // Fractional delay with linear interpolation
            float delF = baseDelay + lfo * modDepth;
            delF = std::clamp(delF, 0.f, static_cast<float>(bufSize - 1));
            int   delI = static_cast<int>(delF);
            float frac = delF - static_cast<float>(delI);

            int rp0 = wp - delI;
            if (rp0 < 0) rp0 += bufSize;
            int rp1 = rp0 - 1;
            if (rp1 < 0) rp1 += bufSize;

            float delayed = buf[static_cast<size_t>(rp0)] * (1.f - frac)
                          + buf[static_cast<size_t>(rp1)] * frac;

            // Write input + soft-clipped feedback into delay buffer
            buf[static_cast<size_t>(wp)] = data[ch][i] + std::tanh(delayed * feedback);
            // Output = input + delayed
            data[ch][i] = data[ch][i] + delayed;
        }

        // Advance writePos and LFO ONCE per sample
        m_flangerState.writePos = (wp + 1) % bufSize;
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

    const int numCh = std::min(wet.getNumChannels(), 2);

    for (int i = 0; i < n; ++i)
    {
        // Compute LFO and all-pass coefficient once per sample
        float lfo = 0.5f * (1.f + std::sin(2.f * juce::MathConstants<float>::pi
                                           * m_phaserState.lfoPhase));
        float freq = fMin + lfo * (fMax - fMin);
        float tanHalf = std::tan(twoPiOverSr * freq * 0.5f);
        float coeff = (tanHalf - 1.f) / (tanHalf + 1.f);

        for (int ch = 0; ch < numCh; ++ch)
        {
            float x = wet.getWritePointer(ch)[start + i];

            // 4-stage all-pass cascade
            // Correct 1st-order all-pass: y[n] = coeff * (x[n] - y[n-1]) + x[n-1]
            for (int s = 0; s < 4; ++s)
            {
                float xn = x;
                float yn = coeff * (xn - m_phaserState.yPrev[ch][s])
                         + m_phaserState.xPrev[ch][s];
                m_phaserState.xPrev[ch][s] = xn;
                m_phaserState.yPrev[ch][s] = yn;
                x = yn;
            }

            // Mix dry + phased with feedback depth
            wet.getWritePointer(ch)[start + i] += x * feedback;
        }

        // Advance LFO ONCE per sample
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
    if (detBufSize == 0) return;

    const int numCh = std::min(wet.getNumChannels(), 2);
    float* data[2] = { wet.getWritePointer(0) + start,
                       numCh > 1 ? wet.getWritePointer(1) + start : nullptr };
    std::vector<float>* detBufs[2] = { &m_enigmaState.detBufL, &m_enigmaState.detBufR };

    for (int i = 0; i < n; ++i)
    {
        // Compute phaser LFO + coefficient once per sample
        float lfo   = 0.5f * (1.f + std::sin(2.f * juce::MathConstants<float>::pi
                                              * m_enigmaState.lfoPhase));
        float freq  = fMin + lfo * (fMax - fMin);
        float tanHalf = std::tan(twoPiOverSr * freq * 0.5f);
        float coeff = (tanHalf - 1.f) / (tanHalf + 1.f);

        // Detune LFO
        float detLfoVal = std::sin(2.f * juce::MathConstants<float>::pi
                                   * m_enigmaState.detLfo);

        const int wp = m_enigmaState.detWritePos;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& detBuf = *detBufs[ch];

            // 8-stage all-pass cascade (correct formula)
            float x = data[ch][i];
            for (int s = 0; s < 8; ++s)
            {
                float xn = x;
                float yn = coeff * (xn - m_enigmaState.yPrev[ch][s])
                         + m_enigmaState.xPrev[ch][s];
                m_enigmaState.xPrev[ch][s] = xn;
                m_enigmaState.yPrev[ch][s] = yn;
                x = yn;
            }
            float phased = data[ch][i] + x * feedback;

            // Detune via chorus delay with linear interpolation
            detBuf[static_cast<size_t>(wp)] = phased;
            float delF = detDepth * 0.5f * (1.f + detLfoVal);
            delF = std::clamp(delF, 0.f, static_cast<float>(detBufSize - 1));
            int   delI = static_cast<int>(delF);
            float frac = delF - static_cast<float>(delI);

            int rp0 = wp - delI;
            if (rp0 < 0) rp0 += detBufSize;
            int rp1 = rp0 - 1;
            if (rp1 < 0) rp1 += detBufSize;

            float delayed = detBuf[static_cast<size_t>(rp0)] * (1.f - frac)
                          + detBuf[static_cast<size_t>(rp1)] * frac;

            data[ch][i] = phased * 0.6f + delayed * 0.4f;
        }

        // Advance all state ONCE per sample
        m_enigmaState.detWritePos = (wp + 1) % detBufSize;
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

// ─────────────────────────────────────────────────────────────────────────────
// Sound Color FX — Pioneer DDJ-FLX10 style
//
// All SC effects receive a bipolar knob value: -1.0 (max left) … 0.0 (centre/
// bypass) … +1.0 (max right).  At 0.0 the signal passes through 100% dry.
//
// Architecture: [Effect core] → [Bipolar SVF LPF/HPF] → mix back into buffer.
//
// The SVF (State Variable Filter) is used for smooth morphing because it:
//  • Has no instability risk across the full frequency range
//  • Naturally separates LP and HP outputs in one pass
//  • Allows Q to be modulated without artefacts
// ─────────────────────────────────────────────────────────────────────────────

// ── Prepare SC delay lines ────────────────────────────────────────────────────
void FxProcessor::prepareSCDelays()
{
    m_scDubEchoState.lineL.prepare(kMaxDelaySamples);
    m_scDubEchoState.lineR.prepare(kMaxDelaySamples);
    m_scDubEchoState.lpL = 0.f;
    m_scDubEchoState.lpR = 0.f;
    m_scDubEchoState.svf = SVFState{};
    m_scFilterState.svfA = SVFState{};
    m_scFilterState.svfB = SVFState{};
    m_scSpaceState.svf   = SVFState{};
    m_scNoiseState.svf   = SVFState{};
    m_scNoiseState.seed  = 12345u;
    m_scCrushState.svf   = SVFState{};
    m_scSweepState = SCSweepState{};
}

// ── Bipolar SVF helper ────────────────────────────────────────────────────────
//
// Processes buf in-place with a 2-pole SVF.
// knob < 0 → LPF:  cutoff from 20 kHz (|knob|=0) down to 20 Hz (|knob|=1)
// knob > 0 → HPF:  cutoff from 20 Hz  (knob=0)   up   to 20 kHz (knob=1)
// knob = 0 → transparent (no filtering)
// Q rises from 0.7 (open) to 1.8 (full) for a slight resonance sweep.
//
// Returns wet gain (0 at centre, 1 at extremes) so callers know mix amount.
float FxProcessor::applySCFilter(juce::AudioBuffer<float>& buf, int start, int n,
                                  float knob, SVFState& state)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return 0.f;   // fully transparent at centre

    const float sr  = static_cast<float>(m_sampleRate);
    const float pi  = juce::MathConstants<float>::pi;

    // Cutoff frequency: exponential mapping 20 Hz ↔ 20 kHz
    // At absK=0 → 20kHz (LPF fully open / HPF fully closed), |knob|=1 → 20Hz / 20kHz
    float fc;
    if (knob < 0.f)
        // LPF: 20 kHz → 20 Hz as knob goes -1
        fc = 20000.f * std::pow(20.f / 20000.f, absK);
    else
        // HPF: 20 Hz → 20 kHz as knob goes +1
        fc = 20.f * std::pow(20000.f / 20.f, absK);

    fc = std::clamp(fc, 20.f, 20000.f);

    const float q   = 0.7f + absK * 1.1f;  // 0.7 → 1.8
    const float w   = 2.f * std::tan(pi * fc / sr); // SVF g coefficient
    const float k   = 1.f / q;
    const float a1  = 1.f / (1.f + k * w + w * w);
    const float a2  = w * a1;
    const float a3  = w * a2;

    const int numCh = std::min(buf.getNumChannels(), 2);
    for (int i = 0; i < n; ++i)
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            float x = buf.getWritePointer(ch)[start + i];

            float v3 = x - state.s2[ch];
            float v1 = a1 * state.s1[ch] + a2 * v3;
            float v2 = state.s2[ch]       + a2 * state.s1[ch] + a3 * v3;

            state.s1[ch] = 2.f * v1 - state.s1[ch];
            state.s2[ch] = 2.f * v2 - state.s2[ch];

            // Choose LP or HP output based on sign of knob
            float out = (knob < 0.f) ? v2 : (x - k * v1 - v2);
            buf.getWritePointer(ch)[start + i] = out;
        }
    }
    return absK; // wet gain = |knob|
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. SC FILTER — pure dual resonant LPF/HPF
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Filter(juce::AudioBuffer<float>& buffer,
                                    int start, int n, float knob)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return; // centre = bypass

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);
    const float q = 0.70f + param * 15.30f;
    const float sr = static_cast<float>(m_sampleRate);
    const float pi = juce::MathConstants<float>::pi;

    float fc;
    if (knob < 0.f)
        fc = 20000.f * std::pow(20.f / 20000.f, absK);
    else
        fc = 20.f * std::pow(20000.f / 20.f, absK);
    fc = std::clamp(fc, 20.f, 20000.f);

    const float w = 2.f * std::tan(pi * fc / sr);
    const float k = 1.f / std::max(0.2f, q);
    const float a1 = 1.f / (1.f + k * w + w * w);
    const float a2 = w * a1;
    const float a3 = w * a2;

    // Copy to wet buffer
    juce::AudioBuffer<float> wetBuf(m_numChannels, n);
    copyToWet(buffer, wetBuf, start, n);

    // 24 dB/oct slope: cascade two 2-pole SVF stages with shared cutoff/Q.
    const int numChProc = std::min(wetBuf.getNumChannels(), 2);
    for (int i = 0; i < n; ++i) {
        for (int ch = 0; ch < numChProc; ++ch) {
            float* d = wetBuf.getWritePointer(ch);
            const float x = d[i];

            float v3a = x - m_scFilterState.svfA.s2[ch];
            float v1a = a1 * m_scFilterState.svfA.s1[ch] + a2 * v3a;
            float v2a = m_scFilterState.svfA.s2[ch] + a2 * m_scFilterState.svfA.s1[ch] + a3 * v3a;
            m_scFilterState.svfA.s1[ch] = 2.f * v1a - m_scFilterState.svfA.s1[ch];
            m_scFilterState.svfA.s2[ch] = 2.f * v2a - m_scFilterState.svfA.s2[ch];
            const float stageA = (knob < 0.f) ? v2a : (x - k * v1a - v2a);

            float v3b = stageA - m_scFilterState.svfB.s2[ch];
            float v1b = a1 * m_scFilterState.svfB.s1[ch] + a2 * v3b;
            float v2b = m_scFilterState.svfB.s2[ch] + a2 * m_scFilterState.svfB.s1[ch] + a3 * v3b;
            m_scFilterState.svfB.s1[ch] = 2.f * v1b - m_scFilterState.svfB.s1[ch];
            m_scFilterState.svfB.s2[ch] = 2.f * v2b - m_scFilterState.svfB.s2[ch];
            d[i] = (knob < 0.f) ? v2b : (stageA - k * v1b - v2b);
        }
    }

    // Resonance-driven edge emphasis (musical "squelch") controlled by PARAM.
    // PARAM=0 is near isolator behavior (minimal bump), PARAM=1 is aggressive.
    const float edgeGain = 1.0f + param * 0.9f;
    const int numChEdge = std::min(wetBuf.getNumChannels(), m_numChannels);
    for (int ch = 0; ch < numChEdge; ++ch) {
        float* d = wetBuf.getWritePointer(ch);
        for (int i = 0; i < n; ++i)
            d[i] = std::tanh(d[i] * edgeGain);
    }

    // Wet amount: quick fade-in from centre so there's no sudden cut at 0
    const float wet = absK;
    const float dry = 1.f - wet;
    const int   numCh = std::min(buffer.getNumChannels(), m_numChannels);
    for (int ch = 0; ch < numCh; ++ch)
    {
        float*       dst = buffer.getWritePointer(ch) + start;
        const float* src = wetBuf.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            dst[i] = dst[i] * dry + src[i] * wet;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. SC DUB ECHO — delay + bipolar LPF/HPF on the wet tail
//    knob < 0 → dark dub echo (delay + LPF in feedback)
//    knob > 0 → bright sibilant echo (delay + HPF in feedback)
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_DubEcho(juce::AudioBuffer<float>& buffer,
                                     int start, int n, float knob)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);

    // PARAM controls delay time from musical short to long range.
    // 120 BPM reference: 1/16 = 125 ms, 3/4 = 1500 ms.
    const double delaySec = 0.125 + static_cast<double>(param) * (1.50 - 0.125);
    const int delaySamples = std::clamp(
        static_cast<int>(m_sampleRate * delaySec),
        1, kMaxDelaySamples - 1);
    const float feedback = std::clamp(0.30f + 0.45f * param + 0.15f * absK, 0.2f, 0.92f);
    const float tapeLpHz = 12000.0f - param * 11000.0f; // darker at high PARAM
    const float tapeAlpha = std::clamp(2.0f * juce::MathConstants<float>::pi
                                       * (tapeLpHz / static_cast<float>(m_sampleRate)),
                                       0.001f, 0.99f);

    // Build wet (echo) buffer
    juce::AudioBuffer<float> wetBuf(m_numChannels, n);
    copyToWet(buffer, wetBuf, start, n);

    // Process delay per channel
    const int numCh = std::min(wetBuf.getNumChannels(), 2);
    for (int ch = 0; ch < numCh; ++ch)
    {
        float* data = wetBuf.getWritePointer(ch);
        DelayLine& line = (ch == 0) ? m_scDubEchoState.lineL : m_scDubEchoState.lineR;

        for (int i = 0; i < n; ++i)
        {
            float delayed = line.read(delaySamples);

            float& lpState = (ch == 0) ? m_scDubEchoState.lpL : m_scDubEchoState.lpR;
            lpState += tapeAlpha * (delayed - lpState);
            float tapeDelayed = lpState;

            float fb = std::tanh(tapeDelayed * feedback);
            float out = data[i] + fb;
            line.write(out);
            data[i] = tapeDelayed;
        }
    }

    // Apply bipolar filter on wet tail
    applySCFilter(wetBuf, 0, n, knob, m_scDubEchoState.svf);

    // Mix: wet grows with |knob|
    const float wet = absK;
    const float dry = 1.f - wet;
    for (int ch = 0; ch < numCh; ++ch)
    {
        float*       dst = buffer.getWritePointer(ch) + start;
        const float* src = wetBuf.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            dst[i] = dst[i] * dry + src[i] * wet;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. SC CRUSH — bitcrusher + bipolar LPF/HPF
//    knob < 0 → crunchy + dark (LPF closes as crushing increases)
//    knob > 0 → crunchy + bright/thin (HPF opens as crushing increases)
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Crush(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);
    const float intensity = std::clamp(absK * (0.15f + 0.85f * param), 0.0f, 1.0f);

    // Build wet buffer
    juce::AudioBuffer<float> wetBuf(m_numChannels, n);
    copyToWet(buffer, wetBuf, start, n);

    // Bit depth: 16 bit -> 2 bit with intensity
    const float bitDepth = 16.f * std::pow(2.f / 16.f, intensity);
    const float steps    = std::pow(2.f, bitDepth);
    const float invSteps = 1.f / steps;
    // Decimation: 1 -> 96 samples with intensity
    const int holdLen = 1 + static_cast<int>(intensity * 95.f);

    // Ensure per-channel state
    if (m_scCrushState.bc.size() < static_cast<size_t>(m_numChannels))
        m_scCrushState.bc.assign(static_cast<size_t>(m_numChannels), BitcrusherState{});

    const int numCh = std::min(wetBuf.getNumChannels(), m_numChannels);
    for (int ch = 0; ch < numCh; ++ch)
    {
        float* data = wetBuf.getWritePointer(ch);
        auto&  st   = m_scCrushState.bc[static_cast<size_t>(ch)];

        for (int i = 0; i < n; ++i)
        {
            if (st.holdCounter <= 0)
            {
                float q = std::floor(data[i] * steps * 0.5f + 0.5f) * invSteps * 2.f;
                st.holdSample  = std::clamp(q, -1.f, 1.f);
                st.holdCounter = holdLen;
            }
            data[i] = st.holdSample;
            --st.holdCounter;
        }
    }

    // Bipolar filter pass
    applySCFilter(wetBuf, 0, n, knob, m_scCrushState.svf);

    const float wet = intensity;
    const float dry = 1.f - wet;
    for (int ch = 0; ch < numCh; ++ch)
    {
        float*       dst = buffer.getWritePointer(ch) + start;
        const float* src = wetBuf.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            dst[i] = dst[i] * dry + src[i] * wet;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. SC SPACE — reverb + bipolar LPF/HPF on wet
//    knob < 0 → dark hall (LPF on reverb tail)
//    knob > 0 → bright icy hall (HPF on reverb tail, only sibilants echo)
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Space(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);

    juce::AudioBuffer<float> wetBuf(m_numChannels, n);
    copyToWet(buffer, wetBuf, start, n);

    // PARAM controls room size + decay length.
    // left (0): short room/chamber, right (1): huge long hall.
    juce::dsp::Reverb::Parameters p;
    p.roomSize   = 0.18f + param * 0.82f;
    p.damping    = 0.85f - param * 0.70f;
    p.wetLevel   = 1.f;   // wet/dry handled externally
    p.dryLevel   = 0.f;
    p.width      = 0.55f + param * 0.45f;
    p.freezeMode = std::clamp((param - 0.90f) * 2.0f, 0.0f, 1.0f);
    m_reverb.setParameters(p);

    processReverb(wetBuf, 0, n);

    // Apply bipolar filter on reverb output
    applySCFilter(wetBuf, 0, n, knob, m_scSpaceState.svf);

    const float wet = absK;
    const float dry = 1.f - wet;
    const int numCh = std::min(buffer.getNumChannels(), m_numChannels);
    for (int ch = 0; ch < numCh; ++ch)
    {
        float*       dst = buffer.getWritePointer(ch) + start;
        const float* src = wetBuf.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            dst[i] = dst[i] * dry + src[i] * wet;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. SC PITCH — ±12 semitones, no filter, wet fades in quickly from centre
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Pitch(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);

    juce::AudioBuffer<float> wetBuf(m_numChannels, n);
    copyToWet(buffer, wetBuf, start, n);

    // PARAM controls max pitch range:
    // 0.0 => +/-1 semitone, 1.0 => +/-36 semitones (3 octaves).
    const double maxRangeSemitones = 1.0 + static_cast<double>(param) * 35.0;
    const double semitones  = static_cast<double>(knob) * maxRangeSemitones;
    const double pitchRatio = std::pow(2.0, semitones / 12.0);
    m_pitchShifter->setPitchRatio(pitchRatio);
    m_pitchShifter->process(wetBuf, 0, n);

    // Wet ramps quickly to 100% — at |knob| > 0.15 it's already full wet
    const float wet = std::min(1.f, absK * 6.f);
    const float dry = 1.f - wet;
    const int numCh = std::min(buffer.getNumChannels(), m_numChannels);
    for (int ch = 0; ch < numCh; ++ch)
    {
        float*       dst = buffer.getWritePointer(ch) + start;
        const float* src = wetBuf.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            dst[i] = dst[i] * dry + src[i] * wet;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. SC NOISE — additive white noise through bipolar LPF/HPF
//    knob < 0 → low-pass filtered noise (wind/rumble sweep)
//    knob > 0 → high-pass filtered noise (sibilant hiss sweep)
//    Track audio runs 100% dry underneath — noise is purely additive.
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Noise(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);

    // Generate white noise into a temporary buffer
    juce::AudioBuffer<float> noiseBuf(m_numChannels, n);
    noiseBuf.clear();

    const int numCh = std::min(buffer.getNumChannels(), m_numChannels);
    for (int i = 0; i < n; ++i)
    {
        m_scNoiseState.seed = m_scNoiseState.seed * 1664525u + 1013904223u;
        const float white = static_cast<float>(static_cast<int32_t>(m_scNoiseState.seed))
                            / static_cast<float>(0x7fffffff);
        for (int ch = 0; ch < numCh; ++ch)
            noiseBuf.getWritePointer(ch)[i] = white;
    }

    // Apply bipolar filter to the noise
    applySCFilter(noiseBuf, 0, n, knob, m_scNoiseState.svf);

    // Add filtered noise on top of dry signal
    // Noise gain scales with |knob| — at full knob noise is quite audible
    const float noiseGain = absK * (0.08f + param * 1.20f);
    for (int ch = 0; ch < numCh; ++ch)
    {
        float*       dst   = buffer.getWritePointer(ch) + start;
        const float* noise = noiseBuf.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
        {
            dst[i] += noise[i] * noiseGain;
            dst[i]  = std::clamp(dst[i], -1.f, 1.f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. SC SWEEP — animated LP/HP sweep with user parameter for rate/depth
//    knob sign selects LP (left) vs HP (right) flavor.
//    param controls sweep motion: 0 = subtle/slow, 1 = fast/deep.
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Sweep(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob, float param)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return;

    juce::AudioBuffer<float> wetBuf(m_numChannels, n);
    copyToWet(buffer, wetBuf, start, n);

    // PARAM controls resonance (Q): low -> subtle, high -> aggressive/whistling.
    const float sr = static_cast<float>(m_sampleRate);
    const float pi = juce::MathConstants<float>::pi;
    const float q = 0.7f + param * 19.3f;

    // Color controls cutoff trajectory.
    float fc;
    if (knob < 0.f)
        fc = 18000.f * std::pow(80.f / 18000.f, absK);   // downward sweep for notch side
    else
        fc = 80.f * std::pow(18000.f / 80.f, absK);      // upward sweep for HP side
    fc = std::clamp(fc, 20.f, 20000.f);

    const float w = 2.f * std::tan(pi * fc / sr);
    const float r = 1.f / std::max(0.2f, q);
    const float hpScale = std::clamp(1.0f + param * 0.35f, 1.0f, 1.35f);

    const int numCh = std::min(wetBuf.getNumChannels(), 2);
    for (int i = 0; i < n; ++i) {
        for (int ch = 0; ch < numCh; ++ch) {
            float* data = wetBuf.getWritePointer(ch);
            const float x = data[i];

            // Stage 1 SVF
            float hp1 = x - m_scSweepState.lp1[ch] - r * m_scSweepState.bp1[ch];
            m_scSweepState.bp1[ch] += w * hp1;
            m_scSweepState.lp1[ch] += w * m_scSweepState.bp1[ch];

            // Stage 2 SVF for steeper contour
            float hp2 = x - m_scSweepState.lp2[ch] - r * m_scSweepState.bp2[ch];
            m_scSweepState.bp2[ch] += w * hp2;
            m_scSweepState.lp2[ch] += w * m_scSweepState.bp2[ch];

            const float notch = 0.5f * ((m_scSweepState.lp1[ch] + hp1) + (m_scSweepState.lp2[ch] + hp2));
            const float hpOut = 0.5f * (hp1 + hp2) * hpScale;

            data[i] = (knob < 0.f) ? notch : hpOut;
        }
    }

    const float wet = std::clamp(absK, 0.0f, 1.0f);
    const float dry = 1.f - wet;
    const int outCh = std::min(buffer.getNumChannels(), m_numChannels);
    for (int ch = 0; ch < outCh; ++ch)
    {
        float* dst = buffer.getWritePointer(ch) + start;
        const float* src = wetBuf.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            dst[i] = dst[i] * dry + src[i] * wet;
    }
}
