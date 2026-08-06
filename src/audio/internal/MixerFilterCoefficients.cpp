#include "audio/internal/MixerFilterCoefficients.h"

#include <algorithm>
#include <numbers>

namespace {
BiquadCoefficients normalized(double b0,double b1,double b2,double a0,double a1,double a2) noexcept {
    return {static_cast<float>(b0/a0),static_cast<float>(b1/a0),static_cast<float>(b2/a0),static_cast<float>(a1/a0),static_cast<float>(a2/a0)};
}
double frequency(double hz,double sr) noexcept { return std::clamp(hz,20.0,sr*0.49); }
BiquadCoefficients peak(double sr,double hz,double q,double gain) noexcept {
    const double A=std::sqrt(gain),w=2*std::numbers::pi*frequency(hz,sr)/sr,c=std::cos(w),alpha=std::sin(w)/(2*q);
    return normalized(1+alpha*A,-2*c,1-alpha*A,1+alpha/A,-2*c,1-alpha/A);
}
BiquadCoefficients shelf(double sr,double hz,double q,double gain,bool high) noexcept {
    const double A=std::sqrt(gain),w=2*std::numbers::pi*frequency(hz,sr)/sr,c=std::cos(w),s=std::sin(w);
    const double alpha=s/(2*q),root=2*std::sqrt(A)*alpha;
    if(high) return normalized(A*((A+1)+(A-1)*c+root),-2*A*((A-1)+(A+1)*c),A*((A+1)+(A-1)*c-root),(A+1)-(A-1)*c+root,2*((A-1)-(A+1)*c),(A+1)-(A-1)*c-root);
    return normalized(A*((A+1)-(A-1)*c+root),2*A*((A-1)-(A+1)*c),A*((A+1)-(A-1)*c-root),(A+1)+(A-1)*c+root,-2*((A-1)+(A+1)*c),(A+1)+(A-1)*c-root);
}
BiquadCoefficients pass(double sr,double hz,double q,bool high) noexcept {
    const double w=2*std::numbers::pi*frequency(hz,sr)/sr,c=std::cos(w),alpha=std::sin(w)/(2*q);
    if(high) return normalized((1+c)/2,-(1+c),(1+c)/2,1+alpha,-2*c,1-alpha);
    return normalized((1-c)/2,1-c,(1-c)/2,1+alpha,-2*c,1-alpha);
}
double gainFromKnob(float value) noexcept { const double db=value<0?value*32.0:value*6.0; return std::pow(10.0,db/20.0); }
}

bool BiquadCoefficients::finiteAndStable() const noexcept {
    if(!(std::isfinite(b0)&&std::isfinite(b1)&&std::isfinite(b2)&&std::isfinite(a1)&&std::isfinite(a2)))return false;
    const double d=static_cast<double>(a1)*a1-4.0*a2;
    if(d<0.0)return std::sqrt(std::abs(static_cast<double>(a2)))<1.0;
    const double root=std::sqrt(d);return std::abs((-a1+root)/2)<1.0&&std::abs((-a1-root)/2)<1.0;
}
bool MixerCoefficientSnapshot::valid() const noexcept { return sampleRate>0&&std::isfinite(sampleRate)&&low.finiteAndStable()&&mid.finiteAndStable()&&high.finiteAndStable()&&color.finiteAndStable(); }

MixerCoefficientSnapshot buildMixerCoefficientSnapshot(MixerFilterTargets t,double sr,std::uint64_t pg,std::uint64_t dg) noexcept {
    t.low=std::clamp(t.low,-1.0f,1.0f);t.mid=std::clamp(t.mid,-1.0f,1.0f);t.high=std::clamp(t.high,-1.0f,1.0f);t.color=std::clamp(t.color,-1.0f,1.0f);
    MixerCoefficientSnapshot s; s.sampleRate=sr;s.parameterGeneration=pg;s.deviceGeneration=dg;
    if(!(std::isfinite(sr)&&sr>=8000&&sr<=384000))return s;
    s.low=shelf(sr,250,0.707,gainFromKnob(t.low),false);s.mid=peak(sr,1000,0.707,gainFromKnob(t.mid));s.high=shelf(sr,2500,0.707,gainFromKnob(t.high),true);
    if(std::abs(t.color)<0.05f)s.color={};
    else if(t.color<0){const double x=1.0+t.color;s.color=pass(sr,80*std::pow(20000.0/80.0,x),1.2,false);}
    else s.color=pass(sr,20*std::pow(10000.0/20.0,t.color),1.2,true);
    return s;
}

float StereoBiquad::process(int channel,float input) noexcept { channel=std::clamp(channel,0,1);const float out=coefficients.b0*input+z1[channel];z1[channel]=coefficients.b1*input-coefficients.a1*out+z2[channel];z2[channel]=coefficients.b2*input-coefficients.a2*out;return out; }
void MixerFilterBank::setSnapshot(const MixerCoefficientSnapshot&s) noexcept {low.setCoefficients(s.low);mid.setCoefficients(s.mid);high.setCoefficients(s.high);color.setCoefficients(s.color);}
float MixerFilterBank::process(int ch,float v) noexcept{return color.process(ch,high.process(ch,mid.process(ch,low.process(ch,v))));}
