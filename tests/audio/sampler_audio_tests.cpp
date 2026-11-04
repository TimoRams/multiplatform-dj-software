#include "audio/SamplerAudioEngine.h"
#include <juce_events/juce_events.h>
#include <cmath>
#include <iostream>
#include <thread>

namespace {
bool ok = true;
void require(bool condition, const char* message) { if (!condition) { std::cerr << "FAIL: " << message << '\n'; ok = false; } }
std::unique_ptr<SamplerSample> sample(bool loop, bool sync = false, double rate = 48000) {
    auto s = std::make_unique<SamplerSample>();
    s->sampleRate = rate; s->loop = loop; s->sync = sync; s->bpm = 120;
    s->pcm.setSize(2, static_cast<int>(rate * 2));
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < s->pcm.getNumSamples(); ++i)
        s->pcm.setSample(ch, i, static_cast<float>(0.2 * std::sin(2 * juce::MathConstants<double>::pi * 440 * i / rate)));
    return s;
}
}
int main() {
    juce::ScopedJuceInitialiser_GUI runtime;
    SamplerAudioEngine engine;
    engine.prepare(256, 48000);
    juce::AudioBuffer<float> program(2, 256), cue(2, 256);
    double time = 0;
    auto render = [&](int size = 256) {
        program.clear(); cue.clear(); engine.render(program, cue, size, time); time += size/48000.0;
    };
    engine.publishClock({120, 0, 0, 1});
    engine.install(0, sample(true)); engine.setGain(1); engine.setCue(true);
    engine.trigger(0, 0);
    for (int i = 0; i < 4; ++i) render();
    require(engine.state(0) == SamplerAudioEngine::Playing, "independent sample starts with no deck");
    require(program.getMagnitude(0, 256) > 0.15, "sample has real audio");
    require(std::abs(program.getSample(0, 70) - cue.getSample(0, 70)) < 1e-6f, "unity program and cue match");
    engine.setGain(0);
    for (int i = 0; i < 3; ++i) render();
    require(program.getMagnitude(0, 256) == 0 && cue.getMagnitude(0, 256) > 0.15, "pre-fader cue remains when sampler level is zero");
    engine.trigger(0, 0);
    for (int i = 0; i < 3; ++i) render();
    require(engine.state(0) == SamplerAudioEngine::Ready, "second loop press stops");
    engine.setGain(1); engine.setCue(false);
    engine.install(0, sample(false)); engine.trigger(0, 0);
    for (int i = 0; i < 390; ++i) render();
    require(engine.state(0) == SamplerAudioEngine::Ready, "one-shot ends and frees voice");
    require(cue.getMagnitude(0, 256) == 0, "disabled cue stays silent");

    engine.release(); engine.prepare(256, 48000); time = 0;
    engine.install(0, sample(true)); engine.publishClock({120, 0.25, 0, 2}); engine.trigger(0, 1);
    int first = -1;
    for (int block = 0; block < 80; ++block) {
        render();
        for (int i = 0; i < 256; ++i) if (first < 0 && std::abs(program.getSample(0, i)) > 1e-7) first = block * 256 + i;
    }
    require(first >= 18000 && first <= 18002, "quantized start lands inside the correct audio block at the beat");
    for (const int size : {64,128}) {
        engine.stopAll(); for (int i = 0; i < 3; ++i) render();
        engine.publishClock({120,0.25,time,static_cast<std::uint64_t>(size)}); engine.trigger(0,1);
        int onset = -1;
        for (int frame = 0; frame < 20000; frame += size) {
            render(size);
            for (int i = 0; i < size; ++i) if (onset < 0 && std::abs(program.getSample(0,i)) > 1e-7) onset = frame + i;
        }
        require(onset >= 18000 && onset <= 18002, "quantized onset is stable at 64/128-frame device blocks");
    }
    engine.stopAll(); for (int i = 0; i < 3; ++i) render();
    require(program.getMagnitude(0,256) == 0, "STOP ALL silences every voice");
    for (int i = 0; i < 9; ++i) engine.install(i, sample(true));
    for (int i = 0; i < 8; ++i) engine.trigger(i, 0);
    render(); engine.trigger(8, 0); render();
    require(engine.state(8) == SamplerAudioEngine::Busy, "full loop bank rejects another voice instead of stealing a loop");
    for (int i = 0; i < 8; ++i) require(engine.state(i) == SamplerAudioEngine::Playing, "existing loops survive full capacity");
    engine.install(0, {}); render();
    require(engine.state(0) == SamplerAudioEngine::Empty, "clearing a playing slot retires it safely");
    engine.install(0,sample(false)); engine.trigger(0,0); render();
    engine.install(8,sample(false)); engine.trigger(8,0); render();
    require(engine.state(0) == SamplerAudioEngine::Ready && engine.state(8) == SamplerAudioEngine::Playing,
        "full sampler replaces a one-shot while keeping all loop voices");
    engine.install(10,sample(false)); engine.trigger(10,0); engine.install(10,sample(false)); render();
    require(engine.state(10) == SamplerAudioEngine::Ready, "queued presses cannot start a replacement asset of another generation");
    engine.stopAll(); for (int i = 0; i < 3; ++i) render();

    // Exercise the actual shared pitch-preserving pipeline with resident PCM.
    engine.install(0, sample(true, true, 44100));
    engine.publishClock({144, 0, time, 3}); engine.trigger(0, 0);
    int crossings = 0; float previous = 0; int measuredFrames = 0;
    auto nextCallback = std::chrono::steady_clock::now();
    for (int block = 0; block < 600; ++block) {
        render();
        nextCallback += std::chrono::microseconds(5333);
        std::this_thread::sleep_until(nextCallback);
        if (block > 250) for (int i = 0; i < 256; ++i) {
            const float v = program.getSample(0, i);
            require(std::isfinite(v), "sync loop samples are finite");
            if (previous <= 0 && v > 0) ++crossings;
            previous = v; ++measuredFrames;
        }
    }
    const double frequency = crossings * 48000.0 / measuredFrames;
    std::cout << "sync-loop frequency=" << frequency << '\n';
    require(frequency > 430 && frequency < 450, "tempo-synced loop preserves 440 Hz pitch across source sample-rate conversion");
    const auto stats = engine.realtimeStats();
    require(stats.prepareCallsFromAudioThread == 0 && stats.resetCallsFromAudioThread == 0
        && stats.prewarmCallsFromAudioThread == 0 && stats.bufferGrowthsFromAudioThread == 0
        && stats.blockingLockAttempts == 0 && stats.keylockSeedsOnAudioThread == 0,
        "sampler pitch processing performs no preparation, buffer growth, locks or seeds on audio thread");
    std::atomic<bool> stopping {false}, finiteOutput {true};
    std::thread audio([&] {
        juce::AudioBuffer<float> bus(2,64), headphones(2,64);
        double callbackTime = time;
        while (!stopping.load()) {
            bus.clear(); headphones.clear(); engine.render(bus,headphones,64,callbackTime);
            callbackTime += 64.0/48000;
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 64; ++i)
                if (!std::isfinite(bus.getSample(ch,i))) finiteOutput.store(false);
        }
    });
    for (int replacement = 0; replacement < 32; ++replacement) {
        auto asset = std::make_unique<SamplerSample>(); asset->pcm.setSize(2,2048); asset->loop = true;
        for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(asset->pcm.getWritePointer(ch),0.05f,2048);
        engine.install(0,std::move(asset)); engine.trigger(0,0);
    }
    stopping.store(true); audio.join();
    require(finiteOutput.load(), "concurrent sample replacement/retirement preserves finite audio output");
    engine.release();
    require(engine.state(0) == SamplerAudioEngine::Ready, "device release stops playback and retains sample");
    return ok ? 0 : 1;
}
