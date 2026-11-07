#include "SamplerAudioEngine.h"
#include "TempoTimeline.h"
#include <algorithm>
#include <cmath>
#include <thread>

struct SamplerAudioEngine::ReadGuard {
    SamplerAudioEngine& engine;
    bool valid;
    explicit ReadGuard(SamplerAudioEngine& e) : engine(e) {
        e.m_readers.fetch_add(1, std::memory_order_seq_cst);
        valid = !e.m_gate.load(std::memory_order_seq_cst);
    }
    ~ReadGuard() { engine.m_readers.fetch_sub(1, std::memory_order_seq_cst); }
};

struct SamplerAudioEngine::Source final : juce::AudioSource, TimeStretchReadAheadSource {
    explicit Source(SamplerAudioEngine& e) : engine(e) {}
    SamplerAudioEngine& engine;
    std::atomic<const SamplerSample*> sample {nullptr};
    std::atomic<double> position {0};
    // The callback owns this timeline, including read-ahead used to snapshot
    // worker seeds. Already scheduled positions survive later tempo changes.
    TempoTimeline timeline;
    std::atomic<std::uint64_t> generation {0};
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    static float at(const SamplerSample& s, int channel, double p) noexcept {
        const int length = s.pcm.getNumSamples();
        if (length < 2) return 0;
        if (s.loop) p = std::fmod(std::fmod(p, length) + length, length);
        if (p < 0 || p >= length) return 0;
        const int a = static_cast<int>(p), b = s.loop ? (a + 1) % length : std::min(a + 1, length - 1);
        const float fraction = static_cast<float>(p - a);
        float value = s.pcm.getSample(channel, a) * (1 - fraction) + s.pcm.getSample(channel, b) * fraction;
        // A short seam blend keeps arbitrary user trims continuous without
        // changing loop length or cumulative beat phase.
        const double fade = std::min(length / 4.0, s.sampleRate * 0.003);
        if (s.loop && p < fade && fade > 1) {
            const float mix = static_cast<float>(p / fade);
            value = s.pcm.getSample(channel, length - 1) * (1 - mix) + value * mix;
        }
        return value;
    }
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        const auto* s = sample.load(std::memory_order_acquire);
        const double conversion = s ? s->sampleRate / engine.m_rate : 1;
        for (int i = 0; i < info.numSamples; ++i) {
            const double p = timeline.position(i) * conversion;
            for (int ch = 0; ch < 2; ++ch)
                info.buffer->setSample(ch, info.startSample + i, s ? at(*s, ch, p) : 0);
        }
        timeline.advance(info.numSamples);
        position.store(timeline.position(0) * conversion, std::memory_order_release);
    }
    bool readAhead(int offset, juce::AudioBuffer<float>& out, int count) noexcept override {
        ReadGuard guard(engine);
        if (!guard.valid || out.getNumSamples() < count || out.getNumChannels() < 2) return false;
        const auto* s = sample.load(std::memory_order_acquire);
        const double conversion = s ? s->sampleRate / engine.m_rate : 1;
        for (int i = 0; i < count; ++i) {
            const double p = timeline.position(offset + i) * conversion;
            for (int ch = 0; ch < 2; ++ch) out.setSample(ch, i, s ? at(*s, ch, p) : 0);
        }
        return true;
    }
    double readAheadTempo(int offset) noexcept override { return timeline.rate(offset); }
    double readAheadPosition() noexcept override { return position.load(); }
    std::uint64_t readAheadTransportGeneration() const noexcept override { return generation.load(); }
    bool readAheadPlaybackActive() const noexcept override { return sample.load() != nullptr; }
};
struct SamplerAudioEngine::Voice {
    explicit Voice(SamplerAudioEngine& e) : source(e), stretch(&source) {}
    Source source;
    TimeStretchProcessor stretch;
    juce::AudioBuffer<float> scratch;
    int slot = -1;
    bool pending = false, stopping = false;
    bool sequenced = false;
    int quantize = 0;
    double targetBeat = 0;
    std::uint64_t clockGeneration = 0, order = 0;
    float envelope = 0;
    std::array<float, 2> last {}, tail {};
    int tailRemaining = 0;
};

SamplerAudioEngine::PlaybackSnapshot SamplerAudioEngine::PlaybackCell::read() const noexcept {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto before = sequence.load(std::memory_order_acquire);
        if (before & 1) continue;
        PlaybackSnapshot result {true, static_cast<State>(state.load(std::memory_order_relaxed)),
            generation.load(std::memory_order_relaxed), position.load(std::memory_order_relaxed),
            duration.load(std::memory_order_relaxed), progress.load(std::memory_order_relaxed),
            remaining.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (before == sequence.load(std::memory_order_relaxed)) return result;
    }
    return {};
}
void SamplerAudioEngine::PlaybackCell::publish(State value, std::uint64_t revision, double pos,
                                               double length, double fraction, double wait) noexcept {
    sequence.fetch_add(1, std::memory_order_acq_rel);
    state.store(value, std::memory_order_relaxed); generation.store(revision, std::memory_order_relaxed);
    position.store(pos, std::memory_order_relaxed); duration.store(length, std::memory_order_relaxed);
    progress.store(fraction, std::memory_order_relaxed); remaining.store(wait, std::memory_order_relaxed);
    sequence.fetch_add(1, std::memory_order_release);
}
SamplerAudioEngine::PlaybackSnapshot SamplerAudioEngine::playback(int slot) const noexcept {
    return slot >= 0 && slot < kSlots ? m_playback[slot].read() : PlaybackSnapshot {};
}
SamplerAudioEngine::PlaybackSnapshot SamplerAudioEngine::previewPlayback() const noexcept { return m_previewPlayback.read(); }
void SamplerAudioEngine::publishVoice(const Voice& v, double beat) noexcept {
    if (v.slot < 0) return;
    const auto* sample = v.source.sample.load();
    if (!sample) return;
    const double frames = sample->pcm.getNumSamples();
    double position = v.source.position.load(std::memory_order_relaxed);
    position = sample->loop ? std::fmod(position, frames) : std::clamp(position, 0.0, frames);
    m_playback[v.slot].publish(v.pending ? Pending : Playing, m_generations[v.slot].load(),
        position / sample->sampleRate, frames / sample->sampleRate, position / frames,
        v.pending ? std::max(0.0, v.targetBeat - beat) : 0);
}
void SamplerAudioEngine::installPreview(std::unique_ptr<SamplerSample> sample) {
    closeGate();
    m_previewTail = m_previewLast;
    m_previewTailRemaining = m_previewActive.load() ? std::max(1, static_cast<int>(m_rate * 0.005)) : 0;
    m_previewSample = std::move(sample); m_previewPosition = 0; m_previewEnvelope = 0;
    ++m_previewGeneration;
    m_previewRequested.store(m_previewSample != nullptr);
    m_previewActive.store(m_previewSample != nullptr || m_previewTailRemaining > 0);
    m_previewPlayback.publish(m_previewSample ? Playing : Empty, m_previewGeneration, 0,
        m_previewSample ? m_previewSample->pcm.getNumSamples() / m_previewSample->sampleRate : 0, 0);
    openGate();
}
void SamplerAudioEngine::collectStoppedPreview() {
    if (previewPlaying() || !m_previewSample) return;
    closeGate();
    if (!previewPlaying()) m_previewSample.reset();
    openGate();
}
void SamplerAudioEngine::renderPreview(juce::AudioBuffer<float>& cue, int count) noexcept {
    const auto* sample = m_previewSample.get();
    const double fadeFrames = sample && !sample->loop
        ? std::max(1.0, std::min(m_rate * 0.005, sample->pcm.getNumSamples() * m_rate / sample->sampleRate / 2))
        : std::max(1.0, m_rate * 0.005);
    const float step = static_cast<float>(1 / fadeFrames);
    const double advance = sample ? sample->sampleRate / m_rate : 1;
    for (int i = 0; i < count; ++i) {
        bool play = sample && m_previewRequested.load(std::memory_order_relaxed);
        if (play && !sample->loop && sample->pcm.getNumSamples() - m_previewPosition <= fadeFrames * advance) {
            m_previewRequested.store(false, std::memory_order_relaxed); play = false;
        }
        m_previewEnvelope = std::clamp(m_previewEnvelope + (play ? step : -step), 0.0f, 1.0f);
        for (int ch = 0; ch < 2; ++ch) {
            float value = sample && m_previewEnvelope > 0
                ? Source::at(*sample, ch, m_previewPosition) * m_previewEnvelope * sample->gain * 0.72f : 0;
            if (m_previewTailRemaining > 0) value += m_previewTail[ch] * static_cast<float>(m_previewTailRemaining / std::max(1.0, m_rate * 0.005));
            if (!std::isfinite(value)) value = 0;
            cue.addSample(ch, i, value); m_previewLast[ch] = value;
        }
        if (m_previewEnvelope > 0) m_previewPosition += advance;
        if (m_previewTailRemaining > 0) --m_previewTailRemaining;
    }
    const bool active = m_previewEnvelope > 0 || m_previewRequested.load() || m_previewTailRemaining > 0;
    m_previewActive.store(active, std::memory_order_release);
    const double length = sample ? sample->pcm.getNumSamples() : 0;
    const double position = length > 0 ? (sample->loop ? std::fmod(m_previewPosition, length)
                                                     : std::min(m_previewPosition, length)) : 0;
    m_previewPlayback.publish(active ? Playing : Ready, m_previewGeneration,
        sample ? position / sample->sampleRate : 0, sample ? length / sample->sampleRate : 0,
        length > 0 ? position / length : 0);
}

SamplerAudioEngine::SamplerAudioEngine() {
    for (auto& v : m_voices) v = std::make_unique<Voice>(*this);
}
SamplerAudioEngine::~SamplerAudioEngine() { release(); }
void SamplerAudioEngine::closeGate() {
    m_gate.store(true, std::memory_order_seq_cst);
    while (m_readers.load(std::memory_order_seq_cst)) std::this_thread::yield();
}
void SamplerAudioEngine::openGate() noexcept { m_gate.store(false, std::memory_order_seq_cst); }
void SamplerAudioEngine::prepare(int block, double rate) {
    release(); closeGate(); m_rate = rate;
    for (auto& v : m_voices) {
        v->scratch.setSize(2, block);
        v->stretch.prepareToPlay(block, rate);
        v->stretch.setInputPlaybackActive(false);
    }
    m_prepared = true; openGate();
}
void SamplerAudioEngine::release() {
    closeGate();
    for (auto& v : m_voices) {
        if (v->slot >= 0) {
            m_states[v->slot].store(Ready);
            m_playback[v->slot].publish(Ready, m_generations[v->slot].load(), 0, 0, 0);
        }
        v->slot = -1; v->source.sample.store(nullptr); v->tailRemaining = 0;
        v->sequenced = false;
    }
    m_previewRequested.store(false); m_previewActive.store(false); m_previewSample.reset();
    m_previewEnvelope = 0; m_previewTailRemaining = 0;
    m_previewPlayback.publish(Empty, ++m_previewGeneration, 0, 0, 0);
    m_sequenceWasEnabled = false;
    m_lastSequenceStep = -1;
    m_prepared = false; openGate();
    // A seed worker can itself be a reader. Join only after reopening the gate.
    for (auto& v : m_voices) v->stretch.releaseResources();
    m_read.store(m_write.load());
}
void SamplerAudioEngine::install(int slot, std::unique_ptr<SamplerSample> sample) {
    if (slot < 0 || slot >= kSlots) return;
    closeGate();
    for (auto& v : m_voices) if (v->slot == slot) retire(*v);
    m_samples[slot] = std::move(sample);
    m_generations[slot].fetch_add(1, std::memory_order_release);
    m_states[slot].store(m_samples[slot] ? Ready : Empty);
    m_playback[slot].publish(m_samples[slot] ? Ready : Empty, m_generations[slot].load(), 0,
        m_samples[slot] ? m_samples[slot]->pcm.getNumSamples() / m_samples[slot]->sampleRate : 0, 0);
    openGate();
}
std::uint64_t SamplerAudioEngine::residentBytes() const noexcept {
    std::uint64_t bytes = 0;
    for (const auto& s : m_samples) if (s) bytes += static_cast<std::uint64_t>(s->pcm.getNumSamples()) * 8;
    if (m_previewSample) bytes += static_cast<std::uint64_t>(m_previewSample->pcm.getNumSamples()) * 8;
    return bytes;
}
int SamplerAudioEngine::state(int slot) const noexcept { return slot >= 0 && slot < kSlots ? m_states[slot].load() : Empty; }
bool SamplerAudioEngine::enqueue(Command c) {
    const auto write = m_write.load(std::memory_order_relaxed);
    if (write - m_read.load(std::memory_order_acquire) == m_commands.size()) return false;
    m_commands[write % m_commands.size()] = c;
    m_write.store(write + 1, std::memory_order_release); return true;
}
bool SamplerAudioEngine::trigger(int slot, int quantize) {
    return slot >= 0 && slot < kSlots && enqueue({slot, quantize, false, m_generations[slot].load(std::memory_order_acquire)});
}
void SamplerAudioEngine::stop(int slot) { if (!enqueue({slot, 0, true})) m_panic.store(true); }
void SamplerAudioEngine::stopAll() { if (!enqueue({-1, 0, true})) m_panic.store(true, std::memory_order_release); }
void SamplerAudioEngine::setSequencerPattern(int bank, int track, std::uint16_t steps) noexcept {
    if (bank < 0 || bank >= 4 || track < 0 || track >= 8) return;
    m_sequencePatterns[bank * 8 + track].store(steps, std::memory_order_release);
}
void SamplerAudioEngine::setSequencerBank(int bank) noexcept {
    if (bank >= 0 && bank < 4) m_sequenceBank.store(bank, std::memory_order_release);
}
void SamplerAudioEngine::setSequencerEnabled(bool enabled) noexcept {
    m_sequencerEnabled.store(enabled, std::memory_order_release);
}
void SamplerAudioEngine::setSequencerSwing(float swing) noexcept {
    if (std::isfinite(swing)) m_sequencerSwing.store(std::clamp(swing, 0.0f, 0.5f), std::memory_order_release);
}
TimeStretchRealtimeStats SamplerAudioEngine::realtimeStats() const noexcept {
    TimeStretchRealtimeStats result;
    for (const auto& v : m_voices) {
        const auto stats = v->stretch.realtimeStats();
        result.prepareCallsFromAudioThread += stats.prepareCallsFromAudioThread;
        result.resetCallsFromAudioThread += stats.resetCallsFromAudioThread;
        result.prewarmCallsFromAudioThread += stats.prewarmCallsFromAudioThread;
        result.bufferGrowthsFromAudioThread += stats.bufferGrowthsFromAudioThread;
        result.blockingLockAttempts += stats.blockingLockAttempts;
        result.keylockSeedsOnAudioThread += stats.keylockSeedsOnAudioThread;
        result.keylockSeeds += stats.keylockSeeds;
    }
    return result;
}
void SamplerAudioEngine::setGain(float gain) noexcept { if (std::isfinite(gain)) m_gain.store(std::clamp(gain, 0.0f, 1.5f)); }
void SamplerAudioEngine::publishClock(const SamplerBeatClock& c) noexcept {
    if (!std::isfinite(c.bpm) || c.bpm < 20 || c.bpm > 400 || !std::isfinite(c.beat) || !std::isfinite(c.timestamp)) return;
    m_clockSequence.fetch_add(1, std::memory_order_acq_rel);
    m_clockBpm.store(c.bpm); m_clockBeat.store(c.beat); m_clockTime.store(c.timestamp); m_clockGeneration.store(c.generation);
    m_clockSequence.fetch_add(1, std::memory_order_release);
}
SamplerBeatClock SamplerAudioEngine::clockSnapshot() noexcept {
    const auto before = m_clockSequence.load(std::memory_order_acquire);
    if (!(before & 1)) {
        SamplerBeatClock c {m_clockBpm.load(), m_clockBeat.load(), m_clockTime.load(), m_clockGeneration.load()};
        if (before == m_clockSequence.load(std::memory_order_acquire)) m_lastClock = c;
    }
    return m_lastClock;
}
void SamplerAudioEngine::retire(Voice& v) noexcept {
    if (v.slot >= 0) {
        m_states[v.slot].store(m_samples[v.slot] ? Ready : Empty);
        m_playback[v.slot].publish(m_samples[v.slot] ? Ready : Empty, m_generations[v.slot].load(), 0,
            m_samples[v.slot] ? m_samples[v.slot]->pcm.getNumSamples() / m_samples[v.slot]->sampleRate : 0, 0);
    }
    v.tail = v.last; v.tailRemaining = std::max(1, static_cast<int>(m_rate * 0.005));
    v.source.sample.store(nullptr); v.source.generation.fetch_add(1);
    v.stretch.setInputPlaybackActive(false);
    v.slot = -1; v.pending = false; v.sequenced = false; v.envelope = 0;
}
void SamplerAudioEngine::start(int slot, int quantize, double beat, const SamplerBeatClock& clock,
                               bool sequenced, double scheduledBeat) noexcept {
    auto* sample = m_samples[slot].get();
    if (!sample) return;
    Voice* voice = nullptr;
    bool retiredExisting = false;
    for (auto& v : m_voices) if (v->slot == slot) {
        if ((sample->loop || v->pending) && !v->stopping) {
            if (!sequenced) { v->stopping = true; return; }
            retire(*v);
            voice = v.get();
            retiredExisting = true;
            break;
        }
        voice = v.get(); break;
    }
    if (!voice) for (auto& v : m_voices) if (v->slot < 0) { voice = v.get(); break; }
    if (!voice) for (auto& v : m_voices) {
        if (!m_samples[v->slot]->loop && (!voice || v->order < voice->order)) voice = v.get();
    }
    if (!voice) {
        m_states[slot].store(Busy);
        m_playback[slot].publish(Busy, m_generations[slot].load(), 0,
            sample->pcm.getNumSamples() / sample->sampleRate, 0);
        return;
    }
    if (!retiredExisting) retire(*voice);
    voice->slot = slot; voice->stopping = false; voice->order = ++m_order;
    voice->sequenced = sequenced;
    voice->quantize = quantize;
    voice->targetBeat = sequenced ? scheduledBeat
        : quantize > 0 ? std::ceil((beat + 1e-8) / quantize) * quantize : beat;
    voice->pending = quantize > 0 || (sequenced && scheduledBeat > beat + 1e-8);
    voice->clockGeneration = clock.generation;
    voice->source.position.store(0); voice->source.sample.store(sample);
    const double ratio = sample->sync && sample->bpm > 0 ? std::clamp(clock.bpm / sample->bpm, 0.25, 4.0) : 1;
    voice->source.timeline.reset(0, ratio);
    voice->stretch.setTrackGeneration(voice->source.generation.load());
    voice->stretch.setTempoRatio(ratio); voice->stretch.setPitchLockEnabled(sample->sync);
    voice->stretch.notifyTransportDiscontinuity();
    voice->stretch.setInputPlaybackActive(!voice->pending);
    m_states[slot].store(voice->pending ? Pending : Playing);
    publishVoice(*voice, beat);
}
void SamplerAudioEngine::render(juce::AudioBuffer<float>& program, juce::AudioBuffer<float>& cueBuffer, int samples, double startTime) noexcept {
    ReadGuard guard(*this);
    if (!guard.valid || !m_prepared) return;
    const auto clock = clockSnapshot();
    const double beat = clock.beat + (startTime - clock.timestamp) * clock.bpm / 60;
    const double endBeat = beat + samples / m_rate * clock.bpm / 60;
    const auto currentStep = static_cast<std::int64_t>(std::floor(beat * 4 + 1e-8));
    const auto finalStep = static_cast<std::int64_t>(std::floor(endBeat * 4 + 1e-8));
    m_sequencerStep.store(static_cast<int>((currentStep % kSequenceSteps + kSequenceSteps) % kSequenceSteps),
                          std::memory_order_relaxed);
    const bool sequencerEnabled = m_sequencerEnabled.load(std::memory_order_acquire);
    const int sequenceBank = m_sequenceBank.load(std::memory_order_acquire);
    if (sequencerEnabled && (!m_sequenceWasEnabled || sequenceBank != m_lastSequenceBank)) {
        if (m_sequenceWasEnabled) {
            for (auto& voice : m_voices) {
                const auto* sample = voice->source.sample.load();
                if (voice->sequenced && (voice->pending || (sample && sample->loop)))
                    voice->stopping = true;
            }
        }
        m_lastSequenceStep = static_cast<std::int64_t>(std::ceil(beat * 4 - 1e-8)) - 1;
        m_sequenceWasEnabled = true;
        m_lastSequenceBank = sequenceBank;
    } else if (!sequencerEnabled && m_sequenceWasEnabled) {
        for (auto& voice : m_voices) {
            const auto* sample = voice->source.sample.load();
            if (voice->sequenced && (voice->pending || (sample && sample->loop)))
                voice->stopping = true;
        }
        m_sequenceWasEnabled = false;
        m_lastSequenceStep = finalStep;
    }
    if (sequencerEnabled) {
        const float swing = m_sequencerSwing.load(std::memory_order_relaxed);
        const auto firstStep = std::max(m_lastSequenceStep + 1, currentStep);
        for (auto step = firstStep; step <= finalStep; ++step) {
            const int patternStep = static_cast<int>((step % kSequenceSteps + kSequenceSteps) % kSequenceSteps);
            const double scheduledBeat = step * 0.25 + ((step & 1) ? swing * 0.25 : 0.0);
            const auto bit = static_cast<std::uint16_t>(1u << patternStep);
            for (int track = 0; track < 8; ++track) {
                const int slot = sequenceBank * 8 + track;
                if ((m_sequencePatterns[slot].load(std::memory_order_acquire) & bit)
                    && m_samples[slot])
                    start(slot, 0, beat, clock, true, scheduledBeat);
            }
        }
        m_lastSequenceStep = finalStep;
    } else {
        m_lastSequenceStep = finalStep;
    }
    if (m_panic.exchange(false)) {
        m_read.store(m_write.load(std::memory_order_acquire));
        for (auto& v : m_voices) v->stopping = true;
    }
    auto read = m_read.load(std::memory_order_relaxed);
    const auto write = m_write.load(std::memory_order_acquire);
    for (; read != write; ++read) {
        const auto c = m_commands[read % m_commands.size()];
        if (c.stop) { for (auto& v : m_voices) if (c.slot < 0 || v->slot == c.slot) v->stopping = true; }
        else if (c.generation == m_generations[c.slot].load(std::memory_order_acquire)) start(c.slot, c.quantize, beat, clock);
    }
    m_read.store(read, std::memory_order_release);
    const float gain = m_gain.load(std::memory_order_relaxed);
    const float step = 1.0f / std::max(1.0, m_rate * 0.005);
    const float cueTarget = m_cue.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    const float previousCueGain = m_smoothedCueGain;
    // Accumulate pre-fader sampler signal in scratch output, then apply one
    // shared gain ramp so it is independent of the number of active voices.
    const float previousGain = m_smoothedGain;
    for (auto& owned : m_voices) {
        auto& v = *owned;
        if (v.slot < 0 && v.tailRemaining <= 0) continue;
        int offset = 0;
        const auto* sample = v.source.sample.load();
        if (v.slot >= 0 && v.pending) {
            if (v.stopping) { retire(v); sample = nullptr; }
            else {
                if (clock.generation != v.clockGeneration) {
                    if (v.sequenced) {
                        const auto nextStep = static_cast<std::int64_t>(std::floor(beat * 4 + 1e-8)) + 1;
                        const float swing = m_sequencerSwing.load(std::memory_order_relaxed);
                        v.targetBeat = nextStep * 0.25 + ((nextStep & 1) ? swing * 0.25 : 0.0);
                    } else {
                        v.targetBeat = std::ceil((beat + 1e-8) / v.quantize) * v.quantize;
                    }
                    v.clockGeneration = clock.generation;
                }
                offset = static_cast<int>(std::clamp(std::ceil((v.targetBeat - beat) * 60 / clock.bpm * m_rate), 0.0, static_cast<double>(samples)));
                if (offset < samples) {
                    v.pending = false; v.stretch.setInputPlaybackActive(true); m_states[v.slot].store(Playing);
                }
            }
        }
        const int count = v.slot >= 0 && !v.pending ? samples - offset : 0;
        if (!count && v.tailRemaining <= 0) { publishVoice(v, beat + samples / m_rate * clock.bpm / 60); continue; }
        if (count && sample) {
            double ratio = sample->sync && sample->bpm > 0 ? std::clamp(clock.bpm / sample->bpm, 0.25, 4.0) : 1;
            if (sample->sync && sample->loop && !v.pending && offset == 0) {
                const double sourceBeat = v.source.position.load() / sample->sampleRate * sample->bpm / 60;
                const double error = std::remainder(beat - v.targetBeat - sourceBeat, 1.0);
                // Follow phase gradually; a new master never seeks a playing
                // sample or resets its pitch-preserving pipeline.
                ratio *= 1 + std::clamp(error * 0.1, -0.03, 0.03);
            }
            // Smooth clock/phase corrections before the vocoder sees them;
            // the ramp also applies when a new master supplies a different BPM.
            v.source.timeline.setTarget(ratio, std::max(count, static_cast<int>(m_rate * 0.025)));
            v.source.timeline.ensure(count + v.stretch.readAheadHorizon());
            v.stretch.setTempoRatio(ratio);
            v.stretch.getNextAudioBlock({&v.scratch, 0, count});
            if (!sample->loop && v.source.position.load() >= sample->pcm.getNumSamples()) v.stopping = true;
        }
        for (int i = 0; i < samples; ++i) {
            const float masterGain = previousGain + (gain - previousGain) * static_cast<float>(i + 1) / samples;
            std::array<float, 2> value {};
            if (v.tailRemaining > 0) {
                const float tailGain = v.tailRemaining / std::max(1.0, m_rate * 0.005);
                for (int ch = 0; ch < 2; ++ch) value[ch] = v.tail[ch] * tailGain;
                --v.tailRemaining;
            }
            if (count && i >= offset && v.slot >= 0 && sample) {
                v.envelope = std::clamp(v.envelope + (v.stopping ? -step : step), 0.0f, 1.0f);
                for (int ch = 0; ch < 2; ++ch) value[ch] += v.scratch.getSample(ch, i - offset) * v.envelope * sample->gain;
            }
            for (int ch = 0; ch < 2; ++ch) {
                const float finite = std::isfinite(value[ch]) ? value[ch] : 0;
                program.addSample(ch, i, finite * masterGain);
                const float cueGain = previousCueGain + (cueTarget - previousCueGain) * static_cast<float>(i + 1) / samples;
                cueBuffer.addSample(ch, i, finite * cueGain);
                v.last[ch] = finite;
            }
        }
        if (v.slot >= 0 && v.stopping && v.envelope <= 0) retire(v);
        publishVoice(v, beat + samples / m_rate * clock.bpm / 60);
    }
    renderPreview(cueBuffer, samples);
    m_smoothedGain = gain;
    m_smoothedCueGain = cueTarget;
}
