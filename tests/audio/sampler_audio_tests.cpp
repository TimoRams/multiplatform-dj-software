#include "audio/SamplerAudioEngine.h"
#include "audio/SamplerMidiPickup.h"
#include <chrono>
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
    SamplerMidiPickup pickup;
    require(!pickup.accept(1, 0.5) && pickup.pending(), "startup knob cannot jump the software level");
    require(pickup.accept(0.49, 0.5), "crossing the software level acquires the knob");
    require(pickup.accept(0.9, 0.49), "an acquired knob follows later movements");
    pickup.arm();
    require(!pickup.accept(0.8, 0.2), "a touch change rearms pickup without changing gain");
    require(pickup.accept(0.2, 0.2), "reaching the touched value acquires the knob again");
    pickup.disconnect(); require(!pickup.accept(0.9, 0.2), "reconnect forgets the previous physical position");
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
    const auto played = engine.playback(0);
    require(played.valid && std::abs(played.positionSeconds - 1024.0 / 48000) < 1e-8
        && std::abs(played.durationSeconds - 2) < 1e-8, "progress reports actual rendered source frames and clip duration");
    require(engine.playback(0).positionSeconds == played.positionSeconds,
        "reading telemetry never advances playback");
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
    engine.setGain(0); engine.setCue(false); for (int i = 0; i < 3; ++i) render();
    const auto bytesBeforePreview = engine.residentBytes();
    engine.installPreview(sample(true, false, 44100));
    for (int i = 0; i < 4; ++i) render();
    require(program.getMagnitude(0, 256) == 0 && cue.getMagnitude(0, 256) > 0.1,
        "editor preview reaches only the independent headphone bus with sampler cue and level off");
    for (int i = 0; i < 8; ++i) require(engine.state(i) == SamplerAudioEngine::Playing,
        "preview never consumes or steals any of eight performance voices");
    const auto preview = engine.previewPlayback();
    require(preview.valid && preview.positionSeconds > 0 && preview.durationSeconds == 2,
        "preview has source-rate-correct progress");
    require(engine.residentBytes() > bytesBeforePreview, "preview PCM is included in resident accounting");
    auto tinyPreview = sample(false);
    tinyPreview->pcm.setSize(2,2);
    for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(tinyPreview->pcm.getWritePointer(ch),0.2f,2);
    engine.installPreview(std::move(tinyPreview)); render(64);
    require(cue.getMagnitude(0,64) < 0.35f, "replacing a preview with a two-frame clip keeps the old tail bounded");
    engine.stopPreview(); for (int i = 0; i < 4; ++i) render();
    require(!engine.previewPlaying() && cue.getMagnitude(0, 256) == 0, "preview stop fades to silence independently");
    engine.collectStoppedPreview();
    require(engine.residentBytes() == bytesBeforePreview, "stopped preview PCM is reclaimed on the control thread");
    engine.setGain(1);
    engine.install(0, {}); render();
    require(engine.state(0) == SamplerAudioEngine::Empty, "clearing a playing slot retires it safely");
    engine.install(0,sample(false)); engine.trigger(0,0); render();
    engine.install(8,sample(false)); engine.trigger(8,0); render();
    require(engine.state(0) == SamplerAudioEngine::Ready && engine.state(8) == SamplerAudioEngine::Playing,
        "full sampler replaces a one-shot while keeping all loop voices");
    engine.install(10,sample(false)); engine.trigger(10,0); engine.install(10,sample(false)); render();
    require(engine.state(10) == SamplerAudioEngine::Ready, "queued presses cannot start a replacement asset of another generation");
    engine.stopAll(); for (int i = 0; i < 3; ++i) render();

    {
        SamplerAudioEngine pending;
        pending.prepare(128, 48000); pending.install(0, sample(true, true));
        juce::AudioBuffer<float> bus(2,128), phones(2,128);
        pending.publishClock({120,0.25,0,10}); pending.trigger(0,4);
        bus.clear(); phones.clear(); pending.render(bus,phones,128,0);
        const auto before = pending.playback(0);
        require(before.state == SamplerAudioEngine::Pending && before.remainingBeats > 3.7
            && before.remainingBeats < 3.75, "pending telemetry counts down from the audio clock");
        pending.publishClock({100,7.5,128.0/48000,11});
        bus.clear(); phones.clear(); pending.render(bus,phones,128,128.0/48000);
        const auto after = pending.playback(0);
        require(after.state == SamplerAudioEngine::Pending && after.remainingBeats > 0.49
            && after.remainingBeats < 0.5 && bus.getMagnitude(0,128) == 0,
            "master handover reschedules queued onset and countdown without early audio");
        pending.stop(0); pending.render(bus,phones,128,256.0/48000);
        require(pending.state(0) == SamplerAudioEngine::Ready && pending.playback(0).remainingBeats == 0,
            "stopping a queued pad removes its countdown");
    }

    {
        SamplerAudioEngine sequencer;
        sequencer.prepare(128, 48000);
        auto kick = sample(false), hat = sample(false);
        for (int ch = 0; ch < 2; ++ch) {
            juce::FloatVectorOperations::fill(kick->pcm.getWritePointer(ch), 0.2f, kick->pcm.getNumSamples());
            juce::FloatVectorOperations::fill(hat->pcm.getWritePointer(ch), 0.1f, hat->pcm.getNumSamples());
        }
        sequencer.install(0, std::move(kick));
        sequencer.install(1, std::move(hat));
        sequencer.publishClock({120, 0, 0, 20});
        sequencer.setSequencerPattern(0, 0, 1u << 0);
        sequencer.setSequencerPattern(0, 1, 1u << 1);
        sequencer.setSequencerSwing(0.5f);
        sequencer.setSequencerEnabled(true);
        juce::AudioBuffer<float> bus(2, 128), phones(2, 128);
        double callbackTime = 0;
        auto renderSequence = [&] {
            bus.clear(); phones.clear(); sequencer.render(bus, phones, 128, callbackTime);
            callbackTime += 128.0 / 48000;
        };
        renderSequence();
        require(sequencer.state(0) == SamplerAudioEngine::Playing,
            "sequencer starts the first enabled sample step on the audio thread");
        for (int i = 0; i < 56; ++i) renderSequence();
        require(sequencer.state(1) == SamplerAudioEngine::Pending,
            "swing delays an odd sixteenth step while it waits on the audio clock");
        for (int i = 0; i < 20; ++i) renderSequence();
        require(sequencer.state(1) == SamplerAudioEngine::Playing,
            "swing-timed sample step starts at its scheduled beat");
        require(sequencer.sequencerStep() == 1,
            "sequencer publishes its current 16-step playhead");
        sequencer.setSequencerEnabled(false);
        sequencer.stopAll();
        for (int i = 0; i < 4; ++i) renderSequence();
        require(sequencer.state(0) == SamplerAudioEngine::Ready
            && sequencer.state(1) == SamplerAudioEngine::Ready,
            "sequencer stop and stop-all retire triggered voices");
        sequencer.release();
    }

    {
        bool deterministic = true, fullOnLastStep = true;
        int hits = 0;
        for (int bar = 0; bar < 2000; ++bar) {
            deterministic &= SamplerAudioEngine::fillVelocity(bar, 1, 3, 70, 100)
                == SamplerAudioEngine::fillVelocity(bar, 1, 3, 70, 100);
            fullOnLastStep &= SamplerAudioEngine::fillVelocity(bar, 3, bar % 8, 100, 90) > 0;
            hits += SamplerAudioEngine::fillVelocity(bar, 0, 2, 50, 100) > 0;
        }
        require(deterministic, "fill decisions are deterministic per bar, step and track");
        require(fullOnLastStep, "full density always fills the last sixteenth before the downbeat");
        require(hits > 300 && hits < 500, "fill density scales the hit probability");
        require(SamplerAudioEngine::fillVelocity(5, 3, 0, 0, 127) == 0
            && SamplerAudioEngine::fillVelocity(5, 3, 0, 100, 0) == 0,
            "fills stay silent without density or on empty tracks");
        require(SamplerAudioEngine::fillVelocity(7, 3, 0, 100, 100) <= 100,
            "fill hits never exceed the track's own peak velocity");

        auto positionAfterStep = [](int roll, int fillEvery, int targetStep, bool* fillActive) {
            SamplerAudioEngine sequencer;
            sequencer.prepare(128, 48000);
            sequencer.install(0, sample(false));
            sequencer.publishClock({120, 0, 0, 40});
            sequencer.setSequencerPattern(0, 0, 1u << 0);
            sequencer.setSequencerFill(fillEvery, 100);
            sequencer.setSequencerRoll(roll);
            sequencer.setSequencerEnabled(true);
            juce::AudioBuffer<float> bus(2, 128), phones(2, 128);
            int frames = 0;
            while (frames < targetStep * 6000 + 256) {
                bus.clear(); phones.clear();
                sequencer.render(bus, phones, 128, frames / 48000.0);
                frames += 128;
            }
            if (fillActive) *fillActive = sequencer.sequencerFillActive();
            const double position = sequencer.playback(0).positionSeconds;
            sequencer.setSequencerEnabled(false);
            sequencer.release();
            return position;
        };
        require(positionAfterStep(0, 0, 2, nullptr) > 0.2,
            "without roll an empty step leaves the first hit ringing");
        require(positionAfterStep(2, 0, 2, nullptr) < 0.05,
            "a two-step roll repeats the captured hit");
        bool fillActive = false;
        require(positionAfterStep(0, 0, 15, &fillActive) > 1.5 && !fillActive,
            "without fills the last beat stays empty");
        require(positionAfterStep(0, 1, 15, &fillActive) < 0.05 && fillActive,
            "an auto-fill adds a hit in the last beat of the bar");
    }

    {
        SamplerAudioEngine sequencer;
        sequencer.prepare(2048, 48000);
        sequencer.setGain(1);
        sequencer.setCue(true);
        for (int bank = 0; bank < 4; ++bank) {
            auto clip = sample(true);
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill(clip->pcm.getWritePointer(ch),
                    0.2f * (bank + 1), clip->pcm.getNumSamples());
            sequencer.install(bank * 8, std::move(clip));
            sequencer.setSequencerPattern(bank, 0, 1);
        }
        sequencer.publishClock({120, 0, 0, 30});
        sequencer.setSequencerEnabled(true);
        juce::AudioBuffer<float> bus(2, 2048), phones(2, 2048);
        int frames = 0;
        auto renderCount = [&](int count) {
            bus.clear(); phones.clear();
            sequencer.render(bus, phones, count, frames / 48000.0);
            frames += count;
        };
        auto renderTo = [&](int target) {
            while (frames < target) renderCount(std::min(1024, target - frames));
        };
        renderTo(12000);
        sequencer.setSequencerBank(1);
        require(sequencer.sequencerPendingBank() == 1 && sequencer.sequencerBank() == 0,
            "selecting a bank queues it without changing the running pattern");
        renderTo(95488);
        require(sequencer.sequencerBank() == 0 && sequencer.sequencerStep() == 15
            && sequencer.state(8) == SamplerAudioEngine::Ready,
            "all sixteen steps of the old pattern finish before switching");
        renderCount(1024);
        require(std::abs(bus.getSample(0, 500) - 0.2f) < 1e-5f
            && std::abs(bus.getSample(0, 1000) - 0.4f) < 1e-5f,
            "a pattern boundary inside a callback preserves old audio before the boundary");
        require(std::abs(bus.getSample(0, 1000) - phones.getSample(0, 1000)) < 1e-6f,
            "split callbacks preserve cue output offsets");
        require(sequencer.sequencerBank() == 1 && sequencer.sequencerPendingBank() == -1
            && sequencer.sequencerStep() == 0 && sequencer.state(0) == SamplerAudioEngine::Ready,
            "queued pattern starts at step one and retires old sequenced loops");
        sequencer.setSequencerBank(2);
        sequencer.setSequencerBank(1);
        require(sequencer.sequencerPendingBank() == -1, "selecting the running bank cancels the queue");
        sequencer.setSequencerBank(2);
        sequencer.setSequencerBank(3);
        sequencer.setSequencerLength(3, 32);
        renderTo(192001);
        require(sequencer.sequencerBank() == 3 && sequencer.state(16) == SamplerAudioEngine::Ready,
            "the latest queued bank wins without briefly playing superseded requests");
        sequencer.setSequencerBank(0);
        renderTo(288001);
        require(sequencer.sequencerBank() == 3 && sequencer.sequencerStep() == 16,
            "a 32-step pattern does not switch after only sixteen steps");
        renderTo(384001);
        require(sequencer.sequencerBank() == 0 && sequencer.sequencerStep() == 0,
            "long patterns switch at their own end");
        sequencer.stopAll();
        renderCount(1024);
        require(!sequencer.sequencerRunning() && sequencer.sequencerPendingBank() == -1,
            "stop-all also halts sequencing and clears queued status");
        const auto stats = sequencer.realtimeStats();
        require(stats.prepareCallsFromAudioThread == 0 && stats.bufferGrowthsFromAudioThread == 0
            && stats.blockingLockAttempts == 0, "pattern switching never prepares, grows or locks in the callback");
    }
    {
        SamplerAudioEngine sequencer;
        sequencer.prepare(1024, 48000); sequencer.setGain(1);
        for (int track = 0; track < 3; ++track) {
            auto clip = sample(true);
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill(clip->pcm.getWritePointer(ch), 0.2f, clip->pcm.getNumSamples());
            sequencer.install(track, std::move(clip));
        }
        sequencer.setSequencerLength(0, 64);
        sequencer.setSequencerVelocity(0, 0, 0, 32);
        sequencer.setSequencerVelocity(0, 1, 63, 127);
        sequencer.publishClock({120, 0, 0, 40}); sequencer.setSequencerEnabled(true);
        juce::AudioBuffer<float> bus(2, 1024), phones(2, 1024);
        int frames = 0;
        auto renderCount = [&](int count) {
            bus.clear(); phones.clear();
            sequencer.render(bus, phones, count, frames / 48000.0); frames += count;
        };
        renderCount(1024);
        renderCount(1024);
        require(std::abs(bus.getSample(0, 1000) - 0.2f * 32 / 127) < 1e-5f,
            "step velocity scales actual sample audio");
        sequencer.setSequencerTracks(0, 1, 0);
        renderCount(1024);
        require(sequencer.state(0) == SamplerAudioEngine::Ready,
            "mute fades an existing sequenced loop");
        sequencer.trigger(2, 0); sequencer.setSequencerTracks(0, 0, 2);
        renderCount(1024);
        require(sequencer.state(2) == SamplerAudioEngine::Playing,
            "solo only filters sequencer tracks, not manually played pads");
        while (frames < 378001) renderCount(std::min(1024, 378001 - frames));
        require(sequencer.sequencerStep() == 63 && sequencer.state(1) == SamplerAudioEngine::Playing,
            "step 64 is editable and plays without truncated masks");
        sequencer.setSequencerBank(1);
        sequencer.publishClock({120, 100, frames / 48000.0, 41});
        renderCount(1024);
        require(sequencer.sequencerBank() == 0 && sequencer.sequencerPendingBank() == 1
            && sequencer.sequencerStep() == 63,
            "clock handover preserves local pattern position and queued selection");
        while (frames < 384002) renderCount(std::min(1024, 384002 - frames));
        require(sequencer.sequencerBank() == 1, "queued change completes after handover without restarting the pattern");
    }
    {
        SamplerAudioEngine sequencer;
        sequencer.prepare(1024, 48000);
        sequencer.publishClock({120, 0, 0, 50});
        sequencer.setSequencerEnabled(true);
        juce::AudioBuffer<float> bus(2, 1024), phones(2, 1024);
        int frames = 0;
        auto renderTo = [&](int target) {
            while (frames < target) {
                const int count = std::min(1024, target - frames);
                bus.clear(); phones.clear();
                sequencer.render(bus, phones, count, frames / 48000.0); frames += count;
            }
        };
        renderTo(1);
        sequencer.setSequencerLength(0, 32);
        renderTo(96001);
        require(sequencer.sequencerStep() == 0,
            "editing length waits for the original sixteen-step cycle to complete");
        sequencer.setSequencerBank(1);
        renderTo(192001);
        require(sequencer.sequencerBank() == 0 && sequencer.sequencerStep() == 16,
            "the following cycle uses the edited thirty-two-step length");
        renderTo(288001);
        require(sequencer.sequencerBank() == 1 && sequencer.sequencerStep() == 0,
            "a queued pattern respects the newly latched cycle length");
    }

    // Exercise the actual shared pitch-preserving pipeline with resident PCM.
    engine.install(0, sample(true, true, 44100));
    const double syncStartTime = time;
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

    // A running loop must survive both fast master-tempo moves and a master
    // handover without a seek, silence, or a discontinuity at a device block.
    const auto seedsBeforeSweep = engine.realtimeStats().keylockSeeds;
    require(seedsBeforeSweep > 0, "master sweep begins with a seeded keylock pipeline");
    double beat = (time - syncStartTime) * 144.0 / 60.0;
    float maximumStep = 0;
    int quietBlocks = 0;
    previous = program.getSample(0, 255);
    nextCallback = std::chrono::steady_clock::now();
    for (int block = 0; block < 240; ++block) {
        const double elapsed = block * 256.0 / 48000;
        const double halfPeriod = elapsed < 0.6 ? 0.05 : 0.2;
        const double phase = std::fmod(elapsed / halfPeriod, 2.0);
        const double triangle = phase <= 1 ? phase : 2 - phase;
        const double bpm = 120 * (0.84 + 0.32 * triangle);
        // A new master has its own beat origin as well as a clock generation.
        if (block == 120) beat += 0.35;
        engine.publishClock({bpm, beat, time, block < 120 ? 3u : 4u});
        render();
        beat += 256.0 / 48000 * bpm / 60;
        quietBlocks += program.getRMSLevel(0, 0, 256) < 0.04f;
        for (int i = 0; i < 256; ++i) {
            const float value = program.getSample(0, i);
            require(std::isfinite(value), "master-tempo sweep samples are finite");
            maximumStep = std::max(maximumStep, std::abs(value - previous));
            previous = value;
        }
        require(engine.state(0) == SamplerAudioEngine::Playing, "master handover keeps the sync loop playing");
        nextCallback += std::chrono::microseconds(5333);
        std::this_thread::sleep_until(nextCallback);
    }
    std::cout << "sampler master-sweep maximum-step=" << maximumStep << " quiet-blocks=" << quietBlocks << '\n';
    require(maximumStep < 0.035f, "master tempo and generation changes preserve sampler waveform continuity");
    require(quietBlocks == 0, "master tempo and generation changes preserve sampler envelope");
    require(engine.realtimeStats().keylockSeeds == seedsBeforeSweep,
        "master tempo and generation changes never reseed a playing sampler loop");
    const auto stats = engine.realtimeStats();
    require(stats.prepareCallsFromAudioThread == 0 && stats.resetCallsFromAudioThread == 0
        && stats.prewarmCallsFromAudioThread == 0 && stats.bufferGrowthsFromAudioThread == 0
        && stats.blockingLockAttempts == 0 && stats.keylockSeedsOnAudioThread == 0,
        "sampler pitch processing performs no preparation, buffer growth, locks or seeds on audio thread");
    std::atomic<bool> stopping {false}, finiteOutput {true};
    std::atomic<int> renderedBlocks {0};
    std::thread audio([&] {
        juce::AudioBuffer<float> bus(2,64), headphones(2,64);
        double callbackTime = time;
        while (!stopping.load()) {
            bus.clear(); headphones.clear(); engine.render(bus,headphones,64,callbackTime);
            callbackTime += 64.0/48000;
            renderedBlocks.fetch_add(1);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 64; ++i)
                if (!std::isfinite(bus.getSample(ch,i))) finiteOutput.store(false);
        }
    });
    const auto replacementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (int replacement = 0; replacement < 32; ++replacement) {
        const int before = renderedBlocks.load();
        auto asset = std::make_unique<SamplerSample>(); asset->pcm.setSize(2,2048); asset->loop = true;
        for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(asset->pcm.getWritePointer(ch),0.05f,2048);
        engine.install(0,std::move(asset)); engine.trigger(0,0);
        while (renderedBlocks.load() <= before && std::chrono::steady_clock::now() < replacementDeadline)
            std::this_thread::yield();
        require(renderedBlocks.load() > before, "every replacement overlaps real audio callbacks");
    }
    stopping.store(true); audio.join();
    require(finiteOutput.load(), "concurrent sample replacement/retirement preserves finite audio output");
    engine.release();
    require(engine.state(0) == SamplerAudioEngine::Ready, "device release stops playback and retains sample");
    return ok ? 0 : 1;
}
