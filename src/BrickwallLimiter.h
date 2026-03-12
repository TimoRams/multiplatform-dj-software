#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════════
// BrickwallLimiter — Mastering-grade true-peak brickwall limiter
// ═══════════════════════════════════════════════════════════════════════════════
//
// Design goals:
//   • Lookahead (1–5 ms) via zero-allocation circular buffer
//   • Program-dependent adaptive release (dual-release envelope)
//   • 4× oversampled true-peak detection (ISP-safe)
//   • Soft-knee saturation stage before the brickwall
//   • Completely real-time safe: no heap allocations after prepare()
//
// Signal flow:
//   Input → Soft-Knee Saturation → Sidechain (True-Peak Detection)
//                                        ↓
//                             Gain Computer (threshold / peak)
//                                        ↓
//                             Adaptive Envelope Follower
//                                        ↓
//                           Lookahead Delay Line ← delayed audio
//                                        ↓
//                                   Gain Apply → Output
//
// The limiter operates per-sample over all channels simultaneously.
// ═══════════════════════════════════════════════════════════════════════════════

class BrickwallLimiter
{
public:
    BrickwallLimiter();

    // Call once from prepareToPlay().  Allocates all buffers.
    void prepare(double sampleRate, int maxBlockSize, int numChannels);

    // Reset all state (call on track change or transport stop/start).
    void reset();

    // Process a block in-place.  `buffer` is interleaved or planar JUCE-style
    // (channel pointers).  `startSample` and `numSamples` define the slice.
    // Returns the minimum gain reduction applied in this block (for UI metering).
    float processBlock(float* const* channelData, int numChannels,
                       int startSample, int numSamples);

    // ── Parameters (thread-safe, set from UI thread) ─────────────────────
    void setThreshold(float thresholdLinear);   // e.g. 0.99 = –0.08 dBFS
    void setCeiling(float ceilingLinear);       // hard output ceiling
    void setLookaheadMs(float ms);              // 1.0–5.0 ms
    void setRelease(float releaseMs);           // base release time
    void setSaturationAmount(float amount);     // 0.0 = off, 1.0 = full saturation
    void setTruePeakEnabled(bool enabled);
    void setEnabled(bool enabled);              // seamless on/off with crossfade
    bool isEnabled() const;

private:
    // ── Soft-knee saturation ─────────────────────────────────────────────
    float applySaturation(float sample) const;

    // ── True-peak detection (4× oversampling) ────────────────────────────
    float detectTruePeak(float sample, int channel) const;

    // ── Gain computer ────────────────────────────────────────────────────
    float computeGain(float peakLevel) const;

    // ── Adaptive envelope follower ───────────────────────────────────────
    float applyEnvelope(float targetGain);

    // ── Delay line helpers ───────────────────────────────────────────────
    void  delayWrite(int channel, float sample);
    float delayRead(int channel) const;
    void  delayAdvance();

    // ── State ────────────────────────────────────────────────────────────
    double m_sampleRate     = 44100.0;
    int    m_maxBlockSize   = 512;
    int    m_numChannels    = 2;

    // Parameters (atomic for thread-safe UI access)
    std::atomic<float> m_threshold      { 0.99f };
    std::atomic<float> m_ceiling        { 1.0f };
    std::atomic<float> m_lookaheadMs    { 3.0f };
    std::atomic<float> m_releaseMs      { 100.0f };
    std::atomic<float> m_saturationAmt  { 0.3f };
    std::atomic<bool>  m_truePeakOn     { true };

    // Computed coefficients (updated in prepare / setLookahead)
    int   m_lookaheadSamples = 0;
    float m_attackCoeff      = 0.0f;   // derived from lookahead time

    // Dual-release envelope coefficients
    float m_releaseFast      = 0.0f;   // ~5–20 ms  (transient recovery)
    float m_releaseSlow      = 0.0f;   // ~80–200 ms (sustained program)

    // Envelope state
    float m_envelope         = 1.0f;   // current gain reduction (1.0 = no reduction)
    float m_envelopeSlow     = 1.0f;   // slow follower for adaptive release

    // Seamless bypass: crossfade between limited and dry-delayed signal
    std::atomic<bool>  m_enabled       { false };
    float m_bypassMix        = 0.0f;   // 0.0 = fully bypassed, 1.0 = fully active
    static constexpr int kCrossfadeSamples = 64;

    // ── Circular delay buffer ────────────────────────────────────────────
    // Layout: m_delayBuffer[channel][sample]
    // Pre-allocated in prepare(), never resized on audio thread.
    std::vector<std::vector<float>> m_delayBuffer;
    int m_delayWritePos = 0;
    int m_delaySize     = 0;           // total ring buffer length (power of 2)
    int m_delayMask     = 0;           // m_delaySize - 1 for fast modulo

    // ── True-peak 4× oversampling FIR state ──────────────────────────────
    // Half-band polyphase FIR for 4× interpolation (4 taps per phase, 4 phases).
    // State: last 4 input samples per channel for the FIR.
    mutable std::vector<std::array<float, 4>> m_truePeakHistory;

    // Pre-computed FIR coefficients for 4× polyphase interpolation.
    // 4 phases × 4 taps = 16 coefficients total.
    static constexpr int kOversampleFactor = 4;
    static constexpr int kFirTaps = 4;
    static constexpr float kFirCoeffs[kOversampleFactor][kFirTaps] = {
        // Phase 0 (original sample position): identity pass-through
        {  0.0f,       0.0f,       1.0f,       0.0f      },
        // Phase 1 (¼ sample offset)
        { -0.0625f,    0.5625f,    0.5625f,   -0.0625f   },
        // Phase 2 (½ sample offset): symmetric half-point
        { -0.0859375f, 0.5859375f, 0.5859375f, -0.0859375f },
        // Phase 3 (¾ sample offset): mirror of phase 1
        { -0.0625f,    0.5625f,    0.5625f,   -0.0625f   }
    };

    void updateCoefficients();
};
