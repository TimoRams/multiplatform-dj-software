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
        juce::AudioBuffer<float> settle(2,256);
        source.getNextAudioBlock({&settle,0,256});
        ok &= require(source.activeBackend() == TimeStretchBackend::Signalsmith,
                      "Signalsmith is active by default");
        const auto keylockGeneration = source.activeConfigurationGeneration();
        source.setPitchLockEnabled(false);
        source.getNextAudioBlock({&settle,0,256});
        source.setPitchLockEnabled(true);
        source.getNextAudioBlock({&settle,0,256});
        ok &= require(source.activeConfigurationGeneration() == keylockGeneration,
                      "toggling keylock never rebuilds a pipeline");
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
        juce::AudioBuffer<float>b(2,8192);source.getNextAudioBlock({&b,0,8192});ok&=require(finite(b),"stretched finite");
        ok&=require(source.activeConfigurationGeneration()==generation,
                    "enabling keylock uses the standby pipeline instead of rebuilding");
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
        const auto switchesBeforeScratch=source.realtimeStats().successfulPipelineSwitches;
        source.enterScratchBypass();
        for(int i=0;i<16;++i){source.getNextAudioBlock({&b,0,8192});std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        source.enterScratchBypass();
        for(int i=0;i<16;++i){source.getNextAudioBlock({&b,0,8192});std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        ok&=require(source.activeConfigurationGeneration()==preScratchGeneration
                        && source.realtimeStats().successfulPipelineSwitches==switchesBeforeScratch,
                    "scratching keeps the keylock pipeline instead of rebuilding it");
        source.endScratchBypass();source.getNextAudioBlock({&b,0,8192});
        ok&=require(finite(b),"scratch transition finite");
        ok&=require(source.activeConfigurationGeneration()==preScratchGeneration,
                    "scratch release resumes keylock without waiting for a rebuild");
        // A block this long has room for an inline seed, so keylock must be
        // back in the signal path on the very first callback after release.
        ok&=require(source.getLatencySamples()>0,
                    "scratch release renders through keylock again immediately");
        auto stats=source.realtimeStats();
        ok&=require(stats.prepareCallsFromAudioThread==0,"no prepare in callback");
        ok&=require(stats.resetCallsFromAudioThread==0,"no reset in callback");
        ok&=require(stats.prewarmCallsFromAudioThread==0,"no prewarm in callback");
        ok&=require(stats.bufferGrowthsFromAudioThread==0,"no buffer growth in callback");
        ok&=require(stats.blockingLockAttempts==0,"no callback locks");
        source.releaseResources();
    }
    {   // Turning keylock on must not punch a hole in the output. A freshly
        // built stretcher runs silent for its own latency — over 30 ms — unless
        // it is handed the audio that was just playing as pre-roll, and that
        // dropout is audible as a click on every toggle.
        ToneSource tone; TimeStretchProcessor source(&tone); source.prepareToPlay(512,48000.0);
        juce::AudioBuffer<float> warm(2,512);
        for(int i=0;i<8;++i) source.getNextAudioBlock({&warm,0,512});
        const auto generation=source.activeConfigurationGeneration();
        source.setPitchLockEnabled(true); source.setTempoRatio(0.9);
        // Render across the whole transition. Whichever path the processor
        // picks — bridging on the direct path while the worker seeds, or
        // seeding inline — the output has to stay continuous.
        juce::AudioBuffer<float> b(2,256);
        int blocks=0,latencyWindow=0;
        double transitionSquares=0.0; int transitionCount=0;
        while(latencyWindow==0&&blocks<64){
            source.getNextAudioBlock({&b,0,256}); ++blocks;
            ok&=require(finite(b),"keylock transition output finite");
            latencyWindow=source.getLatencySamples();
            for(int i=0;i<256;++i){
                const float value=b.getSample(0,i);
                transitionSquares+=static_cast<double>(value)*value; ++transitionCount;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        ok&=require(source.activeConfigurationGeneration()==generation,
                    "keylock engages on the standby pipeline for the gap check");
        ok&=require(latencyWindow>0,"keylock reports a latency to check against");
        ok&=require(source.realtimeStats().keylockSeeds>0,
                    "engaging keylock seeds the stretcher from output history");
        // Everything up to and including the switch-over block, which is where
        // the dropout used to sit.
        ok&=require(std::sqrt(transitionSquares/transitionCount)>0.07,
                    "the keylock transition never goes quiet");
        // Only the stretcher's own latency window matters after that: it is
        // exactly the stretch that used to come out silent, and it is short
        // enough that later real audio cannot average the hole away.
        double sumOfSquares=0.0; int counted=0; float peak=0.0f;
        while(counted<latencyWindow){
            source.getNextAudioBlock({&b,0,256});
            ok&=require(finite(b),"keylock enable output finite");
            for(int i=0;i<256&&counted<latencyWindow;++i){
                const float value=b.getSample(0,i);
                sumOfSquares+=static_cast<double>(value)*value; ++counted;
                peak=std::max(peak,std::abs(value));
            }
        }
        // The tone sits at 0.2 amplitude, so continuous audio lands near 0.141
        // RMS. Half of that leaves room for the crossfade and the stretcher's
        // ripple; an unseeded pipeline reports 0 here.
        const double rms=std::sqrt(sumOfSquares/counted);
        ok&=require(rms>0.07,"enabling keylock does not silence the output");
        ok&=require(peak<1.0f,"enabling keylock does not overshoot");
    }
    std::mt19937 rng(0xB40CD5u); ToneSource tone; TimeStretchProcessor stress(&tone);stress.prepareToPlay(8192,48000);
    for(int i=0;i<1000;++i){if(i%7==0)stress.setPitchLockEnabled((i/7)%2);stress.setTempoRatio(0.25+(rng()%700)/100.0);if(i%31==0)stress.enterScratchBypass();if(i%31==2)stress.endScratchBypass();const int sizes[]={64,128,256,512,1024,2048,4096,8192};juce::AudioBuffer<float>b(2,sizes[rng()%8]);stress.getNextAudioBlock({&b,0,b.getNumSamples()});ok&=require(finite(b),"stress finite");}
    const auto stats=stress.realtimeStats();ok&=require(stats.prepareCallsFromAudioThread+stats.resetCallsFromAudioThread+stats.prewarmCallsFromAudioThread+stats.bufferGrowthsFromAudioThread+stats.blockingLockAttempts==0,"all realtime counters zero");
    if(std::getenv("BROCKDJ_TIME_STRETCH_BENCHMARK")){
        juce::AudioBuffer<float>b(2,512);double total=0.0,worst=0.0;
        for(int i=0;i<1000;++i){const auto start=std::chrono::steady_clock::now();stress.getNextAudioBlock({&b,0,512});const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();total+=us;worst=std::max(worst,us);}
        std::cout<<"time-stretch 512-block mean-us="<<total/1000.0<<" worst-us="<<worst<<'\n';
        // Isolate the scratch-release transition, which is the block that seeds
        // the stretcher and therefore the worst realistic callback in a set.
        // Pace it like a real device: the seed budget is expressed in audio
        // time, so a free-running loop would burn through it instantly.
        for (int blockSize : {128, 512}) {
            ToneSource benchTone; TimeStretchProcessor bench(&benchTone);
            bench.prepareToPlay(blockSize, 48000.0);
            bench.setPitchLockEnabled(true); bench.setTempoRatio(0.92);
            juce::AudioBuffer<float> rb(2, blockSize);
            const auto blockPeriod = std::chrono::nanoseconds(
                static_cast<long long>(1.0e9 * blockSize / 48000.0));
            const int settle = std::max(2, 48000 / blockSize / 20);
            auto deadline = std::chrono::steady_clock::now();
            double worstRelease = 0.0;
            const auto render = [&](bool measure) {
                deadline += blockPeriod;
                std::this_thread::sleep_until(deadline);
                const auto start = std::chrono::steady_clock::now();
                bench.getNextAudioBlock({&rb, 0, blockSize});
                if (measure)
                    worstRelease = std::max(worstRelease, std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - start).count());
            };
            for (int i = 0; i < 20; ++i) {
                for (int w = 0; w < settle; ++w) render(false);
                bench.enterScratchBypass();
                for (int w = 0; w < settle; ++w) render(false);
                bench.endScratchBypass();
                // Follow the whole transition, not just its first block: the
                // switch-over can land several callbacks after the release.
                for (int w = 0; w < settle; ++w) render(true);
            }
            const auto benchStats = bench.realtimeStats();
            std::cout << "keylock scratch-release block=" << blockSize
                      << " budget-us=" << (1.0e6 * blockSize / 48000.0)
                      << " worst-us=" << worstRelease
                      << " seeds=" << benchStats.keylockSeeds
                      << " inline=" << benchStats.keylockSeedsOnAudioThread
                      << " bridge-blocks=" << benchStats.keylockSeedBridgeBlocks
                      << " worst-seed-us=" << benchStats.worstKeylockSeedMicros
                      << " rebuilds=" << benchStats.successfulPipelineSwitches << '\n';
            bench.releaseResources();
        }
    }
    return ok?0:1;
}
