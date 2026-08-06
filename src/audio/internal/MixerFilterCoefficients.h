#pragma once

#include <array>
#include <cmath>
#include <cstdint>

struct BiquadCoefficients {
    float b0=1.0f,b1=0.0f,b2=0.0f,a1=0.0f,a2=0.0f;
    [[nodiscard]] bool finiteAndStable() const noexcept;
};

struct MixerCoefficientSnapshot {
    BiquadCoefficients low,mid,high,color;
    std::uint64_t parameterGeneration=0,deviceGeneration=0;
    double sampleRate=0.0;
    [[nodiscard]] bool valid() const noexcept;
};

struct MixerFilterTargets { float low=0,mid=0,high=0,color=0; };

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
    StereoBiquad low,mid,high,color;
    void setSnapshot(const MixerCoefficientSnapshot& s) noexcept;
    void clearState() noexcept { low.clearState();mid.clearState();high.clearState();color.clearState(); }
    [[nodiscard]] float process(int channel,float input) noexcept;
};
