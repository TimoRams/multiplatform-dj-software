#include "engine/audio/MixerDspSource.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <atomic>

namespace {
bool require(bool v,const char*m){if(!v)std::cerr<<"FAIL: "<<m<<'\n';return v;}
class Sine final:public juce::AudioSource{public:explicit Sine(double f):freq(f){}void prepareToPlay(int,double r)override{rate=r;}void releaseResources()override{}void getNextAudioBlock(const juce::AudioSourceChannelInfo&i)override{for(int n=0;n<i.numSamples;++n){const float v=static_cast<float>(0.2*std::sin(phase));phase+=juce::MathConstants<double>::twoPi*freq/rate;for(int c=0;c<i.buffer->getNumChannels();++c)i.buffer->setSample(c,i.startSample+n,v);}}private:double freq,rate=48000,phase=0;};
bool finite(const juce::AudioBuffer<float>&b){for(int c=0;c<b.getNumChannels();++c)for(int i=0;i<b.getNumSamples();++i)if(!std::isfinite(b.getSample(c,i)))return false;return true;}
float render(MixerDspSource&m,int size){juce::AudioBuffer<float>b(2,size);m.getNextAudioBlock({&b,0,size});return b.getMagnitude(0,0,size);}
}
int main(){bool ok=true;
 for(double sr:{44100.0,48000.0,96000.0,192000.0})for(auto t:{MixerFilterTargets{},MixerFilterTargets{-1,0,0,-1},MixerFilterTargets{0,-1,0,1},MixerFilterTargets{1,1,1,0}}){auto s=buildMixerCoefficientSnapshot(t,sr,1,1);ok&=require(s.valid(),"finite stable coefficients");}
 Sine sine(8000);auto mixer=std::make_unique<MixerDspSource>(&sine);mixer->prepareToPlay(8192,48000);
 for(int size:{64,128,256,512,1024,2048,4096,8192}){const float p=render(*mixer,size);ok&=require(std::isfinite(p)&&p>0,"all block sizes audible finite");}
 const float flat=render(*mixer,2048);mixer->setFilterVal(-1);for(int i=0;i<4;++i)render(*mixer,2048);const float lowPass=render(*mixer,2048);ok&=require(lowPass<flat*0.7f,"low pass attenuates high tone");
 std::mt19937 rng(0xD5F00Du);for(int i=0;i<2000;++i){mixer->setEq((rng()%2001-1000)/1000.f,(rng()%2001-1000)/1000.f,(rng()%2001-1000)/1000.f);mixer->setFilterVal((rng()%2001-1000)/1000.f);mixer->setTrim((rng()%1001)/1000.f);mixer->setFader((rng()%1001)/1000.f);const int sizes[]={64,128,256,512,1024,2048,4096,8192};const float p=render(*mixer,sizes[rng()%8]);ok&=require(std::isfinite(p)&&p<8,"stress finite bounded");}
 const auto stats=mixer->realtimeStats();ok&=require(stats.coefficientBuildsFromAudioThread==0,"no coefficient builds in callback");ok&=require(stats.prepareCallsFromAudioThread==0,"no prepare in callback");ok&=require(stats.bufferGrowthsFromAudioThread==0,"no growth in callback");ok&=require(stats.blockingLockAttempts==0,"no locks in callback");ok&=require(stats.objectConstructionsFromAudioThread==0,"no object construction in callback");
 {Sine concurrentTone(1000);auto concurrentMixer=std::make_unique<MixerDspSource>(&concurrentTone);concurrentMixer->prepareToPlay(8192,48000);std::atomic<bool>go{false};std::thread control([&]{while(!go.load(std::memory_order_acquire)){}for(int i=0;i<2000;++i){const float v=(i%201-100)/100.f;concurrentMixer->setEq(v,-v,v*0.5f);concurrentMixer->setFilterVal(-v);}});go.store(true,std::memory_order_release);for(int i=0;i<1000;++i)ok&=require(std::isfinite(render(*concurrentMixer,64<<(i%8))),"concurrent finite");control.join();const auto s=concurrentMixer->realtimeStats();ok&=require(s.coefficientBuildsFromAudioThread+s.prepareCallsFromAudioThread+s.bufferGrowthsFromAudioThread+s.blockingLockAttempts+s.objectConstructionsFromAudioThread==0,"concurrent realtime counters zero");}
 if(std::getenv("BROCKDJ_MIXER_BENCHMARK")){double total=0,worst=0;for(int i=0;i<1000;++i){const auto s=std::chrono::steady_clock::now();render(*mixer,512);const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-s).count();total+=us;worst=std::max(worst,us);}std::cout<<"mixer 512 mean-us="<<total/1000<<" worst-us="<<worst<<'\n';}
 return ok?0:1;}
