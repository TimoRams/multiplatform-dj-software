#include "audio/TimeStretchProcessor.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>
#include <cstdlib>

namespace {
class ToneSource final : public juce::AudioSource {
public:
    void prepareToPlay(int, double rate) override { sampleRate = rate; }
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        for (int i=0;i<info.numSamples;++i) {
            const float value=static_cast<float>(0.2*std::sin(phase));
            phase += 2.0*juce::MathConstants<double>::pi*220.0/sampleRate;
            for(int ch=0;ch<info.buffer->getNumChannels();++ch) info.buffer->setSample(ch,info.startSample+i,value);
        }
    }
private: double sampleRate=48000.0, phase=0.0;
};
bool require(bool value,const char* message){if(!value)std::cerr<<"FAIL: "<<message<<'\n';return value;}
bool finite(const juce::AudioBuffer<float>& b){for(int ch=0;ch<b.getNumChannels();++ch)for(int i=0;i<b.getNumSamples();++i)if(!std::isfinite(b.getSample(ch,i)))return false;return true;}
bool waitForGeneration(TimeStretchProcessor& source,std::uint64_t oldGeneration){
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    juce::AudioBuffer<float> b(2,256);
    while(std::chrono::steady_clock::now()<until){source.getNextAudioBlock({&b,0,256});if(source.activeConfigurationGeneration()>oldGeneration)return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    return false;
}
}

static_assert(noexcept(std::declval<TimeStretchProcessor&>().getNextAudioBlock(
    std::declval<const juce::AudioSourceChannelInfo&>())));

int main(){
    bool ok=true;
    { ToneSource tone; TimeStretchProcessor source(&tone); source.prepareToPlay(512, 48000.0);
        source.setPitchLockEnabled(true);
        ok &= require(waitForGeneration(source, source.activeConfigurationGeneration()),
                      "Signalsmith is the default keylock backend");
        ok &= require(source.activeBackend() == TimeStretchBackend::Signalsmith,
                      "Signalsmith is active by default");
        const auto signalsmithGeneration = source.activeConfigurationGeneration();
        source.setBackend(TimeStretchBackend::RubberBand);
        ok &= require(waitForGeneration(source, signalsmithGeneration),
                      "Rubber Band backend switches through prepared pipeline");
        ok &= require(source.activeBackend() == TimeStretchBackend::RubberBand,
                      "Rubber Band becomes active after switching");
        source.setBackend(TimeStretchBackend::Signalsmith);
        ok &= require(waitForGeneration(source, source.activeConfigurationGeneration()),
                      "Signalsmith backend switches back through prepared pipeline");
        ok &= require(source.activeBackend() == TimeStretchBackend::Signalsmith,
                      "Signalsmith becomes active after switching back");
    }
    for(double rate:{44100.0,48000.0,96000.0,192000.0}){
        ToneSource tone; TimeStretchProcessor source(&tone); source.prepareToPlay(8192,rate);
        for(int size:{64,128,256,512,1024,2048,4096,8192}){juce::AudioBuffer<float>b(2,size);source.getNextAudioBlock({&b,0,size});ok&=require(finite(b),"bypass finite");}
        { juce::AudioBuffer<float> mono(1,512); source.getNextAudioBlock({&mono,0,512}); ok&=require(finite(mono),"mono output finite"); }
        auto generation=source.activeConfigurationGeneration();
        source.setPitchLockEnabled(true); source.setTempoRatio(0.7);
        ok&=require(waitForGeneration(source,generation),"prepared keylock pipeline activates");
        juce::AudioBuffer<float>b(2,8192);source.getNextAudioBlock({&b,0,8192});ok&=require(finite(b),"stretched finite");
        generation=source.activeConfigurationGeneration();
        const auto switchesBeforeNudge=source.realtimeStats().successfulPipelineSwitches;
        for(int i=0;i<300;++i){
            source.setTempoRatio(0.94+0.12*(i%2));
            juce::AudioBuffer<float> nudgeBlock(2,(i%2)==0?64:8192);
            source.getNextAudioBlock({&nudgeBlock,0,nudgeBlock.getNumSamples()});
            ok&=require(finite(nudgeBlock),"keylock nudge output finite");
        }
        source.setTempoRatio(1.0);
        source.getNextAudioBlock({&b,0,8192});
        ok&=require(source.activeConfigurationGeneration()==generation,
                    "keylock nudge does not rebuild a pipeline");
        ok&=require(source.realtimeStats().successfulPipelineSwitches==switchesBeforeNudge,
                    "keylock nudge does not switch a pipeline");
        const auto preScratchGeneration=source.activeConfigurationGeneration();
        source.enterScratchBypass();source.getNextAudioBlock({&b,0,8192});
        ok&=require(waitForGeneration(source,preScratchGeneration),
                    "scratch prepares a clean keylock pipeline");
        const auto cleanScratchGeneration=source.activeConfigurationGeneration();
        const auto switchesAfterCleanScratch=source.realtimeStats().successfulPipelineSwitches;
        source.enterScratchBypass();
        for(int i=0;i<16;++i){source.getNextAudioBlock({&b,0,8192});std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        ok&=require(source.activeConfigurationGeneration()==cleanScratchGeneration
                        && source.realtimeStats().successfulPipelineSwitches==switchesAfterCleanScratch,
                    "regrab during scratch bypass does not rebuild Rubber Band again");
        source.endScratchBypass();source.getNextAudioBlock({&b,0,8192});
        ok&=require(finite(b),"scratch transition finite");
        auto stats=source.realtimeStats();
        ok&=require(stats.prepareCallsFromAudioThread==0,"no prepare in callback");
        ok&=require(stats.resetCallsFromAudioThread==0,"no reset in callback");
        ok&=require(stats.prewarmCallsFromAudioThread==0,"no prewarm in callback");
        ok&=require(stats.bufferGrowthsFromAudioThread==0,"no buffer growth in callback");
        ok&=require(stats.blockingLockAttempts==0,"no callback locks");
        source.releaseResources();
    }
    std::mt19937 rng(0xB40CD5u); ToneSource tone; TimeStretchProcessor stress(&tone);stress.prepareToPlay(8192,48000);
    for(int i=0;i<1000;++i){if(i%7==0)stress.setPitchLockEnabled((i/7)%2);stress.setTempoRatio(0.25+(rng()%700)/100.0);if(i%31==0)stress.enterScratchBypass();if(i%31==2)stress.endScratchBypass();const int sizes[]={64,128,256,512,1024,2048,4096,8192};juce::AudioBuffer<float>b(2,sizes[rng()%8]);stress.getNextAudioBlock({&b,0,b.getNumSamples()});ok&=require(finite(b),"stress finite");}
    const auto stats=stress.realtimeStats();ok&=require(stats.prepareCallsFromAudioThread+stats.resetCallsFromAudioThread+stats.prewarmCallsFromAudioThread+stats.bufferGrowthsFromAudioThread+stats.blockingLockAttempts==0,"all realtime counters zero");
    if(std::getenv("BROCKDJ_TIME_STRETCH_BENCHMARK")){
        juce::AudioBuffer<float>b(2,512);double total=0.0,worst=0.0;
        for(int i=0;i<1000;++i){const auto start=std::chrono::steady_clock::now();stress.getNextAudioBlock({&b,0,512});const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();total+=us;worst=std::max(worst,us);}
        std::cout<<"time-stretch 512-block mean-us="<<total/1000.0<<" worst-us="<<worst<<'\n';
    }
    return ok?0:1;
}
