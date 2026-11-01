#include "audio/DeckChannelProcessor.h"
#include "fx/FxPrimitives.h"

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
void testFxQuantize(bool& ok)
{
    // A 128-sample roll requested halfway through its division must capture
    // 64 samples later with quantize, while tempo-only sync captures immediately.
    const auto render = [](bool quantize, bool cancelWaiting = false) {
        auto fx = std::make_unique<FxProcessor>();
        fx->prepare(48000.0, 64, 2);
        fx->setEffectType(EffectType::Roll);
        fx->setAmount(1.0f);
        fx->setExternalDelayTime(128.0f / 48000.0f);
        fx->setBeatSyncPosition(128.0 / 48000.0, 0.5);
        fx->setQuantizeEnabled(quantize);
        juce::AudioBuffer<float> result(2, 256);
        for (int i = 0; i < 256; ++i)
            for (int channel = 0; channel < 2; ++channel)
                result.setSample(channel, i, 0.1f + i * 0.002f);
        for (int start = 0; start < 256; start += 32) {
            if (cancelWaiting && start == 32) fx->setQuantizeEnabled(false);
            fx->process(result, start, 32);
        }
        return result;
    };
    const auto echoStart = [](bool quantize) {
        auto fx = std::make_unique<FxProcessor>();
        fx->prepare(48000.0, 64, 2);
        fx->setAmount(1.0f);
        juce::AudioBuffer<float> block(2, 64);
        // Settle the wet/dry smoothing before testing the exact start boundary.
        for (int i = 0; i < 20; ++i) { block.clear(); fx->process(block, 0, 64); }
        fx->setEffectType(EffectType::Echo);
        fx->setExternalDelayTime(0.01f);
        fx->setBeatSyncPosition(0.01, 0.5); // half of a 480-sample division
        fx->setQuantizeEnabled(quantize);
        bool waitingIsDry = true;
        bool startsAtBoundary = false;
        for (int start = 0; start < 256; start += 64) {
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < 64; ++i) block.setSample(channel, i, 1.0f);
            fx->process(block, 0, 64);
            for (int i = 0; i < 64 && start + i < 240; ++i)
                waitingIsDry &= std::abs(block.getSample(0, i) - 1.0f) < 1.0e-5f;
            if (start == 192)
                startsAtBoundary = std::abs(block.getSample(0, 48)) < 1.0e-5f;
        }
        return std::pair{waitingIsDry, startsAtBoundary};
    };
    const auto quantizedEcho = echoStart(true);
    const auto immediateEcho = echoStart(false);
    ok &= require(quantizedEcho.first && quantizedEcho.second && !immediateEcho.first,
                  "echo capture waits transparently and starts on the exact grid sample inside a block");
    const auto immediate = render(false);
    const auto quantized = render(true);
    const auto cancelled = render(true, true);
    const float dryAt160 = 0.1f + 160 * 0.002f;
    ok &= require(std::abs(quantized.getSample(0, 160) - dryAt160) < 1.0e-5f,
                  "quantized roll keeps live audio through the delayed capture window");
    ok &= require(immediate.getSample(0, 160) < dryAt160 - 0.01f,
                  "tempo-only roll starts immediately without waiting for the beatgrid");
    ok &= require(cancelled.getSample(0, 170) < quantized.getSample(0, 170) - 0.01f,
                  "disabling quantize cancels a pending roll wait at the next audio block");
    ok &= require(finite(immediate) && finite(quantized) && finite(cancelled),
                  "FX quantize produces finite stereo output");
}

void testFxPrimitives(bool& ok)
{
    dsp::SsDelay delay;
    delay.prepare(8);
    delay.reset();
    delay.write(0.25f);
    delay.write(0.75f);
    ok &= require(std::abs(delay.read(1) - 0.25f) < 1.0e-6f,
                  "fractional delay wrapper preserves integer tap");
    ok &= require(std::abs(delay.readFrac(1.5f) - 0.125f) < 1.0e-6f,
                  "fractional delay wrapper preserves linear interpolation");
    delay.reset();
    ok &= require(std::abs(delay.read(1)) < 1.0e-6f,
                  "fractional delay wrapper reset clears history");

    dsp::SsLfo lfo;
    lfo.prepare(4.0);
    lfo.setRateImmediate(1.0f);
    lfo.tick();
    ok &= require(std::abs(lfo.sine() - 1.0f) < 1.0e-6f,
                  "LFO wrapper advances phase by rate over sample rate");
    ok &= require(std::abs(lfo.sineUnipolar(0.5f)) < 1.0e-6f,
                  "LFO wrapper retains phase offset and unipolar mapping");

    constexpr int sampleCount = 2048;
    constexpr double sampleRate = 48000.0;
    juce::AudioBuffer<float> lowPass(2, sampleCount);
    juce::AudioBuffer<float> highPass(2, sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
        const float value = static_cast<float>(
            std::sin(juce::MathConstants<double>::twoPi * 12000.0 * sample / sampleRate));
        lowPass.setSample(0, sample, value);
        lowPass.setSample(1, sample, value);
        highPass.setSample(0, sample, value);
        highPass.setSample(1, sample, value);
    }

    dsp::SvfSmoothed lowPassFilter;
    dsp::SvfSmoothed highPassFilter;
    lowPassFilter.prepare(sampleRate);
    highPassFilter.prepare(sampleRate);
    lowPassFilter.setTargets(500.0f, 0.707f);
    highPassFilter.setTargets(500.0f, 0.707f);
    lowPassFilter.process(lowPass, 0, sampleCount, dsp::SvfSmoothed::Mode::LowPass);
    highPassFilter.process(highPass, 0, sampleCount, dsp::SvfSmoothed::Mode::HighPass);
    ok &= require(finite(lowPass) && finite(highPass),
                  "smoothed SVF wrapper remains finite");
    ok &= require(lowPass.getRMSLevel(0, 0, sampleCount) <
                      highPass.getRMSLevel(0, 0, sampleCount) * 0.1f,
                  "smoothed SVF wrapper distinguishes low-pass and high-pass modes");
}
float render(DeckChannelProcessor&m,int size){juce::AudioBuffer<float>b(2,size);m.getNextAudioBlock({&b,0,size});return b.getMagnitude(0,0,size);}
float renderStereoDifference(DeckChannelProcessor& mixer, int size)
{
    juce::AudioBuffer<float> buffer(2, size);
    mixer.getNextAudioBlock({&buffer, 0, size});
    float maximumDifference = 0.0f;
    for (int sample = 0; sample < size; ++sample) {
        maximumDifference = std::max(
            maximumDifference,
            std::abs(buffer.getSample(0, sample) - buffer.getSample(1, sample)));
    }
    return maximumDifference;
}
void testColorCrushLeftTexture(bool& ok)
{
    for (const double rate : {44100.0, 48000.0, 96000.0})
    for (const int block : {64, 256, 1024})
    for (const float param : {0.5f, 0.7f, 1.0f}) {
        auto fx = std::make_unique<FxProcessor>();
        fx->prepare(rate, block, 2);
        fx->setEffectType(EffectType::SoundColorCrush);
        fx->setSCParamValue(param);
        fx->setSCKnobValue(-1.0f);
        juce::AudioBuffer<float> buffer(2, block);
        int frame = 0, measured = 0, held = 0, changes = 0;
        float previous = 0.0f;
        double energy = 0.0;
        while (frame < rate * 0.15) {
            for (int sample = 0; sample < block; ++sample) {
                const double time = (frame + sample) / rate;
                const float input = static_cast<float>(0.42 * std::sin(2.0 * std::numbers::pi * 997.0 * time)
                    + 0.17 * std::sin(2.0 * std::numbers::pi * 3181.0 * time));
                buffer.setSample(0, sample, input);
                buffer.setSample(1, sample, input);
            }
            fx->process(buffer, 0, block);
            for (int sample = 0; sample < block; ++sample) {
                const float value = buffer.getSample(0, sample);
                if (frame + sample >= rate * 0.05) {
                    if (std::abs(value - previous) < 1.0e-7f) ++held;
                    else ++changes;
                    energy += static_cast<double>(value) * value;
                    ++measured;
                }
                previous = value;
            }
            frame += block;
        }
        const double heldFraction = static_cast<double>(held) / measured;
        const double rms = std::sqrt(energy / measured);
        std::cout << "left-crush rate=" << rate << " block=" << block << " adjust=" << param
                  << " held-fraction=" << heldFraction << " changes=" << changes << " rms=" << rms << '\n';
        ok &= require(heldFraction > 0.8,
                      "left Color Crush retains audible sample-and-hold steps instead of smoothing them into a low-pass");
        ok &= require(changes > 10 && rms > 0.05 && finite(buffer),
                      "left Color Crush produces a changing audible crushed signal");
    }
}

void testEffectMixEndpoints(bool& ok)
{
    for (const double rate : {44100.0, 48000.0, 96000.0})
    for (const int block : {64, 256, 1024})
    for (const float param : {0.0f, 0.5f, 0.7f, 1.0f})
    for (const float knob : {-1.0f, 1.0f}) {
        auto fx = std::make_unique<FxProcessor>();
        fx->prepare(rate, block, 2);
        fx->setEffectType(EffectType::SoundColorCrush);
        fx->setSCParamValue(param);
        fx->setSCKnobValue(knob);
        juce::AudioBuffer<float> buffer(2, block);
        const auto renderQuiet = [&] {
            for (int channel = 0; channel < 2; ++channel)
                std::fill_n(buffer.getWritePointer(channel), block, 1.0e-6f);
            fx->process(buffer, 0, block);
        };
        for (int sample = 0; sample < rate * 0.05; sample += block)
            renderQuiet();
        const double dryLeak = buffer.getMagnitude(0, 0, block) / 1.0e-6;
        std::cout << "crush-full-wet rate=" << rate << " block=" << block
                  << " adjust=" << param << " knob=" << knob << " dry-leak=" << dryLeak << '\n';
        ok &= require(dryLeak < 1.0e-5, "full Color Crush removes the unquantized dry signal for every Adjust value");
        fx->setSCKnobValue(0.0f);
        for (int sample = 0; sample < rate * 0.05; sample += block)
            renderQuiet();
        ok &= require(std::abs(buffer.getSample(0, block - 1) - 1.0e-6f) < 1.0e-10f,
                      "Color Crush centre restores the unprocessed signal");
    }

    for (const auto type : {EffectType::SoundColorDubEcho, EffectType::SoundColorSpace,
                            EffectType::Echo, EffectType::LowCutEcho, EffectType::MtDelay,
                            EffectType::Spiral, EffectType::Reverb})
    for (const float mix : {0.0f, 0.5f, 1.0f}) {
        const bool color = FxProcessor::isColorFxType(type);
        auto fx = std::make_unique<FxProcessor>();
        fx->prepare(48000.0, 64, 2);
        fx->setEffectType(type);
        fx->setAmount(mix);
        fx->setSCKnobValue(mix);
        fx->setSCParamValue(0.0f);
        fx->setExternalDelayTime(0.005f);
        fx->applyPendingCommandAtBlockBoundary();
        juce::AudioBuffer<float> input(2, 64), tail(2, 64);
        for (int sample = 0; sample < 4800; sample += 64) {
            input.clear();
            if (color) fx->process(input, 0, 64);
            else fx->processWetReturn(input, tail, 0, 64);
        }
        input.clear();
        input.setSample(0, 0, 0.2f);
        input.setSample(1, 0, 0.2f);
        if (color) fx->process(input, 0, 64);
        else fx->processWetReturn(input, tail, 0, 64);
        const float dryGain = input.getSample(0, 0) / 0.2f;
        std::cout << "tail-mix type=" << static_cast<int>(type) << " mix=" << mix
                  << " instantaneous-dry-gain=" << dryGain << '\n';
        if (!color || mix == 0.0f || mix == 1.0f)
            ok &= require(std::abs(dryGain - (1.0f - mix)) < 1.0e-5f,
                          "tail effects honor dry/wet endpoints instead of retaining the original at full wet");
        if (!color)
            ok &= require(std::abs(tail.getSample(0, 0)) < 1.0e-6f,
                          "wet-only tail return contains no instantaneous copy of the input");
        float tailPeak = 0.0f;
        for (int sample = 64; sample < 12000; sample += 64) {
            input.clear();
            if (color) fx->process(input, 0, 64);
            else fx->processWetReturn(input, tail, 0, 64);
            tailPeak = std::max(tailPeak, (color ? input : tail).getMagnitude(0, 0, 64));
        }
        ok &= require(mix == 0.0f || tailPeak > 1.0e-7f, "wet tail remains audible after removing the direct input");
    }

    for (const auto type : {EffectType::Echo, EffectType::LowCutEcho, EffectType::MtDelay,
                            EffectType::Spiral, EffectType::Reverb})
    for (const float mix : {0.0f, 0.5f, 1.0f})
    for (const bool colorActive : {false, true}) {
        if (colorActive && mix != 1.0f)
            continue;
        Sine source(1000.0);
        auto mixer = std::make_unique<DeckChannelProcessor>(&source);
        mixer->prepareToPlay(480, 48000.0);
        if (colorActive) {
            mixer->setFxEffectType(EffectType::SoundColorCrush);
            mixer->setFxSCKnob(0.4f);
            mixer->setFxSCParam(0.7f);
        }
        mixer->setFxSlotEffectType(1, type);
        mixer->setFxSlotAmount(1, mix);
        mixer->setFxSlotExternalDelayTime(1, 0.005f);
        juce::AudioBuffer<float> buffer(2, 480);
        for (int block = 0; block < 12; ++block)
            mixer->getNextAudioBlock({&buffer, 0, 480});
        const float expectedDryRms = 0.2f / std::sqrt(2.0f) * (1.0f - mix);
        ok &= require(std::abs(buffer.getRMSLevel(0, 0, 480) - expectedDryRms) < 0.001f,
                      "deck Beat FX follows the requested dry gain through the complete mixer");
        const auto& pfl = mixer->getPflBuffer();
        const auto& tail = mixer->getPostFaderTailBuffer();
        float pflError = 0.0f;
        for (int sample = 0; sample < 480; ++sample)
            pflError = std::max(pflError, std::abs(pfl.getSample(0, sample)
                - buffer.getSample(0, sample) - tail.getSample(0, sample)));
        ok &= require(pflError < 1.0e-6f, "headphones receive the complete dry/wet mix before the channel fader");
        ok &= require(mix == 0.0f || tail.getMagnitude(0, 0, 480) > 0.0001f,
                      "full-wet Beat FX remains audible in the deck tail return");
        mixer->setFader(0.0f);
        for (int block = 0; block < 12; ++block)
            mixer->getNextAudioBlock({&buffer, 0, 480});
        ok &= require(mix == 0.0f || mixer->getPflBuffer().getMagnitude(0, 0, 480) > 0.0001f,
                      "full-wet effect remains audible in PFL with the channel fader closed");
        ok &= require(buffer.getMagnitude(0, 0, 480) < 1.0e-7f
                          && mixer->getPostFaderTailBuffer().getMagnitude(0, 0, 480) < 1.0e-7f,
                      "channel fader still closes both dry and wet master paths");
    }
    {
        Sine source(900.0);
        source.setAmplitude(0.0f);
        auto mixer = std::make_unique<DeckChannelProcessor>(&source);
        mixer->prepareToPlay(64, 48000.0);
        for (const int slot : {1, 2}) {
            mixer->setFxSlotEffectType(slot, EffectType::Echo);
            mixer->setFxSlotAmount(slot, 1.0f);
            mixer->setFxSlotExternalDelayTime(slot, slot * 0.005f);
        }
        juce::AudioBuffer<float> buffer(2, 64);
        for (int block = 0; block < 80; ++block)
            mixer->getNextAudioBlock({&buffer, 0, 64});
        source.setAmplitude(0.2f);
        float earlyLeak = 0.0f, latePeak = 0.0f;
        for (int block = 0; block < 32; ++block) {
            mixer->getNextAudioBlock({&buffer, 0, 64});
            const auto& tail = mixer->getPostFaderTailBuffer();
            for (int sample = 0; sample < 64; ++sample) {
                const float value = std::abs(buffer.getSample(0, sample) + tail.getSample(0, sample));
                if (block * 64 + sample < 600) earlyLeak = std::max(earlyLeak, value);
                else latePeak = std::max(latePeak, value);
            }
        }
        ok &= require(earlyLeak < 1.0e-6f && latePeak > 0.001f,
                      "two full-wet echo slots apply both delays without leaking the earlier return");
    }
}

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
 testFxQuantize(ok);
 testFxPrimitives(ok);
 testColorCrushLeftTexture(ok);
 testEffectMixEndpoints(ok);
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
 {
     Sine peakTone(1000);
     auto peakMixer = std::make_unique<DeckChannelProcessor>(&peakTone);
     peakMixer->prepareToPlay(512, 48000);
     peakTone.setAmplitude(0.8f);
     render(*peakMixer, 512);
     peakTone.setAmplitude(0.0f);
     for (int i = 0; i < 4; ++i) render(*peakMixer, 512);
     ok &= require(peakMixer->m_preFaderPeakL.load() < 0.0001f,
                   "the latest block is silent after a short channel peak");
     ok &= require(peakMixer->takePreFaderFeedbackPeak() > 0.7f,
                   "hardware feedback captures a short peak between polls");
     const auto uiPeaks = peakMixer->takePreFaderUiPeaks();
     ok &= require(uiPeaks[0] > 0.7f && uiPeaks[1] > 0.7f,
                   "UI and hardware consume independent stereo peak windows");
     const auto silence = peakMixer->takePreFaderUiPeaks();
     ok &= require(peakMixer->takePreFaderFeedbackPeak() < 0.0001f
                       && silence[0] < 0.0001f && silence[1] < 0.0001f,
                   "reading the peak releases it immediately without an extra software hold");
     peakTone.setAmplitude(0.2f);
     render(*peakMixer, 512);
     (void)peakMixer->takePreFaderFeedbackPeak();
     ok &= require(peakMixer->takePreFaderFeedbackPeak() > 0.1f,
                   "meter polls faster than audio blocks do not insert false silence");
 }
 {Sine meterTone(1000);auto meterMixer=std::make_unique<DeckChannelProcessor>(&meterTone);meterMixer->prepareToPlay(1024,48000);for(int i=0;i<3;++i)render(*meterMixer,1024);const float openPre=meterMixer->m_preFaderPeakL.load();const float openPost=meterMixer->m_peakL.load();meterMixer->setFader(0.0f);for(int i=0;i<3;++i)render(*meterMixer,1024);const float closedPre=meterMixer->m_preFaderPeakL.load();const float closedPost=meterMixer->m_peakL.load();ok&=require(openPre>0.1f&&openPost>0.1f,"meter has signal with open fader");ok&=require(closedPre>0.1f,"pre-fader meter ignores closed channel fader");ok&=require(closedPost<0.001f,"post-fader meter follows closed channel fader");ok&=require(meterMixer->channelFaderGain()<0.0001f,"routing snapshot follows the audio-thread fader gain");}
 {Sine searchTone(1000);auto searchMixer=std::make_unique<DeckChannelProcessor>(&searchTone);searchMixer->prepareToPlay(512,48000);for(int i=0;i<3;++i)render(*searchMixer,512);searchMixer->setSearchMuted(true);float muted=1.0f;for(int i=0;i<4;++i)muted=render(*searchMixer,512);ok&=require(muted<0.0001f&&searchMixer->m_preFaderPeakL.load()<0.0001f,"fast transport search silences master and PFL source");searchMixer->setSearchMuted(false);float restored=0.0f;for(int i=0;i<4;++i)restored=render(*searchMixer,512);ok&=require(restored>0.1f,"fast transport search restores deck audio after release");}
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
     render(*tailMixer, 512);
     ok &= require(tailMixer->getPostFaderTailBuffer().getMagnitude(0, 0, 512) > 0.0001f,
                   "post-fader echo tail survives silent/reset deck input");
 }
 for (const auto type : {EffectType::Reverb, EffectType::Echo, EffectType::LowCutEcho,
                         EffectType::MtDelay, EffectType::Spiral}) {
     Sine effectTone(900);
     auto effectMixer = std::make_unique<DeckChannelProcessor>(&effectTone);
     effectMixer->prepareToPlay(512, 48000);
     effectMixer->setFxSlotEffectType(1, type);
     effectMixer->setFxSlotAmount(1, 0.8f);
     effectMixer->setFxSlotExternalDelayTime(1, 0.005f);
     float wetPeak = 0.0f;
     for (int i = 0; i < 80; ++i) {
         render(*effectMixer, 512);
         wetPeak = std::max(wetPeak,
             effectMixer->getPostFaderTailBuffer().getMagnitude(0, 0, 512));
     }
     ok &= require(wetPeak > 0.0001f, "tail effect produces wet audio with open fader");
     effectMixer->setFader(0.0f);
     for (int i = 0; i < 8; ++i)
         render(*effectMixer, 512);
     ok &= require(effectMixer->getPostFaderTailBuffer().getMagnitude(0, 0, 512) == 0.0f
                       && effectMixer->m_peakL.load() < 0.0001f,
                   "closed channel fader mutes dry audio and effect returns with live input");
     effectMixer->setFader(1.0f);
     for (int i = 0; i < 8; ++i)
         render(*effectMixer, 512);
     ok &= require(effectMixer->getPostFaderTailBuffer().getMagnitude(0, 0, 512) > 0.0001f,
                   "opening fader restores wet audio without resetting the effect");
 }
 {
     Sine stereoTone(700);
     auto stereoMixer = std::make_unique<DeckChannelProcessor>(&stereoTone);
     stereoMixer->prepareToPlay(512, 48000);
     stereoMixer->setFxSlotEffectType(0, EffectType::Echo);
     stereoMixer->setFxSlotAmount(0, 0.8f);
     stereoMixer->setFxSlotExternalDelayTime(0, 0.005f);
     for (int i = 0; i < 8; ++i)
         renderStereoDifference(*stereoMixer, 512);
     stereoMixer->setFxSlotExternalDelayTime(0, 0.080f);
     ok &= require(renderStereoDifference(*stereoMixer, 512) < 1.0e-6f,
                   "delay smoothing remains sample-identical across stereo channels");
 }
 {Sine concurrentTone(1000);auto concurrentMixer=std::make_unique<DeckChannelProcessor>(&concurrentTone);concurrentMixer->prepareToPlay(8192,48000);std::atomic<bool>go{false};std::thread control([&]{while(!go.load(std::memory_order_acquire)){}for(int i=0;i<2000;++i){const float v=(i%201-100)/100.f;concurrentMixer->setEq(v,-v,v*0.5f);concurrentMixer->setFilterVal(-v);}});go.store(true,std::memory_order_release);for(int i=0;i<1000;++i)ok&=require(std::isfinite(render(*concurrentMixer,64<<(i%8))),"concurrent finite");control.join();const auto s=concurrentMixer->realtimeStats();ok&=require(s.coefficientBuildsFromAudioThread+s.prepareCallsFromAudioThread+s.bufferGrowthsFromAudioThread+s.blockingLockAttempts+s.objectConstructionsFromAudioThread==0,"concurrent realtime counters zero");}
 if(std::getenv("BROCKDJ_MIXER_BENCHMARK")){double total=0,worst=0;for(int i=0;i<1000;++i){const auto s=std::chrono::steady_clock::now();render(*mixer,512);const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-s).count();total+=us;worst=std::max(worst,us);}std::cout<<"mixer 512 mean-us="<<total/1000<<" worst-us="<<worst<<'\n';}
 return ok?0:1;}
