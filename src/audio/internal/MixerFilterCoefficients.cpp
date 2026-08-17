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
BiquadCoefficients shelf(double sr,double hz,double decibels,bool high) noexcept {
    const double amplitude=std::pow(10.0,decibels/40.0);
    const double w=2*std::numbers::pi*frequency(hz,sr)/sr;
    const double cosine=std::cos(w);
    const double alpha=std::sin(w)*std::numbers::sqrt2/2.0;
    const double beta=2.0*std::sqrt(amplitude)*alpha;
    if(high) {
        return normalized(
            amplitude*((amplitude+1.0)+(amplitude-1.0)*cosine+beta),
            -2.0*amplitude*((amplitude-1.0)+(amplitude+1.0)*cosine),
            amplitude*((amplitude+1.0)+(amplitude-1.0)*cosine-beta),
            (amplitude+1.0)-(amplitude-1.0)*cosine+beta,
            2.0*((amplitude-1.0)-(amplitude+1.0)*cosine),
            (amplitude+1.0)-(amplitude-1.0)*cosine-beta);
    }
    return normalized(
        amplitude*((amplitude+1.0)-(amplitude-1.0)*cosine+beta),
        2.0*amplitude*((amplitude-1.0)-(amplitude+1.0)*cosine),
        amplitude*((amplitude+1.0)-(amplitude-1.0)*cosine-beta),
        (amplitude+1.0)+(amplitude-1.0)*cosine+beta,
        -2.0*((amplitude-1.0)+(amplitude+1.0)*cosine),
        (amplitude+1.0)+(amplitude-1.0)*cosine-beta);
}
BiquadCoefficients bell(double sr,double hz,double q,double decibels) noexcept {
    const double amplitude=std::pow(10.0,decibels/40.0);
    const double w=2*std::numbers::pi*frequency(hz,sr)/sr;
    const double cosine=std::cos(w);
    const double alpha=std::sin(w)/(2.0*q);
    return normalized(
        1.0+alpha*amplitude,
        -2.0*cosine,
        1.0-alpha*amplitude,
        1.0+alpha/amplitude,
        -2.0*cosine,
        1.0-alpha/amplitude);
}
double eqDecibels(float knob) noexcept {
    const double value=std::clamp(static_cast<double>(knob),-1.0,1.0);
    return value>=0.0 ? value*6.0 : value*26.0;
}
}

double mixerEqGainFromKnob(float knob) noexcept {
    return std::pow(10.0,eqDecibels(knob)/20.0);
}

bool BiquadCoefficients::finiteAndStable() const noexcept {
    if(!(std::isfinite(b0)&&std::isfinite(b1)&&std::isfinite(b2)&&std::isfinite(a1)&&std::isfinite(a2)))return false;
    const double d=static_cast<double>(a1)*a1-4.0*a2;
    if(d<0.0)return std::sqrt(std::abs(static_cast<double>(a2)))<1.0;
    const double root=std::sqrt(d);return std::abs((-a1+root)/2)<1.0&&std::abs((-a1-root)/2)<1.0;
}
bool MixerCoefficientSnapshot::valid() const noexcept {
    return sampleRate>0&&std::isfinite(sampleRate)
        &&lowShelf.finiteAndStable()&&midBell.finiteAndStable()
        &&highShelf.finiteAndStable()&&color.finiteAndStable();
}

MixerCoefficientSnapshot buildMixerCoefficientSnapshot(MixerFilterTargets t,double sr,std::uint64_t pg,std::uint64_t dg) noexcept {
    t.low=std::clamp(t.low,-1.0f,1.0f);t.mid=std::clamp(t.mid,-1.0f,1.0f);t.high=std::clamp(t.high,-1.0f,1.0f);t.color=std::clamp(t.color,-1.0f,1.0f);
    MixerCoefficientSnapshot s; s.sampleRate=sr;s.parameterGeneration=pg;s.deviceGeneration=dg;
    if(!(std::isfinite(sr)&&sr>=8000&&sr<=384000))return s;

    // At the detent the EQ is skipped entirely on a channel whose EQ is untouched.
    s.eqBypass=std::abs(t.low)<0.005f&&std::abs(t.mid)<0.005f&&std::abs(t.high)<0.005f;
    if(!s.eqBypass){
        s.lowShelf=shelf(sr,kEqLowShelfHz,eqDecibels(t.low),false);
        s.midBell=bell(sr,kEqMidBellHz,kEqMidBellQ,eqDecibels(t.mid));
        s.highShelf=shelf(sr,kEqHighShelfHz,eqDecibels(t.high),true);
    }

    if(std::abs(t.color)<0.05f)s.color={};
    else if(t.color<0){const double x=1.0+t.color;s.color=pass(sr,80*std::pow(20000.0/80.0,x),1.2,false);}
    else s.color=pass(sr,20*std::pow(10000.0/20.0,t.color),1.2,true);
    return s;
}

float StereoBiquad::process(int channel,float input) noexcept { channel=std::clamp(channel,0,1);const float out=coefficients.b0*input+z1[channel];z1[channel]=coefficients.b1*input-coefficients.a1*out+z2[channel];z2[channel]=coefficients.b2*input-coefficients.a2*out;return out; }

void MixerFilterBank::setSnapshot(const MixerCoefficientSnapshot&s) noexcept {
    lowShelf.setCoefficients(s.lowShelf);midBell.setCoefficients(s.midBell);
    highShelf.setCoefficients(s.highShelf);color.setCoefficients(s.color);
    bypassEq=s.eqBypass;
}

void MixerFilterBank::clearState() noexcept {
    lowShelf.clearState();midBell.clearState();highShelf.clearState();color.clearState();
}

float MixerFilterBank::process(int ch,float v) noexcept{
    float out=v;
    if(!bypassEq){
        out=lowShelf.process(ch,out);
        out=midBell.process(ch,out);
        out=highShelf.process(ch,out);
    }
    return color.process(ch,out);
}
