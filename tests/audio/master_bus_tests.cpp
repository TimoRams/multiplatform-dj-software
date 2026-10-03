#include "audio/AudioParameters.h"
#include "audio/AudioBusMixer.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

namespace {

void fill(juce::AudioBuffer<float>& buffer, float value)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        std::fill_n(buffer.getWritePointer(channel), buffer.getNumSamples(), value);
}

float tail(const juce::AudioBuffer<float>& buffer, int channel)
{
    float sum = 0.0f;
    for (int sample = buffer.getNumSamples() - 32; sample < buffer.getNumSamples(); ++sample)
        sum += buffer.getSample(channel, sample);
    return sum / 32.0f;
}

void parameterSnapshotIsCoherent()
{
    AudioParameterStore store;
    // Prime the fields used by this synthetic equality invariant before the
    // reader can run. Production defaults intentionally differ (for example
    // masterGain=1 and headphoneMix=0.5), so reading the untouched initial
    // snapshot made this concurrency test scheduler-dependent.
    store.update([](AudioParameters& p) {
        p.masterGain = 0.0f;
        p.headphoneMix = 0.0f;
        p.crossfaderPosition = 0.0f;
        p.masterFirstChannel = 0;
        p.boothFirstChannel = 0;
    });
    std::atomic<bool> run { true };
    std::thread writer([&] {
        for (int generation = 1; generation < 50'000; ++generation) {
            store.update([generation](AudioParameters& p) {
                const float encoded = static_cast<float>(generation % 1000) / 1000.0f;
                p.masterGain = encoded;
                p.headphoneMix = encoded;
                p.crossfaderPosition = encoded;
                p.masterFirstChannel = generation % 31;
                p.boothFirstChannel = generation % 31;
            });
        }
        run.store(false, std::memory_order_release);
    });
    while (run.load(std::memory_order_acquire)) {
        const AudioParameters p = store.snapshot();
        assert(p.masterGain == p.headphoneMix);
        assert(p.masterGain == p.crossfaderPosition);
        assert(p.masterFirstChannel == p.boothFirstChannel);
    }
    writer.join();
}

void commandQueueIsBounded()
{
    AudioCommandQueue<4> queue;
    assert(queue.push({AudioCommandType::Play}));
    assert(queue.push({AudioCommandType::Seek, 4.0}));
    assert(queue.push({AudioCommandType::Pause}));
    assert(!queue.push({AudioCommandType::ResetDeck}));
    assert(queue.droppedCommands() == 1);
    AudioCommand command;
    assert(queue.pop(command) && command.type == AudioCommandType::Play);
    assert(queue.pop(command) && command.type == AudioCommandType::Seek);
    assert(queue.pop(command) && command.type == AudioCommandType::Pause);
    assert(!queue.pop(command));
}

void mixerHeadphoneAndRouter()
{
    constexpr int samples = 512;
    juce::AudioBuffer<float> a(2, samples), b(2, samples), silent(2, samples);
    juce::AudioBuffer<float> tailA(2, samples), master(2, samples), headphones(2, samples);
    juce::AudioBuffer<float> hardware(8, samples);
    fill(a, 0.25f);
    fill(b, 0.5f);
    fill(silent, 0.0f);
    fill(tailA, 0.0f);

    std::array<const juce::AudioBuffer<float>*, 4> programs {&a, &b, &silent, &silent};
    std::array<const juce::AudioBuffer<float>*, 4> tails {&tailA, &silent, &silent, &silent};
    AudioParameters p;
    p.crossfaderAssignments = {CrossfaderAssignment::A, CrossfaderAssignment::B,
                               CrossfaderAssignment::Thru, CrossfaderAssignment::Thru};
    p.crossfaderPosition = -1.0f;
    p.masterFirstChannel = 1;
    p.boothFirstChannel = 3;
    p.headphonesFirstChannel = 5;

    MasterMixer mixer;
    mixer.prepare(48'000.0, samples);
    // The constant-power crossfader intentionally ramps from its reset gains
    // during the first 5 ms. Settle that transition before asserting a full-
    // block peak for the steady-state A-side signal.
    mixer.mixPrograms(programs, tails, p, master, samples);
    mixer.finalize(p, master, samples);
    mixer.mixPrograms(programs, tails, p, master, samples);
    mixer.finalize(p, master, samples);
    assert(std::abs(tail(master, 0) - 0.25f) < 0.002f);
    assert(std::abs(mixer.meter().finalPeakL - 0.25f) < 0.002f);
    assert(mixer.crossfaderGain(0) > 0.999f);
    assert(mixer.crossfaderGain(1) < 0.001f);

    // THRU is represented by a unity gain in the same routing graph. ON AIR
    // can therefore combine this snapshot with the channel-fader snapshot
    // without duplicating the crossfader curve in controller/UI code.
    p.crossfaderAssignments[1] = CrossfaderAssignment::Thru;
    mixer.mixPrograms(programs, tails, p, master, samples);
    assert(mixer.crossfaderGain(1) > 0.999f);
    p.crossfaderAssignments[1] = CrossfaderAssignment::B;

    // Scratch is an immediate cut while the other curves still ramp.
    mixer.reset();
    p.crossfaderPosition = -0.9f;
    p.crossfaderCurve = CrossfaderCurve::Scratch;
    mixer.mixPrograms(programs, tails, p, master, samples);
    mixer.finalize(p, master, samples);
    assert(std::abs(tail(master, 0) - 0.25f) < 0.002f);
    assert(std::abs(tail(master, 0) - tail(a, 0)) < 0.002f);

    const std::array<const juce::AudioBuffer<float>*, 4> pfl {&b, &silent, &silent, &silent};
    p.headphoneMix = 0.0f;
    p.headphoneGain = 0.5f;
    p.pflEnabled[0] = true;
    HeadphoneBus headphoneBus;
    headphoneBus.prepare(48'000.0, samples);
    // Headphone gain also ramps from its safe 1.0 startup value.
    headphoneBus.mix(pfl, master, p, headphones, samples);
    headphoneBus.mix(pfl, master, p, headphones, samples);
    assert(std::abs(tail(headphones, 0) - 0.25f) < 0.01f);
    assert(std::abs(tail(master, 0) - 0.25f) < 0.002f);

    AudioOutputRouter router;
    hardware.clear();
    router.write(master, headphones, p, hardware, 0, samples);
    assert(std::abs(tail(hardware, 0) - tail(hardware, 2)) < 0.002f);
    assert(std::abs(tail(hardware, 4) - tail(headphones, 0)) < 0.002f);

    p.limiterEnabled = true;
    fill(a, 8.0f);
    mixer.mixPrograms(programs, tails, p, master, samples);
    mixer.finalize(p, master, samples);
    assert(mixer.meter().preLimiterPeakL > 1.0f);
    assert(mixer.meter().finalPeakL <= 1.001f);
    assert(mixer.meter().minimumGainReduction < 1.0f);
}

void outputRouterPreservesSampleContract()
{
    juce::AudioBuffer<float> mono(1, 3);
    juce::AudioBuffer<float> headphones(2, 3);
    juce::AudioBuffer<float> hardware(4, 5);
    mono.setSample(0, 0, std::numeric_limits<float>::quiet_NaN());
    mono.setSample(0, 1, 0.25f);
    mono.setSample(0, 2, std::numeric_limits<float>::infinity());
    headphones.clear();
    hardware.clear();

    AudioParameters parameters;
    parameters.masterFirstChannel = 2;
    parameters.boothFirstChannel = 0;
    parameters.headphonesFirstChannel = 0;

    AudioOutputRouter router;
    router.write(mono, headphones, parameters, hardware, 1, 3);
    assert(hardware.getSample(1, 0) == 0.0f && hardware.getSample(2, 0) == 0.0f);
    assert(hardware.getSample(1, 1) == 0.0f && hardware.getSample(2, 1) == 0.0f);
    assert(hardware.getSample(1, 2) == 0.25f && hardware.getSample(2, 2) == 0.25f);
    assert(hardware.getSample(1, 3) == 0.0f && hardware.getSample(2, 3) == 0.0f);
    assert(hardware.getSample(1, 4) == 0.0f && hardware.getSample(2, 4) == 0.0f);

    hardware.clear();
    parameters.masterFirstChannel = 4;
    router.write(mono, headphones, parameters, hardware, 0, 3);
    router.write(mono, headphones, parameters, hardware, 4, 3);
    for (int channel = 0; channel < hardware.getNumChannels(); ++channel)
        assert(hardware.getMagnitude(channel, 0, hardware.getNumSamples()) == 0.0f);
}

void performanceSmoke()
{
    MasterMixer mixer;
    constexpr int maxSamples = 2048;
    mixer.prepare(48'000.0, maxSamples);
    std::array<juce::AudioBuffer<float>, 4> decks {
        juce::AudioBuffer<float>(2, maxSamples), juce::AudioBuffer<float>(2, maxSamples),
        juce::AudioBuffer<float>(2, maxSamples), juce::AudioBuffer<float>(2, maxSamples)
    };
    for (auto& deck : decks)
        fill(deck, 0.05f);
    std::array<const juce::AudioBuffer<float>*, 4> inputs {
        &decks[0], &decks[1], &decks[2], &decks[3]
    };
    juce::AudioBuffer<float> output(2, maxSamples);
    AudioParameters p;
    p.crossfaderAssignments.fill(CrossfaderAssignment::Thru);
    const auto start = std::chrono::steady_clock::now();
    for (int block = 0; block < 5'000; ++block) {
        const int samples = (block & 1) == 0 ? 64 : 128;
        mixer.mixPrograms(inputs, inputs, p, output, samples);
        mixer.finalize(p, output, samples);
        assert(std::isfinite(output.getSample(0, samples - 1)));
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "master components 5000-block smoke: " << elapsed << " ms\n";
}

} // namespace

int main()
{
    parameterSnapshotIsCoherent();
    commandQueueIsBounded();
    mixerHeadphoneAndRouter();
    outputRouterPreservesSampleContract();
    performanceSmoke();
    std::cout << "master component tests passed\n";
    return 0;
}
