#pragma once

#include "audio/TimeStretchProcessor.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

// Immutable, fully resident PCM. Ownership and reclamation stay on the control
// thread, under the short publication gate shared with render and seed readers.
struct SamplerSample {
    ~SamplerSample() { if (budget) budget->fetch_sub(budgetBytes, std::memory_order_relaxed); }
    std::shared_ptr<std::atomic<std::uint64_t>> budget;
    std::uint64_t budgetBytes = 0;
    juce::AudioBuffer<float> pcm;
    double sampleRate = 44100;
    double bpm = 0;
    float gain = 1;
    bool loop = false;
    bool sync = false;
};

struct SamplerBeatClock {
    double bpm = 120;
    double beat = 0;
    double timestamp = 0;
    std::uint64_t generation = 0;
};

class SamplerAudioEngine final {
public:
    static constexpr int kSlots = 32, kVoices = 8;
    enum State { Empty, Ready, Pending, Playing, Busy };
    struct PlaybackSnapshot {
        bool valid = false;
        State state = Empty;
        std::uint64_t generation = 0;
        double positionSeconds = 0, durationSeconds = 0, progress = 0, remainingBeats = 0;
    };
    PlaybackSnapshot playback(int slot) const noexcept;
    PlaybackSnapshot previewPlayback() const noexcept;
    // Control-thread publication; the preview never consumes a performance voice.
    void installPreview(std::unique_ptr<SamplerSample> sample);
    void stopPreview() noexcept { m_previewRequested.store(false, std::memory_order_release); }
    void collectStoppedPreview();
    bool isPrepared() const noexcept { return m_prepared.load(std::memory_order_acquire); }
    bool previewPlaying() const noexcept { return m_previewRequested.load() || m_previewActive.load(); }
    SamplerAudioEngine();
    ~SamplerAudioEngine();
    void prepare(int maximumBlock, double rate);
    void release();
    // Control-thread only. Replaces a slot atomically; previous playback fades.
    void install(int slot, std::unique_ptr<SamplerSample> sample);
    bool trigger(int slot, int quantizeBeats);
    void stop(int slot);
    void stopAll();
    void publishClock(const SamplerBeatClock& clock) noexcept;
    void setGain(float gain) noexcept;
    void setCue(bool cue) noexcept { m_cue.store(cue); }
    bool cue() const noexcept { return m_cue.load(); }
    int state(int slot) const noexcept;
    std::uint64_t residentBytes() const noexcept;
    TimeStretchRealtimeStats realtimeStats() const noexcept;
    void render(juce::AudioBuffer<float>& program, juce::AudioBuffer<float>& cue,
                int samples, double startTime) noexcept;
private:
    struct Voice;
    struct PlaybackCell {
        std::atomic<std::uint64_t> sequence {0}, generation {0};
        std::atomic<int> state {Empty};
        std::atomic<double> position {0}, duration {0}, progress {0}, remaining {0};
        PlaybackSnapshot read() const noexcept;
        void publish(State state, std::uint64_t generation, double position,
                     double duration, double progress, double remaining = 0) noexcept;
    };
    void publishVoice(const Voice& voice, double beat) noexcept;
    void renderPreview(juce::AudioBuffer<float>& cue, int count) noexcept;
    struct ReadGuard;
    struct Source;
    struct Command { int slot = -1; int quantize = 0; bool stop = false; std::uint64_t generation = 0; };
    bool enqueue(Command command);
    void closeGate();
    void openGate() noexcept;
    SamplerBeatClock clockSnapshot() noexcept;
    void retire(Voice& voice) noexcept;
    void start(int slot, int quantize, double beat, const SamplerBeatClock& clock) noexcept;
    std::array<std::unique_ptr<SamplerSample>, kSlots> m_samples;
    std::array<std::unique_ptr<Voice>, kVoices> m_voices;
    std::array<std::atomic<int>, kSlots> m_states {};
    std::array<PlaybackCell, kSlots> m_playback;
    PlaybackCell m_previewPlayback;
    std::unique_ptr<SamplerSample> m_previewSample;
    std::atomic<bool> m_previewRequested {false}, m_previewActive {false};
    double m_previewPosition = 0;
    float m_previewEnvelope = 0;
    std::array<float, 2> m_previewLast {}, m_previewTail {};
    int m_previewTailRemaining = 0;
    std::uint64_t m_previewGeneration = 0;
    std::array<std::atomic<std::uint64_t>, kSlots> m_generations {};
    std::atomic<bool> m_gate {false};
    std::atomic<unsigned> m_readers {0};
    std::array<Command, 256> m_commands {};
    std::atomic<unsigned> m_write {0}, m_read {0};
    std::atomic<bool> m_panic {false};
    std::atomic<float> m_gain {0.72f};
    std::atomic<bool> m_cue {false};
    std::atomic<unsigned> m_clockSequence {0};
    std::atomic<double> m_clockBpm {120}, m_clockBeat {0}, m_clockTime {0};
    std::atomic<std::uint64_t> m_clockGeneration {0};
    SamplerBeatClock m_lastClock;
    double m_rate = 44100;
    float m_smoothedGain = 0.72f;
    float m_smoothedCueGain = 0;
    std::atomic<bool> m_prepared {false};
    std::uint64_t m_order = 0;
};
