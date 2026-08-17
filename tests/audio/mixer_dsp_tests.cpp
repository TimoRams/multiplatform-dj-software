#include "audio/DeckChannelProcessor.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <random>
#include <atomic>
#include <thread>

namespace {
bool require(bool v,const char*m){if(!v)std::cerr<<"FAIL: "<<m<<'\n';return v;}
class Sine final : public juce::AudioSource {
public:
    explicit Sine(double frequency) : freq(frequency) {}
    void prepareToPlay(int, double newRate) override { rate = newRate; }
    void releaseResources() override {}
    void setAmplitude(float value) noexcept { amplitude.store(value); }
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        const float gain = amplitude.load();
        for (int n = 0; n < info.numSamples; ++n) {
            const float value = static_cast<float>(gain * std::sin(phase));
            phase += juce::MathConstants<double>::twoPi * freq / rate;
            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                info.buffer->setSample(c, info.startSample + n, value);
        }
    }
private:
    double freq;
    double rate = 48000.0;
    double phase = 0.0;
    std::atomic<float> amplitude { 0.2f };
};
bool finite(const juce::AudioBuffer<float>&b){for(int c=0;c<b.getNumChannels();++c)for(int i=0;i<b.getNumSamples();++i)if(!std::isfinite(b.getSample(c,i)))return false;return true;}
float render(DeckChannelProcessor&m,int size){juce::AudioBuffer<float>b(2,size);m.getNextAudioBlock({&b,0,size});return b.getMagnitude(0,0,size);}
// Steady-state gain of the EQ at one frequency, in dB relative to the input.
double eqResponseDb(MixerFilterTargets t,double hz,double sr=48000.0){
 auto s=buildMixerCoefficientSnapshot(t,sr,1,1);MixerFilterBank bank;bank.setSnapshot(s);bank.clearState();
 // RMS rather than peak: a few samples per cycle at high frequencies never
 // land on the crest of the wave, which would read as several dB of loss.
 const int warm=static_cast<int>(sr),n=static_cast<int>(sr);double phase=0,sum=0;
 for(int i=0;i<warm+n;++i){const float x=static_cast<float>(std::sin(phase));phase+=juce::MathConstants<double>::twoPi*hz/sr;const float y=bank.process(0,x);if(i>=warm)sum+=static_cast<double>(y)*y;}
 return 20.0*std::log10(std::max(std::sqrt(sum/n)*std::numbers::sqrt2,1e-9));}
}
int main(){bool ok=true;
 for(double sr:{44100.0,48000.0,96000.0,192000.0})for(auto t:{MixerFilterTargets{},MixerFilterTargets{-1,0,0,-1},MixerFilterTargets{0,-1,0,1},MixerFilterTargets{1,1,1,0}}){auto s=buildMixerCoefficientSnapshot(t,sr,1,1);ok&=require(s.valid(),"finite stable coefficients");}
 // Musical channel EQ uses overlapping shelves and a broad mid bell; each band
 // spans -26 dB through +6 dB and never becomes an isolator-style full kill.
 {const MixerFilterTargets cut{-0.5f,-0.5f,-0.5f,0};
  for(double hz:{50.0,1000.0,12000.0}){const double db=eqResponseDb(cut,hz);ok&=require(std::abs(db+13.0)<1.0,"half cut is -13 dB at every EQ band");}
  ok&=require(std::abs(eqResponseDb({-1,0,0,0},50.0)+26.0)<0.8,"low shelf reaches -26 dB");
  ok&=require(eqResponseDb({-1,0,0,0},2000.0)>-1.0,"low shelf leaves the midrange intact");
  ok&=require(std::abs(eqResponseDb({0,-1,0,0},1000.0)+26.0)<0.5,"mid bell reaches -26 dB");
  ok&=require(eqResponseDb({0,-1,0,0},50.0)>-1.0&&eqResponseDb({0,-1,0,0},12000.0)>-1.0,"mid bell leaves bass and treble intact");
  ok&=require(std::abs(eqResponseDb({0,0,-1,0},12000.0)+26.0)<1.0,"high shelf reaches -26 dB");
  ok&=require(eqResponseDb({0,0,-1,0},500.0)>-1.0,"high shelf leaves the midrange intact");
  ok&=require(std::abs(eqResponseDb({1,0,0,0},50.0)-6.0)<0.5,"low shelf reaches +6 dB");
  ok&=require(std::abs(eqResponseDb({0,1,0,0},1000.0)-6.0)<0.5,"mid bell reaches +6 dB");
  ok&=require(std::abs(eqResponseDb({0,0,1,0},12000.0)-6.0)<0.8,"high shelf reaches +6 dB");
  ok&=require(std::abs(20.0*std::log10(mixerEqGainFromKnob(-1.f))+26.0)<0.01,"knob at -1 is -26 dB");
  ok&=require(std::abs(20.0*std::log10(mixerEqGainFromKnob(1.f))-6.0)<0.01,"knob at +1 is +6 dB");
  ok&=require(std::abs(20.0*std::log10(mixerEqGainFromKnob(-0.5f))+13.0)<0.01,"half cut is -13 dB");}
 Sine sine(8000);auto mixer=std::make_unique<DeckChannelProcessor>(&sine);mixer->prepareToPlay(8192,48000);
 for(int size:{64,128,256,512,1024,2048,4096,8192}){const float p=render(*mixer,size);ok&=require(std::isfinite(p)&&p>0,"all block sizes audible finite");}
 const float flat=render(*mixer,2048);mixer->setFilterVal(-1);for(int i=0;i<4;++i)render(*mixer,2048);const float lowPass=render(*mixer,2048);ok&=require(lowPass<flat*0.7f,"low pass attenuates high tone");
 {Sine meterTone(1000);auto meterMixer=std::make_unique<DeckChannelProcessor>(&meterTone);meterMixer->prepareToPlay(1024,48000);for(int i=0;i<3;++i)render(*meterMixer,1024);const float openPre=meterMixer->m_preFaderPeakL.load();const float openPost=meterMixer->m_peakL.load();meterMixer->setFader(0.0f);for(int i=0;i<3;++i)render(*meterMixer,1024);const float closedPre=meterMixer->m_preFaderPeakL.load();const float closedPost=meterMixer->m_peakL.load();ok&=require(openPre>0.1f&&openPost>0.1f,"meter has signal with open fader");ok&=require(closedPre>0.1f,"pre-fader meter ignores closed channel fader");ok&=require(closedPost<0.001f,"post-fader meter follows closed channel fader");}
 std::mt19937 rng(0xD5F00Du);for(int i=0;i<2000;++i){mixer->setEq((rng()%2001-1000)/1000.f,(rng()%2001-1000)/1000.f,(rng()%2001-1000)/1000.f);mixer->setFilterVal((rng()%2001-1000)/1000.f);mixer->setTrim((rng()%1001)/1000.f);mixer->setFader((rng()%1001)/1000.f);const int sizes[]={64,128,256,512,1024,2048,4096,8192};const float p=render(*mixer,sizes[rng()%8]);ok&=require(std::isfinite(p)&&p<8,"stress finite bounded");}
 const auto stats=mixer->realtimeStats();ok&=require(stats.coefficientBuildsFromAudioThread==0,"no coefficient builds in callback");ok&=require(stats.prepareCallsFromAudioThread==0,"no prepare in callback");ok&=require(stats.bufferGrowthsFromAudioThread==0,"no growth in callback");ok&=require(stats.blockingLockAttempts==0,"no locks in callback");ok&=require(stats.objectConstructionsFromAudioThread==0,"no object construction in callback");
 {
     Sine tailTone(900);
     auto tailMixer = std::make_unique<DeckChannelProcessor>(&tailTone);
     tailMixer->prepareToPlay(512, 48000);
     tailMixer->setFxSlotEffectType(1, EffectType::Echo);
     tailMixer->setFxSlotAmount(1, 1.0f);
     tailMixer->setFxSlotExternalDelayTime(1, 0.005f);
     for (int i = 0; i < 8; ++i)
         render(*tailMixer, 512);

     tailTone.setAmplitude(0.0f);
     tailMixer->setFader(0.0f);
     render(*tailMixer, 512);
     ok &= require(tailMixer->getPostFaderTailBuffer().getMagnitude(0, 0, 512) > 0.0001f,
                   "post-fader echo tail survives silent/reset deck input");
 }
 {Sine concurrentTone(1000);auto concurrentMixer=std::make_unique<DeckChannelProcessor>(&concurrentTone);concurrentMixer->prepareToPlay(8192,48000);std::atomic<bool>go{false};std::thread control([&]{while(!go.load(std::memory_order_acquire)){}for(int i=0;i<2000;++i){const float v=(i%201-100)/100.f;concurrentMixer->setEq(v,-v,v*0.5f);concurrentMixer->setFilterVal(-v);}});go.store(true,std::memory_order_release);for(int i=0;i<1000;++i)ok&=require(std::isfinite(render(*concurrentMixer,64<<(i%8))),"concurrent finite");control.join();const auto s=concurrentMixer->realtimeStats();ok&=require(s.coefficientBuildsFromAudioThread+s.prepareCallsFromAudioThread+s.bufferGrowthsFromAudioThread+s.blockingLockAttempts+s.objectConstructionsFromAudioThread==0,"concurrent realtime counters zero");}
 if(std::getenv("BROCKDJ_MIXER_BENCHMARK")){double total=0,worst=0;for(int i=0;i<1000;++i){const auto s=std::chrono::steady_clock::now();render(*mixer,512);const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-s).count();total+=us;worst=std::max(worst,us);}std::cout<<"mixer 512 mean-us="<<total/1000<<" worst-us="<<worst<<'\n';}
 return ok?0:1;}
