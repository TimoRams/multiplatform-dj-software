#pragma once

#include <array>
#include <cmath>
#include <cstdint>

struct BiquadCoefficients {
    float b0=1.0f,b1=0.0f,b2=0.0f,a1=0.0f,a2=0.0f;
    [[nodiscard]] bool finiteAndStable() const noexcept;
};

// Channel EQ: low/high shelving filters with a broad mid bell. This musical,
// overlapping topology gives each channel a -26 dB to +6 dB range without an
// isolator-style full-band kill.
struct MixerCoefficientSnapshot {
    BiquadCoefficients lowShelf, midBell, highShelf, color;
    bool eqBypass=true;              // all three knobs at detent → skip the EQ
    std::uint64_t parameterGeneration=0,deviceGeneration=0;
    double sampleRate=0.0;
    [[nodiscard]] bool valid() const noexcept;
};

struct MixerFilterTargets { float low=0,mid=0,high=0,color=0; };

inline constexpr double kEqLowShelfHz = 300.0;
inline constexpr double kEqMidBellHz = 1000.0;
inline constexpr double kEqHighShelfHz = 4000.0;
inline constexpr double kEqMidBellQ = 0.7;

// Knob → gain law. 0 = unity, +1 = +6 dB, −1 = -26 dB.
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
    StereoBiquad lowShelf,midBell,highShelf,color;
    bool bypassEq=true;
    void setSnapshot(const MixerCoefficientSnapshot& s) noexcept;
    void clearState() noexcept;
    [[nodiscard]] float process(int channel,float input) noexcept;
};
