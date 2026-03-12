#include "BrickwallLimiter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

BrickwallLimiter::BrickwallLimiter() = default;

// ═════════════════════════════════════════════════════════════════════════════
// prepare() — called from prepareToPlay() on the message thread
// ═════════════════════════════════════════════════════════════════════════════

void BrickwallLimiter::prepare(double sampleRate, int maxBlockSize, int numChannels)
{
    m_sampleRate   = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_numChannels  = numChannels;

    // Delay line: allocate enough for max lookahead (5 ms) + some headroom.
    // Round up to next power of 2 for branchless modulo via bitmask.
    const int maxLookahead = static_cast<int>(std::ceil(0.005 * sampleRate)) + 1;
    m_delaySize = 1;
    while (m_delaySize < maxLookahead)
        m_delaySize <<= 1;
    m_delayMask = m_delaySize - 1;

    m_delayBuffer.resize(static_cast<size_t>(numChannels));
    for (auto& ch : m_delayBuffer)
        ch.assign(static_cast<size_t>(m_delaySize), 0.0f);
    m_delayWritePos = 0;

    // True-peak history buffers
    m_truePeakHistory.resize(static_cast<size_t>(numChannels));
    for (auto& h : m_truePeakHistory)
        h.fill(0.0f);

    updateCoefficients();
    reset();
}

// ═════════════════════════════════════════════════════════════════════════════
// reset()
// ═════════════════════════════════════════════════════════════════════════════

void BrickwallLimiter::reset()
{
    m_envelope     = 1.0f;
    m_envelopeSlow = 1.0f;
    m_delayWritePos = 0;

    for (auto& ch : m_delayBuffer)
        std::fill(ch.begin(), ch.end(), 0.0f);

    for (auto& h : m_truePeakHistory)
        h.fill(0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// processBlock() — real-time audio thread, zero allocations
// ═════════════════════════════════════════════════════════════════════════════

float BrickwallLimiter::processBlock(float* const* channelData, int numChannels,
                                      int startSample, int numSamples)
{
    const float threshold   = m_threshold.load(std::memory_order_relaxed);
    const float ceiling     = m_ceiling.load(std::memory_order_relaxed);
    const bool  truePeakOn  = m_truePeakOn.load(std::memory_order_relaxed);
    const float saturation  = m_saturationAmt.load(std::memory_order_relaxed);
    const bool  enabled     = m_enabled.load(std::memory_order_relaxed);

    // Crossfade ramp increment: reach target (0 or 1) over kCrossfadeSamples
    const float targetMix   = enabled ? 1.0f : 0.0f;
    const float rampStep    = 1.0f / static_cast<float>(kCrossfadeSamples);

    const int channels = std::min(numChannels, m_numChannels);
    float minGainReduction = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const int sampleIdx = startSample + i;

        // ── Ramp bypass mix toward target ────────────────────────────────
        if (m_bypassMix < targetMix)
            m_bypassMix = std::min(m_bypassMix + rampStep, 1.0f);
        else if (m_bypassMix > targetMix)
            m_bypassMix = std::max(m_bypassMix - rampStep, 0.0f);

        // ── 1. Always feed audio into the delay line (consistent latency) ─
        float peakLevel = 0.0f;

        for (int ch = 0; ch < channels; ++ch)
        {
            float sample = channelData[ch][sampleIdx];

            // Apply saturation only when active (scaled by mix)
            if (saturation > 0.0f && m_bypassMix > 0.0f)
            {
                float saturated = applySaturation(sample);
                sample = sample + m_bypassMix * (saturated - sample);
            }

            // Always write into the delay line
            delayWrite(ch, sample);

            // Peak detect for sidechain (needed even during crossfade)
            float peak;
            if (truePeakOn)
                peak = detectTruePeak(sample, ch);
            else
                peak = std::abs(sample);

            peakLevel = std::max(peakLevel, peak);
        }

        // ── 2. Compute desired gain reduction ────────────────────────────
        float targetGain = computeGain(peakLevel);

        // ── 3. Adaptive envelope follower ────────────────────────────────
        float smoothedGain = applyEnvelope(targetGain);

        // ── 4. Read from delay line and blend limited / dry ──────────────
        //    Dry  = delayed signal as-is (gain = 1.0)
        //    Wet  = delayed signal × smoothedGain × ceiling
        //    Blend via m_bypassMix (0 = dry, 1 = wet)
        for (int ch = 0; ch < channels; ++ch)
        {
            float delayed = delayRead(ch);

            float wet  = delayed * smoothedGain * ceiling;
            wet = std::clamp(wet, -ceiling, ceiling);

            float dry  = delayed;

            channelData[ch][sampleIdx] = dry + m_bypassMix * (wet - dry);
        }

        minGainReduction = std::min(minGainReduction, smoothedGain);

        // Advance delay write pointer
        delayAdvance();
    }

    return minGainReduction;
}

// ═════════════════════════════════════════════════════════════════════════════
// Soft-knee saturation: tanh-based waveshaper
// ═════════════════════════════════════════════════════════════════════════════

float BrickwallLimiter::applySaturation(float sample) const
{
    const float amt = m_saturationAmt.load(std::memory_order_relaxed);
    if (amt <= 0.0f)
        return sample;

    // Blend between clean and tanh-saturated signal.
    // Fast tanh approximation: x / (1 + |x|)  — Pade-style, no exp().
    const float drive = 1.0f + amt * 2.0f;   // drive range: 1.0–3.0
    const float driven = sample * drive;
    const float saturated = driven / (1.0f + std::abs(driven));
    // Normalise back so unit input ≈ unit output
    const float norm = saturated / (drive / (1.0f + drive));

    return sample + amt * (norm - sample);
}

// ═════════════════════════════════════════════════════════════════════════════
// True-peak detection via 4× polyphase FIR interpolation
// ═════════════════════════════════════════════════════════════════════════════

float BrickwallLimiter::detectTruePeak(float sample, int channel) const
{
    auto& hist = m_truePeakHistory[static_cast<size_t>(channel)];

    // Shift history and insert new sample
    hist[0] = hist[1];
    hist[1] = hist[2];
    hist[2] = hist[3];
    hist[3] = sample;

    float peak = 0.0f;

    // Evaluate each of the 4 polyphase phases
    for (int phase = 0; phase < kOversampleFactor; ++phase)
    {
        float interp = 0.0f;
        for (int tap = 0; tap < kFirTaps; ++tap)
            interp += kFirCoeffs[phase][tap] * hist[tap];

        peak = std::max(peak, std::abs(interp));
    }

    return peak;
}

// ═════════════════════════════════════════════════════════════════════════════
// Gain computer: how much gain reduction is needed?
// ═════════════════════════════════════════════════════════════════════════════

float BrickwallLimiter::computeGain(float peakLevel) const
{
    const float threshold = m_threshold.load(std::memory_order_relaxed);
    if (peakLevel <= threshold)
        return 1.0f;   // no reduction needed

    return threshold / peakLevel;
}

// ═════════════════════════════════════════════════════════════════════════════
// Adaptive envelope follower (dual-release)
//
// Attack: fast (derived from lookahead) — the gain reduction ramps down
//         over the lookahead period so the delayed audio sees the reduction
//         by the time it exits the delay line.
//
// Release: program-dependent dual-release:
//   • Fast release (~5–20 ms): recovers quickly after short transients
//   • Slow release (~80–200 ms): prevents pumping on sustained loud passages
//   The actual release coefficient is interpolated between fast and slow
//   based on how long the signal has been in compression.
// ═════════════════════════════════════════════════════════════════════════════

float BrickwallLimiter::applyEnvelope(float targetGain)
{
    if (targetGain < m_envelope)
    {
        // Attack — we need MORE gain reduction (lower envelope value).
        // Use the attack coefficient derived from lookahead time.
        m_envelope = m_attackCoeff * m_envelope + (1.0f - m_attackCoeff) * targetGain;
        // Also push the slow envelope so it tracks sustained compression
        m_envelopeSlow = m_releaseSlow * m_envelopeSlow + (1.0f - m_releaseSlow) * targetGain;
    }
    else
    {
        // Release — we can allow gain to recover.
        // Adaptive: if the slow envelope is much lower than 1.0, the signal
        // has been loud for a while → use slower release to avoid pumping.
        // If the slow envelope is near 1.0, it was a short transient → fast release.
        const float slowDelta = 1.0f - m_envelopeSlow;
        // Blend factor: 0 when slow envelope = 1.0 (transient), 1 when heavily compressed
        const float blend = std::clamp(slowDelta * 4.0f, 0.0f, 1.0f);
        const float releaseCoeff = m_releaseFast + blend * (m_releaseSlow - m_releaseFast);

        m_envelope = releaseCoeff * m_envelope + (1.0f - releaseCoeff) * targetGain;

        // Let the slow follower recover too
        m_envelopeSlow = m_releaseSlow * m_envelopeSlow + (1.0f - m_releaseSlow) * 1.0f;
    }

    return m_envelope;
}

// ═════════════════════════════════════════════════════════════════════════════
// Delay line helpers
// ═════════════════════════════════════════════════════════════════════════════

void BrickwallLimiter::delayWrite(int channel, float sample)
{
    m_delayBuffer[static_cast<size_t>(channel)][m_delayWritePos] = sample;
}

float BrickwallLimiter::delayRead(int channel) const
{
    const int readPos = (m_delayWritePos - m_lookaheadSamples + m_delaySize) & m_delayMask;
    return m_delayBuffer[static_cast<size_t>(channel)][readPos];
}

void BrickwallLimiter::delayAdvance()
{
    m_delayWritePos = (m_delayWritePos + 1) & m_delayMask;
}

// ═════════════════════════════════════════════════════════════════════════════
// Parameter setters
// ═════════════════════════════════════════════════════════════════════════════

void BrickwallLimiter::setThreshold(float thresholdLinear)
{
    m_threshold.store(std::clamp(thresholdLinear, 0.1f, 1.0f), std::memory_order_relaxed);
}

void BrickwallLimiter::setCeiling(float ceilingLinear)
{
    m_ceiling.store(std::clamp(ceilingLinear, 0.1f, 1.0f), std::memory_order_relaxed);
}

void BrickwallLimiter::setLookaheadMs(float ms)
{
    m_lookaheadMs.store(std::clamp(ms, 1.0f, 5.0f), std::memory_order_relaxed);
    updateCoefficients();
}

void BrickwallLimiter::setRelease(float releaseMs)
{
    m_releaseMs.store(std::clamp(releaseMs, 10.0f, 500.0f), std::memory_order_relaxed);
    updateCoefficients();
}

void BrickwallLimiter::setSaturationAmount(float amount)
{
    m_saturationAmt.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
}

void BrickwallLimiter::setTruePeakEnabled(bool enabled)
{
    m_truePeakOn.store(enabled, std::memory_order_relaxed);
}

void BrickwallLimiter::setEnabled(bool enabled)
{
    m_enabled.store(enabled, std::memory_order_relaxed);
}

bool BrickwallLimiter::isEnabled() const
{
    return m_enabled.load(std::memory_order_relaxed);
}

// ═════════════════════════════════════════════════════════════════════════════
// Internal coefficient update
// ═════════════════════════════════════════════════════════════════════════════

void BrickwallLimiter::updateCoefficients()
{
    if (m_sampleRate <= 0.0)
        return;

    // Lookahead in samples
    const float laMs = m_lookaheadMs.load(std::memory_order_relaxed);
    m_lookaheadSamples = std::max(1, static_cast<int>(std::round(laMs * 0.001 * m_sampleRate)));
    // Clamp to buffer size
    m_lookaheadSamples = std::min(m_lookaheadSamples, m_delaySize - 1);

    // Attack coefficient: envelope reaches target within the lookahead period.
    // Using 1-pole filter: coeff = exp(-1 / (tau * sampleRate))
    // tau chosen so ~99% settling ≈ 5*tau occurs within lookahead time.
    const double attackTimeSec = static_cast<double>(m_lookaheadSamples) / m_sampleRate / 5.0;
    m_attackCoeff = static_cast<float>(std::exp(-1.0 / (attackTimeSec * m_sampleRate)));

    // Dual-release coefficients
    const float releaseBaseMs = m_releaseMs.load(std::memory_order_relaxed);

    // Fast release: ~1/5 of base release (quick transient recovery)
    const double fastMs = std::max(5.0, static_cast<double>(releaseBaseMs) * 0.2);
    m_releaseFast = static_cast<float>(std::exp(-1.0 / (fastMs * 0.001 * m_sampleRate)));

    // Slow release: base release (sustained program, anti-pumping)
    m_releaseSlow = static_cast<float>(std::exp(-1.0 / (static_cast<double>(releaseBaseMs) * 0.001 * m_sampleRate)));
}
