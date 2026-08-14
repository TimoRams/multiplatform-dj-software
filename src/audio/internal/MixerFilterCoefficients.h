#pragma once

#include <array>
#include <cmath>
#include <cstdint>

struct BiquadCoefficients {
    float b0=1.0f,b1=0.0f,b2=0.0f,a1=0.0f,a2=0.0f;
    [[nodiscard]] bool finiteAndStable() const noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// Channel EQ: 3-band Linkwitz–Riley crossover split (LR4, 24 dB/oct)
//
// Instead of shelf/bell filters (which overlap heavily and never fully remove a
// band), the signal is split into three non-overlapping bands that sum back to
// a flat response at unity gain — the behaviour expected from professional DJ
// hardware and the standard software mixers: killing MID removes the whole
// midrange, not just a bell around its centre frequency.
//
//   low  = LP4(f1) · AP2(f2)      (allpass keeps the low band phase-aligned)
//   rest = HP4(f1)
//   mid  = LP4(f2) of rest
//   high = HP4(f2) of rest
//   out  = gLow·low + gMid·mid + gHigh·high
//
// LR4 sections sum in phase, so low+mid+high reconstructs magnitude-flat.
// ─────────────────────────────────────────────────────────────────────────────
struct MixerCoefficientSnapshot {
    // Band-split sections. Each LR4 stage is the same biquad applied twice.
    BiquadCoefficients lowSplitLp,   // LP @ f1  (×2)
                       lowSplitHp,   // HP @ f1  (×2)
                       midSplitLp,   // LP @ f2  (×2)
                       highSplitHp,  // HP @ f2  (×2)
                       lowAllpass,   // AP2 @ f2 — phase match for the low band
                       color;        // mixer filter knob (LPF/HPF)
    float lowGain=1.0f,midGain=1.0f,highGain=1.0f;
    bool eqBypass=true;              // all three knobs at detent → skip the split
    std::uint64_t parameterGeneration=0,deviceGeneration=0;
    double sampleRate=0.0;
    [[nodiscard]] bool valid() const noexcept;
};

struct MixerFilterTargets { float low=0,mid=0,high=0,color=0; };

// Crossover points of the 3-band split. Low/mid at 300 Hz, mid/high at 4 kHz —
// the classic bass / midrange / treble division used by professional DJ mixers.
inline constexpr double kEqCrossoverLowHz  = 300.0;
inline constexpr double kEqCrossoverHighHz = 4000.0;

// Knob → gain law. 0 = unity, +1 = +6 dB, −1 = full kill (−∞); the last stretch
// of downward travel fades from −26 dB to silence like a hardware EQ.
[[nodiscard]] double mixerEqGainFromKnob(float knob) noexcept;

[[nodiscard]] MixerCoefficientSnapshot buildMixerCoefficientSnapshot(
    MixerFilterTargets targets,double sampleRate,std::uint64_t parameterGeneration,
    std::uint64_t deviceGeneration) noexcept;

class StereoBiquad {
public:
    void setCoefficients(BiquadCoefficients c) noexcept { coefficients=c; }
    void clearState() noexcept { z1[0]=z1[1]=z2[0]=z2[1]=0.0f; }
    [[nodiscard]] float process(int channel,float input) noexcept;
private:
    BiquadCoefficients coefficients;
    float z1[2]{},z2[2]{};
};

struct MixerFilterBank {
    StereoBiquad lowLp1,lowLp2,splitHp1,splitHp2,midLp1,midLp2,highHp1,highHp2,lowAp,color;
    float lowGain=1.0f,midGain=1.0f,highGain=1.0f;
    bool bypassEq=true;
    void setSnapshot(const MixerCoefficientSnapshot& s) noexcept;
    void clearState() noexcept;
    [[nodiscard]] float process(int channel,float input) noexcept;
};
