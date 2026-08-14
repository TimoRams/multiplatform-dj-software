#include "audio/AudioOutputRouter.h"
#include "audio/AudioRouting.h"
#include "audio/HeadphoneBus.h"
#include "audio/MasterMixer.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {

constexpr int kSamples = 512;

bool require(bool condition, std::string_view message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

void fill(juce::AudioBuffer<float>& buffer, float value)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        std::fill_n(buffer.getWritePointer(channel), buffer.getNumSamples(), value);
}

float averageTail(const juce::AudioBuffer<float>& buffer, int channel)
{
    constexpr int count = 64;
    float sum = 0.0f;
    for (int sample = buffer.getNumSamples() - count; sample < buffer.getNumSamples(); ++sample)
        sum += buffer.getSample(channel, sample);
    return sum / static_cast<float>(count);
}

bool close(float actual, float expected, float tolerance = 2.0e-3f)
{
    return std::abs(actual - expected) <= tolerance;
}

struct RoutingFixture {
    RoutingFixture()
        : deck0(2, kSamples), deck1(2, kSamples), deck2(2, kSamples), deck3(2, kSamples),
          tail0(2, kSamples), tail1(2, kSamples), tail2(2, kSamples), tail3(2, kSamples),
          pfl0(2, kSamples), pfl1(2, kSamples), pfl2(2, kSamples), pfl3(2, kSamples),
          master(2, kSamples), masterCueTap(2, kSamples),
          headphones(2, kSamples), hardware(8, kSamples)
    {
        mixer.prepare(48'000.0, kSamples);
        headphoneBus.prepare(48'000.0, kSamples);
        deck0.clear(); deck1.clear(); deck2.clear(); deck3.clear();
        tail0.clear(); tail1.clear(); tail2.clear(); tail3.clear();
        pfl0.clear(); pfl1.clear(); pfl2.clear(); pfl3.clear();
        master.clear(); masterCueTap.clear(); headphones.clear(); hardware.clear();
    }

    void render()
    {
        const std::array<const juce::AudioBuffer<float>*, 4> programs {
            &deck0, &deck1, &deck2, &deck3
        };
        const std::array<const juce::AudioBuffer<float>*, 4> tails {
            &tail0, &tail1, &tail2, &tail3
        };
        const std::array<const juce::AudioBuffer<float>*, 4> pfl {
            &pfl0, &pfl1, &pfl2, &pfl3
        };
        mixer.mixPrograms(programs, tails, parameters, master, kSamples);
        mixer.finalize(parameters, master, kSamples, &masterCueTap);
        headphoneBus.mix(pfl, masterCueTap, parameters, headphones, kSamples);
        hardware.clear();
        router.write(master, headphones, parameters, hardware, 0, kSamples);
    }

    MasterMixer mixer;
    HeadphoneBus headphoneBus;
    AudioOutputRouter router;
    AudioParameters parameters;
    juce::AudioBuffer<float> deck0, deck1, deck2, deck3;
    juce::AudioBuffer<float> tail0, tail1, tail2, tail3;
    juce::AudioBuffer<float> pfl0, pfl1, pfl2, pfl3;
    juce::AudioBuffer<float> master, masterCueTap, headphones, hardware;
};

} // namespace

int main()
{
    bool ok = true;
    static_assert(AudioRoutingConstants::kScratchCrossfadeMinSamples == 32);
    static_assert(AudioRoutingConstants::kScratchCrossfadeMaxSamples == 128);
    static_assert(AudioRoutingConstants::kCacheMissResumeCrossfadeMinSamples == 32);
    static_assert(AudioRoutingConstants::kCacheMissResumeCrossfadeMaxSamples == 128);

    RoutingFixture fixture;
    fixture.parameters.masterFirstChannel = 1;
    fixture.parameters.boothFirstChannel = 3;
    fixture.parameters.headphonesFirstChannel = 5;
    fixture.parameters.crossfaderAssignments.fill(CrossfaderAssignment::Thru);
    fixture.parameters.headphoneMix = 0.0f;

    fill(fixture.deck0, 0.0f); // closed channel fader
    fill(fixture.pfl0, 0.6f);
    fixture.parameters.pflEnabled[0] = true;
    fixture.render();
    ok &= require(close(averageTail(fixture.hardware, 0), 0.0f),
                  "Fader down keeps Master silent");
    ok &= require(close(averageTail(fixture.hardware, 4), 0.6f),
                  "Fader down plus PFL remains audible");

    fixture.parameters.masterGain = 0.25f;
    fixture.render();
    ok &= require(close(averageTail(fixture.hardware, 4), 0.6f),
                  "Master Gain does not alter PFL");

    fixture.parameters.masterGain = 1.0f;
    fixture.parameters.crossfaderAssignments = {
        CrossfaderAssignment::A, CrossfaderAssignment::B,
        CrossfaderAssignment::Thru, CrossfaderAssignment::Thru
    };
    fixture.parameters.crossfaderPosition = -1.0f;
    fill(fixture.deck0, 0.2f);
    fill(fixture.deck1, 0.4f);
    fill(fixture.deck2, 0.1f);
    fixture.render();
    ok &= require(close(averageTail(fixture.master, 0), 0.3f),
                  "Crossfader left silences B and leaves Thru untouched");

    fill(fixture.deck0, 0.0f);
    fill(fixture.deck1, 0.0f);
    fill(fixture.deck2, 0.0f);
    fill(fixture.tail0, 0.2f);
    fixture.render();
    ok &= require(close(averageTail(fixture.master, 0), 0.2f),
                  "post-fader tail continues after deck reset/fader close");
    ok &= require(close(averageTail(fixture.hardware, 0),
                        averageTail(fixture.hardware, 2)),
                  "Booth and Master are the same canonical Master Tap");

    const float recordedSample = fixture.master.getSample(0, kSamples - 1);
    ok &= require(close(recordedSample, fixture.hardware.getSample(0, kSamples - 1)),
                  "recording source and Master output are identical Master Tap samples");

    fixture.parameters.headphoneGain = 0.25f;
    fixture.render();
    ok &= require(close(averageTail(fixture.master, 0), 0.2f),
                  "Headphone Gain does not alter Master");

    // MASTER LEVEL controls only the physical Master/Booth outputs. MASTER
    // CUE listens to the pre-level master tap and must remain unchanged.
    fill(fixture.tail0, 0.0f);
    fill(fixture.deck0, 0.2f);
    fixture.parameters.crossfaderAssignments.fill(CrossfaderAssignment::Thru);
    fixture.parameters.pflEnabled.fill(false);
    fixture.parameters.headphoneGain = 1.0f;
    fixture.parameters.headphoneMix = 1.0f;
    fixture.parameters.masterCueEnabled = true;
    fixture.parameters.masterGain = 1.0f;
    fixture.render();
    fixture.render();
    const float masterCueAtUnity = averageTail(fixture.headphones, 0);
    fixture.parameters.masterGain = 0.0f;
    fixture.render();
    fixture.render();
    ok &= require(close(averageTail(fixture.master, 0), 0.0f),
                  "Master Level closes the physical Master output");
    ok &= require(close(averageTail(fixture.headphones, 0), masterCueAtUnity)
                      && masterCueAtUnity > 0.15f,
                  "Master Level does not alter Master Cue headphone volume");

    fixture.parameters.masterGain = 1.0f;
    fixture.parameters.headphoneMix = 0.0f;
    fixture.parameters.masterCueEnabled = false;
    fixture.parameters.masterFxType = static_cast<int>(EffectType::Bitcrusher);
    fixture.parameters.masterFxAmount = 1.0f;
    fill(fixture.tail0, 0.1234f);
    fixture.render();
    fixture.render();
    ok &= require(std::abs(averageTail(fixture.master, 0) - 0.1234f) > 0.01f,
                  "Master FX processes the summed signal exactly once before Master Gain");
    fixture.parameters.masterFxType = static_cast<int>(EffectType::None);
    fixture.parameters.masterFxAmount = 0.0f;

    fixture.parameters.masterFirstChannel = -1;
    fixture.parameters.boothFirstChannel = 99;
    fixture.parameters.headphonesFirstChannel = -1;
    fixture.render();
    ok &= require(close(fixture.hardware.getMagnitude(0, 0, kSamples), 0.0f)
                      && close(fixture.hardware.getMagnitude(1, 0, kSamples), 0.0f),
                  "unassigned or unavailable buses stay silent without fallback");

    std::cout << "audio routing contract tests passed\n";
    return ok ? 0 : 1;
}
