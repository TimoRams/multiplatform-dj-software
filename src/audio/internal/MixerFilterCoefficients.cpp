#include "audio/internal/MixerFilterCoefficients.h"

#include <algorithm>
#include <numbers>

namespace {
BiquadCoefficients normalized(double b0,double b1,double b2,double a0,double a1,double a2) noexcept {
    return {static_cast<float>(b0/a0),static_cast<float>(b1/a0),static_cast<float>(b2/a0),static_cast<float>(a1/a0),static_cast<float>(a2/a0)};
}
double frequency(double hz,double sr) noexcept { return std::clamp(hz,20.0,sr*0.45); }
BiquadCoefficients pass(double sr,double hz,double q,bool high) noexcept {
    const double w=2*std::numbers::pi*frequency(hz,sr)/sr,c=std::cos(w),alpha=std::sin(w)/(2*q);
    if(high) return normalized((1+c)/2,-(1+c),(1+c)/2,1+alpha,-2*c,1-alpha);
    return normalized((1-c)/2,1-c,(1-c)/2,1+alpha,-2*c,1-alpha);
}
// 2nd-order allpass — the phase equivalent of an LR4 crossover at the same
// frequency, so the low band stays aligned with mid+high after the second split.
BiquadCoefficients allpass(double sr,double hz,double q) noexcept {
    const double w=2*std::numbers::pi*frequency(hz,sr)/sr,c=std::cos(w),alpha=std::sin(w)/(2*q);
    return normalized(1-alpha,-2*c,1+alpha,1+alpha,-2*c,1-alpha);
}
constexpr double kButterworthQ=0.70710678118654752; // LR4 = two cascaded Butterworth sections
}

// Boost tops out at +6 dB. Cut follows a −26 dB taper (hardware EQ range) and
// then fades to true silence over the last 8 % of travel so the knob still kills.
double mixerEqGainFromKnob(float knob) noexcept {
    const double v=std::clamp(static_cast<double>(knob),-1.0,1.0);
    if(v>=0.0)return std::pow(10.0,v*6.0/20.0);
    const double t=-v,gain=std::pow(10.0,t*-26.0/20.0);
    constexpr double killStart=0.92;
    if(t<=killStart)return gain;
    return gain*(1.0-(t-killStart)/(1.0-killStart));
}

bool BiquadCoefficients::finiteAndStable() const noexcept {
    if(!(std::isfinite(b0)&&std::isfinite(b1)&&std::isfinite(b2)&&std::isfinite(a1)&&std::isfinite(a2)))return false;
    const double d=static_cast<double>(a1)*a1-4.0*a2;
    if(d<0.0)return std::sqrt(std::abs(static_cast<double>(a2)))<1.0;
    const double root=std::sqrt(d);return std::abs((-a1+root)/2)<1.0&&std::abs((-a1-root)/2)<1.0;
}
bool MixerCoefficientSnapshot::valid() const noexcept {
    return sampleRate>0&&std::isfinite(sampleRate)
        &&lowSplitLp.finiteAndStable()&&lowSplitHp.finiteAndStable()
        &&midSplitLp.finiteAndStable()&&highSplitHp.finiteAndStable()
        &&lowAllpass.finiteAndStable()&&color.finiteAndStable()
        &&std::isfinite(lowGain)&&std::isfinite(midGain)&&std::isfinite(highGain);
}

MixerCoefficientSnapshot buildMixerCoefficientSnapshot(MixerFilterTargets t,double sr,std::uint64_t pg,std::uint64_t dg) noexcept {
    t.low=std::clamp(t.low,-1.0f,1.0f);t.mid=std::clamp(t.mid,-1.0f,1.0f);t.high=std::clamp(t.high,-1.0f,1.0f);t.color=std::clamp(t.color,-1.0f,1.0f);
    MixerCoefficientSnapshot s; s.sampleRate=sr;s.parameterGeneration=pg;s.deviceGeneration=dg;
    if(!(std::isfinite(sr)&&sr>=8000&&sr<=384000))return s;

    s.lowGain =static_cast<float>(mixerEqGainFromKnob(t.low));
    s.midGain =static_cast<float>(mixerEqGainFromKnob(t.mid));
    s.highGain=static_cast<float>(mixerEqGainFromKnob(t.high));
    // At the detent the split is skipped entirely: no crossover phase shift on a
    // channel whose EQ is untouched.
    s.eqBypass=std::abs(t.low)<0.005f&&std::abs(t.mid)<0.005f&&std::abs(t.high)<0.005f;
    if(!s.eqBypass){
        s.lowSplitLp =pass(sr,kEqCrossoverLowHz ,kButterworthQ,false);
        s.lowSplitHp =pass(sr,kEqCrossoverLowHz ,kButterworthQ,true);
        s.midSplitLp =pass(sr,kEqCrossoverHighHz,kButterworthQ,false);
        s.highSplitHp=pass(sr,kEqCrossoverHighHz,kButterworthQ,true);
        s.lowAllpass =allpass(sr,kEqCrossoverHighHz,kButterworthQ);
    }

    if(std::abs(t.color)<0.05f)s.color={};
    else if(t.color<0){const double x=1.0+t.color;s.color=pass(sr,80*std::pow(20000.0/80.0,x),1.2,false);}
    else s.color=pass(sr,20*std::pow(10000.0/20.0,t.color),1.2,true);
    return s;
}

float StereoBiquad::process(int channel,float input) noexcept { channel=std::clamp(channel,0,1);const float out=coefficients.b0*input+z1[channel];z1[channel]=coefficients.b1*input-coefficients.a1*out+z2[channel];z2[channel]=coefficients.b2*input-coefficients.a2*out;return out; }

void MixerFilterBank::setSnapshot(const MixerCoefficientSnapshot&s) noexcept {
    lowLp1.setCoefficients(s.lowSplitLp);lowLp2.setCoefficients(s.lowSplitLp);
    splitHp1.setCoefficients(s.lowSplitHp);splitHp2.setCoefficients(s.lowSplitHp);
    midLp1.setCoefficients(s.midSplitLp);midLp2.setCoefficients(s.midSplitLp);
    highHp1.setCoefficients(s.highSplitHp);highHp2.setCoefficients(s.highSplitHp);
    lowAp.setCoefficients(s.lowAllpass);color.setCoefficients(s.color);
    lowGain=s.lowGain;midGain=s.midGain;highGain=s.highGain;bypassEq=s.eqBypass;
}

void MixerFilterBank::clearState() noexcept {
    lowLp1.clearState();lowLp2.clearState();splitHp1.clearState();splitHp2.clearState();
    midLp1.clearState();midLp2.clearState();highHp1.clearState();highHp2.clearState();
    lowAp.clearState();color.clearState();
}

float MixerFilterBank::process(int ch,float v) noexcept{
    float out=v;
    if(!bypassEq){
        const float low =lowLp2 .process(ch,lowLp1  .process(ch,v));
        const float rest=splitHp2.process(ch,splitHp1.process(ch,v));
        const float mid =midLp2 .process(ch,midLp1  .process(ch,rest));
        const float high=highHp2.process(ch,highHp1 .process(ch,rest));
        out=lowGain*lowAp.process(ch,low)+midGain*mid+highGain*high;
    }
    return color.process(ch,out);
}
