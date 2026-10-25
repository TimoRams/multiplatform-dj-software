#include "audio/TimeStretchProcessor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>
#include <cstdlib>
#include <vector>

namespace {
class ToneSource final : public juce::AudioSource {
public:
    void prepareToPlay(int, double rate) override { sampleRate = rate; }
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        ++renderCalls;
        if (!playing) {
            info.clearActiveBufferRegion();
            return;
        }
        for (int i=0;i<info.numSamples;++i) {
            const float value=static_cast<float>(0.2*std::sin(phase));
            phase += 2.0*juce::MathConstants<double>::pi*frequency/sampleRate;
            for(int ch=0;ch<info.buffer->getNumChannels();++ch)
                info.buffer->setSample(ch,info.startSample+i,value * (ch == 0 ? 1.0f : rightGain));
        }
    }
    [[nodiscard]] std::uint64_t getRenderCalls() const noexcept { return renderCalls; }
    bool playing = true;
    double frequency = 220.0;
    float rightGain = 1.0f;
private:
    double sampleRate=48000.0, phase=0.0;
    std::uint64_t renderCalls=0;
};
bool require(bool value,const char* message){if(!value)std::cerr<<"FAIL: "<<message<<'\n';return value;}
bool finite(const juce::AudioBuffer<float>& b){for(int ch=0;ch<b.getNumChannels();++ch)for(int i=0;i<b.getNumSamples();++i)if(!std::isfinite(b.getSample(ch,i)))return false;return true;}
bool waitForGeneration(TimeStretchProcessor& source,std::uint64_t oldGeneration){
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    juce::AudioBuffer<float> b(2,256);
    while(std::chrono::steady_clock::now()<until){source.getNextAudioBlock({&b,0,256});if(source.activeConfigurationGeneration()>oldGeneration)return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    return false;
}

bool checkKeylockToneQuality()
{
    bool ok = true;
    constexpr int blockSize = 128;
    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    for (const double frequency : {55.0, 220.0, 1000.0, 10000.0}) {
        ToneSource tone;
        tone.rightGain = -0.5f;
        tone.frequency = frequency * 0.8;
        TimeStretchProcessor source(&tone);
        source.setTempoRatio(0.8);
        source.setPitchLockEnabled(true);
        source.prepareToPlay(blockSize, sampleRate);
        juce::AudioBuffer<float> block(2, blockSize);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (source.getLatencySamples() == 0 && std::chrono::steady_clock::now() < until) {
            source.getNextAudioBlock({&block, 0, blockSize});
            std::this_thread::sleep_for(std::chrono::duration<double>(blockSize / sampleRate));
        }
        ok &= require(source.getLatencySamples() > 0, "Signalsmith engages for quality measurement");
        const auto seeds = source.realtimeStats().keylockSeeds;
        // Reuse the live pipeline to exercise pitch updates in both directions.
        for (const double tempo : {0.8, 0.9, 1.0, 1.1, 1.2, 0.8}) {
            tone.frequency = frequency * tempo;
            source.setTempoRatio(tempo);
            const int settleSamples = 3 * source.getLatencySamples() + blockSize;
            for (int i = 0; i < settleSamples; i += blockSize)
                source.getNextAudioBlock({&block, 0, blockSize});
            double energy = 0.0;
            double stereoError = 0.0;
            int crossings = 0;
            double firstCrossing = 0.0, lastCrossing = 0.0;
            float previous = block.getSample(0, blockSize - 1);
            const int measuredSamples = static_cast<int>(sampleRate);
            std::vector<float> output;
            output.reserve(measuredSamples);
            const double expectedStep = 2.0 * juce::MathConstants<double>::pi * frequency / sampleRate;
            for (int offset = 0; offset < measuredSamples; offset += blockSize) {
                const int count = std::min(blockSize, measuredSamples - offset);
                source.getNextAudioBlock({&block, 0, count});
                for (int i = 0; i < count; ++i) {
                    const double value = block.getSample(0, i);
                    if (previous < 0.0f && value >= 0.0) {
                        // Sine interpolation avoids the substantial timing bias
                        // of a straight-line crossing near Nyquist.
                        const double crossingPhase = std::atan2(
                            value * std::sin(expectedStep), value * std::cos(expectedStep) - previous);
                        lastCrossing = offset + i - crossingPhase / expectedStep;
                        if (crossings++ == 0)
                            firstCrossing = lastCrossing;
                    }
                    previous = static_cast<float>(value);
                    output.push_back(static_cast<float>(value));
                    energy += value * value;
                    const double error = block.getSample(1, i) - tone.rightGain * value;
                    stereoError += error * error;
                }
            }
            energy /= measuredSamples;
            if (!require(crossings > 2, "quality measurement contains a sustained audible tone")) {
                ok = false;
                continue;
            }
            const double measuredHz = (crossings - 1) * sampleRate / (lastCrossing - firstCrossing);
            double sinProjection = 0.0, cosProjection = 0.0;
            double ss = 0.0, cc = 0.0, sc = 0.0;
            for (int i = 0; i < measuredSamples; ++i) {
                const double phase = 2.0 * juce::MathConstants<double>::pi * measuredHz * i / sampleRate;
                const double s = std::sin(phase), c = std::cos(phase);
                sinProjection += output[i] * s;
                cosProjection += output[i] * c;
                ss += s * s;
                cc += c * c;
                sc += s * c;
            }
            const double determinant = ss * cc - sc * sc;
            const double a = (sinProjection * cc - cosProjection * sc) / determinant;
            const double b = (cosProjection * ss - sinProjection * sc) / determinant;
            const double coherentEnergy = (a * sinProjection + b * cosProjection) / measuredSamples;
            const double gainDb = 10.0 * std::log10(std::max(1.0e-20, energy) / 0.02);
            const double residualDb = 10.0 * std::log10(std::max(1.0e-20, energy - coherentEnergy)
                                                       / std::max(1.0e-20, energy));
            const double pitchCents = 1200.0 * std::log2(measuredHz / frequency);
            // Regression limits, not a perceptual transparency guarantee. The
            // spectral peak mapper is less pitch-accurate for deep bass.
            const double pitchLimitCents = frequency < 100.0 ? 25.0 : 10.0;
            const bool quality = std::isfinite(gainDb) && std::isfinite(residualDb)
                && std::abs(gainDb) < 0.5 && residualDb < -40.0
                && std::isfinite(pitchCents) && std::abs(pitchCents) < pitchLimitCents;
            if (!quality || std::getenv("BROCKDJ_TIME_STRETCH_BENCHMARK"))
                std::cout << "keylock rate=" << sampleRate << " tone-hz=" << frequency
                          << " tempo=" << tempo << " gain-db=" << gainDb
                          << " residual-db=" << residualDb << " pitch-cents=" << pitchCents
                          << " stereo-error=" << stereoError / measuredSamples << '\n';
            ok &= require(quality, "Signalsmith preserves tone level and spectral purity across +/-20%");
            ok &= require(stereoError / measuredSamples < 1.0e-9,
                          "Signalsmith preserves stereo polarity and channel balance");
        }
        ok &= require(source.realtimeStats().keylockSeeds == seeds,
                      "tempo sweeps do not reseed the quality measurement pipeline");
    }
    return ok;
}

bool checkKeyShiftTonalitySwitch()
{
    ToneSource tone;
    tone.frequency = 10000.0;
    TimeStretchProcessor source(&tone);
    source.setKeySemitoneOffset(12.0 * std::log2(1.2));
    source.setPitchLockEnabled(true);
    source.prepareToPlay(128, 48000.0);
    juce::AudioBuffer<float> block(2, 128);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (source.getLatencySamples() == 0 && std::chrono::steady_clock::now() < until) {
        source.getNextAudioBlock({&block, 0, 128});
        std::this_thread::sleep_for(std::chrono::duration<double>(128.0 / 48000.0));
    }
    bool ok = require(source.getLatencySamples() > 0, "Key Shift engages its pipeline");
    const auto seeds = source.realtimeStats().keylockSeeds;
    // At unity tempo the pitch factor stays identical across these toggles.
    // The frequency map must still change, without reseeding or rebuilding.
    for (const bool locked : {true, false, true}) {
        source.setPitchLockEnabled(locked);
        for (int i = 0; i < 3 * source.getLatencySamples() + 128; i += 128)
            source.getNextAudioBlock({&block, 0, 128});
        int crossings = 0;
        float previous = block.getSample(0, 127);
        for (int offset = 0; offset < 48000; offset += 128) {
            const int count = std::min(128, 48000 - offset);
            source.getNextAudioBlock({&block, 0, count});
            for (int i = 0; i < count; ++i) {
                const float value = block.getSample(0, i);
                crossings += previous < 0.0f && value >= 0.0f;
                previous = value;
            }
        }
        const double expectedHz = locked ? 12000.0
            : 10000.0 + 0.2 * TimeStretchProcessor::kKeyShiftTonalityLimitHz / std::sqrt(1.2);
        ok &= require(std::abs(crossings - expectedHz) < 5.0,
                      "keylock/Key Shift toggles update tonality even at an unchanged pitch factor");
    }
    ok &= require(source.realtimeStats().keylockSeeds == seeds,
                  "Key Shift tonality toggles preserve the running spectral state");
    return ok;
}
}

static_assert(noexcept(std::declval<TimeStretchProcessor&>().getNextAudioBlock(
    std::declval<const juce::AudioSourceChannelInfo&>())));

int main(){
    bool ok=true;
    ok &= checkKeylockToneQuality();
    ok &= checkKeyShiftTonalitySwitch();
    for (const auto backend : {TimeStretchBackend::Signalsmith, TimeStretchBackend::RubberBand}) {
        ToneSource tone;
        TimeStretchProcessor source(&tone);
        source.setBackend(backend);
        source.prepareToPlay(128, 48000.0);
        source.setPitchLockEnabled(true);
        source.setTempoRatio(0.8);
        juce::AudioBuffer<float> block(2, 128);
        for (const int pauseBlocks : {160, 1, 4}) {
            tone.playing = false;
            source.setInputPlaybackActive(false);
            for (int i = 0; i < pauseBlocks; ++i)
                source.getNextAudioBlock({&block, 0, 128});
            tone.playing = true;
            source.setInputPlaybackActive(true);
            bool engaged = false;
            float minimumRms = 1.0f;
            float windowEnergy = 0.0f;
            for (int i = 0; i < 140; ++i) {
                source.getNextAudioBlock({&block, 0, 128});
                engaged |= source.getLatencySamples() > 0;
                if (i >= 4) {
                    const float rms = block.getRMSLevel(0, 0, 128);
                    windowEnergy += rms * rms;
                    if (i % 4 == 3) {
                        minimumRms = std::min(minimumRms, std::sqrt(windowEnergy / 4.0f));
                        windowEnergy = 0.0f;
                    }
                }
                std::this_thread::sleep_for(std::chrono::duration<double>(128.0 / 48000.0));
            }
            if (minimumRms < 0.04f)
                std::cerr << "resume backend=" << static_cast<int>(backend)
                          << " minimum-rms=" << minimumRms << '\n';
            ok &= require(engaged, "keylock returns after play/pause");
            ok &= require(minimumRms >= 0.04f, "keylock resume does not replay paused silence");
        }
    }
    for (const auto backend : {TimeStretchBackend::Signalsmith, TimeStretchBackend::RubberBand})
    for (const double rate : {44100.0, 48000.0, 96000.0})
    for (const int samples : {64, 128, 512, 2048}) {
        ToneSource tone;
        TimeStretchProcessor source(&tone);
        source.setBackend(backend);
        source.prepareToPlay(samples, rate);
        source.setTempoRatio(0.92);
        juce::AudioBuffer<float> block(2, samples + 16);
        constexpr int start = 7;
        for (int i = 0; i < 32; ++i)
            source.getNextAudioBlock({&block, start, samples});
        float previous = block.getSample(0, start + samples - 1);
        source.setPitchLockEnabled(true);
        bool engaged = false;
        int lockedSamples = 0;
        float maximumEnableStep = 0.0f;
        int maximumStepSample = 0;
        for (int i = 0; i < 100; ++i) {
            source.getNextAudioBlock({&block, start, samples});
            for (int sample = start; sample < start + samples; ++sample) {
                const float value = block.getSample(0, sample);
                if (std::abs(value - previous) > maximumEnableStep) {
                    maximumEnableStep = std::abs(value - previous);
                    maximumStepSample = sample - start;
                }
                previous = value;
            }
            if (maximumEnableStep >= 0.025f)
                std::cerr << "transition backend=" << static_cast<int>(backend)
                          << " rate=" << rate << " block=" << samples
                          << " maximum-step=" << maximumEnableStep
                          << " sample=" << maximumStepSample << '\n';
            ok &= require(maximumEnableStep < 0.025f, "enabling keylock has no waveform step");
            if (source.getLatencySamples() > 0) {
                engaged = true;
                lockedSamples += samples;
                if (lockedSamples >= source.getLatencySamples() + 2 * samples)
                    break;
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(samples / rate));
        }
        if (!engaged)
            std::cerr << "did not engage backend=" << static_cast<int>(backend)
                      << " rate=" << rate << " block=" << samples << '\n';
        ok &= require(engaged, "both backends engage keylock at all device buffer sizes");
        for (int i = 0; i < 32; ++i)
            source.getNextAudioBlock({&block, start, samples});
        previous = block.getSample(0, start + samples - 1);
        source.setPitchLockEnabled(false);
        for (int i = 0; i < 4; ++i) {
            source.getNextAudioBlock({&block, start, samples});
            for (int sample = start; sample < start + samples; ++sample) {
                const float value = block.getSample(0, sample);
                ok &= require(std::abs(value - previous) < 0.025f,
                              "disabling keylock has no waveform step, including fade boundaries");
                previous = value;
            }
        }
        ok &= require(source.realtimeStats().keylockSeedsOnAudioThread == 0,
                      "keylock transitions never seed on the device callback");
    }
    {
        ToneSource tone;
        TimeStretchProcessor source(&tone);
        source.prepareToPlay(128, 48000.0);
        juce::AudioBuffer<float> block(2, 128);
        for (int i = 0; i < 120; ++i) {
            source.setPitchLockEnabled(i % 3 != 0);
            source.setBackend(i % 2 == 0 ? TimeStretchBackend::Signalsmith
                                        : TimeStretchBackend::RubberBand);
            if (i % 11 == 0)
                source.setTrackGeneration(100 + i);
            source.getNextAudioBlock({&block, 0, 128});
            ok &= require(finite(block), "concurrent seeds and backend changes remain finite");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        source.setPitchLockEnabled(true);
        source.setBackend(TimeStretchBackend::Signalsmith);
        bool recovered = false;
        for (int i = 0; i < 500; ++i) {
            source.getNextAudioBlock({&block, 0, 128});
            if (source.activeBackend() == TimeStretchBackend::Signalsmith
                && source.getLatencySamples() > 0) {
                recovered = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        ok &= require(recovered, "rapid backend/keylock/track changes recover without a stuck bridge");
        ok &= require(source.realtimeStats().keylockSeedsOnAudioThread == 0,
                      "rapid transitions do not reset a stretcher in the callback");
    }
    {
        ToneSource tone;
        TimeStretchProcessor source(&tone);
        source.prepareToPlay(128, 48000.0);
        source.setTempoRatio(0.92);
        juce::AudioBuffer<float> block(2, 128);
        for (int i = 0; i < 64; ++i)
            source.getNextAudioBlock({&block, 0, 128});
        source.setPitchLockEnabled(true);
        // Deliberately advance faster than a device, so the snapshot falls
        // behind even if the seed worker runs promptly.
        for (int i = 0; i < 32; ++i)
            source.getNextAudioBlock({&block, 0, 128});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bool engaged = false;
        float previous = block.getSample(0, 127);
        float maximumStep = 0.0f;
        for (int i = 0; i < 160; ++i) {
            source.getNextAudioBlock({&block, 0, 128});
            engaged |= source.getLatencySamples() > 0;
            for (int sample = 0; sample < 128; ++sample) {
                const float value = block.getSample(0, sample);
                maximumStep = std::max(maximumStep, std::abs(value - previous));
                previous = value;
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(128.0 / 48000.0));
        }
        ok &= require(engaged, "a delayed seed catches up and eventually engages");
        ok &= require(maximumStep < 0.025f, "worker catch-up leaves no missing-input waveform splice");
        const auto seeds = source.realtimeStats().keylockSeeds;
        for (int i = 0; i < 64; ++i) {
            source.setTempoRatio(0.92 + 0.0001 * std::sin(i * 0.1));
            source.getNextAudioBlock({&block, 0, 128});
        }
        ok &= require(source.realtimeStats().keylockSeeds == seeds,
                      "continuous beatmatch rate corrections never reseed keylock");
    }
    {   // A loaded but paused deck still passes through this processor. It must
        // pull the silent router once, not spend FFT work filling a keylock
        // pipeline with silence on every callback.
        ToneSource tone; TimeStretchProcessor source(&tone); source.prepareToPlay(512, 48000.0);
        source.setPitchLockEnabled(true);
        source.setTempoRatio(0.8);
        source.setInputPlaybackActive(false);
        juce::AudioBuffer<float> paused(2, 512);
        const auto callsBefore = tone.getRenderCalls();
        source.getNextAudioBlock({&paused, 0, paused.getNumSamples()});
        ok &= require(tone.getRenderCalls() == callsBefore + 1,
                      "paused keylock pulls its input only once");
        ok &= require(source.getLatencySamples() == 0,
                      "paused keylock bypasses the stretcher");
        source.setInputPlaybackActive(true);
    }
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
        // All buffers bridge while the worker seeds. Keylock has to return
        // without waiting on a pipeline rebuild or seeding in the callback.
        int scratchReleaseBlocks = 0;
        const auto blockPeriod = std::chrono::nanoseconds(
            static_cast<long long>(1.0e9 * b.getNumSamples() / rate));
        auto nextReleaseBlock = std::chrono::steady_clock::now();
        while (source.getLatencySamples() == 0 && scratchReleaseBlocks < 64) {
            // Give the seed worker the time a real device grants between
            // callbacks; a free-running loop measures scheduler luck instead.
            nextReleaseBlock += blockPeriod;
            std::this_thread::sleep_until(nextReleaseBlock);
            source.getNextAudioBlock({&b,0,8192});
            ++scratchReleaseBlocks;
        }
        ok&=require(source.getLatencySamples()>0,
                    "scratch release restores keylock after a bounded bridge");
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
        // spends bridging on the direct path while the worker seeds, the output
        // has to stay continuous.
        juce::AudioBuffer<float> b(2,256);
        int blocks=0,latencyWindow=0;
        double transitionSquares=0.0; int transitionCount=0;
        while(latencyWindow==0&&blocks<256){
            source.getNextAudioBlock({&b,0,256}); ++blocks;
            ok&=require(finite(b),"keylock transition output finite");
            latencyWindow=source.getLatencySamples();
            for(int i=0;i<256;++i){
                const float value=b.getSample(0,i);
                transitionSquares+=static_cast<double>(value)*value; ++transitionCount;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(6000));
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
        for (const int blockSize : {64, 128, 512})
        for (const double tempo : {0.8, 1.2}) {
            std::array<ToneSource, 4> tones;
            std::array<std::unique_ptr<TimeStretchProcessor>, 4> decks;
            juce::AudioBuffer<float> output(2, blockSize);
            for (int i = 0; i < 4; ++i) {
                tones[i].frequency = (110.0 + 55.0 * i) * tempo;
                decks[i] = std::make_unique<TimeStretchProcessor>(&tones[i]);
                decks[i]->setTempoRatio(tempo);
                decks[i]->setPitchLockEnabled(true);
                decks[i]->prepareToPlay(blockSize, 48000.0);
            }
            const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(blockSize / 48000.0));
            auto deadline = std::chrono::steady_clock::now();
            const auto render = [&] {
                deadline += period;
                std::this_thread::sleep_until(deadline);
                const auto start = std::chrono::steady_clock::now();
                for (auto& deck : decks)
                    deck->getNextAudioBlock({&output, 0, blockSize});
                return std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - start).count();
            };
            const auto until = deadline + std::chrono::seconds(5);
            bool engaged = false;
            while (!engaged && std::chrono::steady_clock::now() < until) {
                render();
                engaged = std::all_of(decks.begin(), decks.end(),
                    [](const auto& deck) { return deck->getLatencySamples() > 0; });
            }
            ok &= require(engaged, "four-deck benchmark actually renders through Signalsmith");
            double total = 0.0, worst = 0.0;
            constexpr int measuredBlocks = 200;
            for (int i = 0; i < measuredBlocks; ++i) {
                const double micros = render();
                total += micros;
                worst = std::max(worst, micros);
            }
            std::cout << "keylock four-deck block=" << blockSize << " tempo=" << tempo
                      << " latency-samples=" << decks[0]->getLatencySamples()
                      << " budget-us=" << (1.0e6 * blockSize / 48000.0)
                      << " mean-us=" << total / measuredBlocks << " worst-us=" << worst << '\n';
        }
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
