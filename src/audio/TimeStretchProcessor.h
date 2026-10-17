#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <juce_audio_basics/juce_audio_basics.h>
#include <rubberband/RubberBandStretcher.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

enum class TimeStretchBackend : std::uint8_t {
    Signalsmith,
    RubberBand,
};

struct TimeStretchRealtimeStats {
    std::uint64_t prepareCallsFromAudioThread = 0;
    std::uint64_t resetCallsFromAudioThread = 0;
    std::uint64_t prewarmCallsFromAudioThread = 0;
    std::uint64_t bufferGrowthsFromAudioThread = 0;
    std::uint64_t blockingLockAttempts = 0;
    std::uint64_t successfulPipelineSwitches = 0;
    std::uint64_t stalePreparedPipelines = 0;
    std::uint64_t preparationFailures = 0;
    // Keylock re-entry work. A seed only happens on a real transition, so a
    // rising count during steady playback means something keeps knocking the
    // stretcher out of its continuous state.
    std::uint64_t keylockSeeds = 0;
    std::uint64_t worstKeylockSeedMicros = 0;
    // Direct bridge blocks while the worker primes/catches up the stretcher.
    // The audio-thread seed counter is retained as a zero-required diagnostic.
    std::uint64_t keylockSeedBridgeBlocks = 0;
    std::uint64_t keylockSeedsOnAudioThread = 0;
};

struct TimeStretchConfiguration {
    double sampleRate = 44100.0;
    double tempoRatio = 1.0;
    int maximumBlockSize = 512;
    int channelCount = 2;
    bool keylockEnabled = false;
    TimeStretchBackend backend = TimeStretchBackend::Signalsmith;
    std::uint64_t trackGeneration = 0;
    std::uint64_t configurationGeneration = 0;
};

class TimeStretchProcessor : public juce::AudioSource {
public:
    static constexpr int kMinPullSize = 64;
    static constexpr int kMaxPullSize = 512;
    static constexpr int kFifoCapacity = 65536;
    static constexpr int kMaxPrefillSamples = 2048;
    static constexpr int kPullLoopLimit = 24;
    static constexpr int kSwitchFadeSamples = 256;

    // ── Keylock analysis settings ────────────────────────────────────────────
    // A phase vocoder's added delay is essentially its analysis window, so the
    // window length is the one knob that trades latency against how cleanly low
    // frequencies are resolved. 32 ms resolves down to roughly 30 Hz and adds
    // about 48 ms in total at 44.1 kHz with the distributed-computation hop —
    // still noticeably less than the 56 ms the previous 50 ms window cost,
    // while keeping callback CPU spikes out of the audio deadline.
    static constexpr double kKeylockWindowSeconds = 0.032;
    // Hops per window. Four is the library's own default ratio; the previous
    // eight doubled the CPU cost for no audible gain at keylock-sized shifts.
    static constexpr int kKeylockOverlap = 4;
    // Above this frequency the pitch map rolls off instead of transposing
    // linearly. Cymbals, breath and hiss are noise rather than pitch, so
    // dragging them along with the shift is what makes wide pitch ranges sound
    // artificial. Limiting the map keeps that timbre in place and costs no
    // latency at all — this is the main quality win at large shifts.
    static constexpr double kKeylockTonalityLimitHz = 8000.0;
    // How much already-played audio is kept around so a freshly activated
    // pipeline can be seeded with what the listener just heard. Big enough for
    // the longest pre-roll any supported sample rate asks for.
    static constexpr int kOutputHistorySamples = 16384;
    explicit TimeStretchProcessor(juce::AudioSource* inSource);
    ~TimeStretchProcessor() override;

    void setTempoRatio(double ratio) noexcept;
    void setPitchLockEnabled(bool enabled) noexcept;
    // A manual pitch offset (e.g. Key Shift), independent of the keylock
    // toggle: a non-zero offset routes through the stretcher pipeline even
    // when keylock is off, so it stacks on top of whatever pitch the current
    // tempo would otherwise produce.
    void setKeySemitoneOffset(double semitones) noexcept;
    // Lets the owning deck bypass the stretcher while normal transport is
    // paused. Scratch still reaches the router through the direct path.
    void setInputPlaybackActive(bool active) noexcept;
    void setBackend(TimeStretchBackend backend) noexcept;
    void setScratchBypass(bool enabled) noexcept;
    void setTrackGeneration(std::uint64_t generation) noexcept;
    void enterScratchBypass() noexcept;
    void endScratchBypass() noexcept;

    void prepareToPlay(int samplesPerBlockExpected, double sr) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept override;

    [[nodiscard]] int getLatencySamples() const noexcept;
    [[nodiscard]] TimeStretchBackend activeBackend() const noexcept;
    [[nodiscard]] TimeStretchRealtimeStats realtimeStats() const noexcept;
    [[nodiscard]] std::uint64_t activeConfigurationGeneration() const noexcept;

private:
    enum class SlotState : std::uint8_t { Empty, Preparing, Ready, Active };
    // Handshake for seeding the stretcher off the audio thread. The audio
    // thread only ever writes the snapshot while Idle and only ever renders
    // through the pipeline again once it has consumed a Ready seed, so the
    // worker owns the stretcher exclusively while it is Seeding. Ready batches
    // can be followed by catch-up batches before live rendering resumes.
    enum class SeedState : std::uint8_t { Idle, Requested, Seeding, Ready };
    struct Pipeline {
        std::unique_ptr<RubberBand::RubberBandStretcher> rubberBand;
        std::unique_ptr<signalsmith::stretch::SignalsmithStretch<float>> signalsmith;
        juce::AudioBuffer<float> input;
        juce::AudioBuffer<float> output;
        juce::AudioBuffer<float> trim;
        juce::AudioBuffer<float> zeros;
        std::unique_ptr<juce::AbstractFifo> fifo;
        TimeStretchConfiguration config;
        std::atomic<SlotState> state { SlotState::Empty };
        int prefill = 0;
        int latency = 0;
        double appliedPitchScale = 1.0;
        // Normalised tonality limit for this pipeline's sample rate. Kept here
        // because setTransposeFactor() resets the limit whenever it is called
        // without one, so every later pitch update has to pass it again.
        double tonalityLimit = 0.0;
    };

    void publishDesiredConfiguration() noexcept;
    [[nodiscard]] TimeStretchConfiguration desiredConfiguration() const noexcept;
    bool preparePipeline(Pipeline& pipeline, const TimeStretchConfiguration& config);
    void prewarmPipeline(Pipeline& pipeline);
    void workerLoop();
    void stopWorker() noexcept;
    void activatePreparedPipelineAtBlockBoundary() noexcept;
    void processPipeline(Pipeline& pipeline, const juce::AudioSourceChannelInfo& info) noexcept;
    // Refresh pitch before processing either live input or worker-side pre-roll.
    void syncPipelinePitchScale(Pipeline& pipeline) noexcept;
    void processSignalsmithPipeline(Pipeline& pipeline,
                                    const juce::AudioSourceChannelInfo& info) noexcept;
    void applySwitchFade(const juce::AudioSourceChannelInfo& info) noexcept;
    void beginTransitionFade() noexcept;
    [[nodiscard]] bool requestKeylockSeed(int slot, std::uint64_t catchupSamples = 0) noexcept;
    bool serviceSeedRequest() noexcept;
    void recordSeedDuration(std::chrono::steady_clock::time_point started) noexcept;
    void wakeWorker() noexcept
    {
        m_workerTicket.fetch_add(1, std::memory_order_release);
        m_workerTicket.notify_all();
    }
    void appendOutputHistory(const juce::AudioSourceChannelInfo& info) noexcept;
    void readOutputHistory(juce::AudioBuffer<float>& destination, int count) const noexcept;
    static bool validConfiguration(const TimeStretchConfiguration& config) noexcept;
    void resizeBuffer(juce::AudioBuffer<float>& buffer, int channels, int samples);

    juce::AudioSource* source = nullptr;
    std::array<Pipeline, 2> m_pipelines;
    std::atomic<int> m_activeSlot { -1 };
    std::atomic<double> m_targetTempoRatio { 1.0 };
    std::atomic<bool> m_pitchLockEnabled { false };
    std::atomic<double> m_keySemitoneOffset { 0.0 };
    // Standalone processors default to active because they cannot infer their
    // source state. DeckAudioPipeline publishes its transport state explicitly.
    std::atomic<bool> m_inputPlaybackActive { true };
    std::atomic<TimeStretchBackend> m_backend { TimeStretchBackend::Signalsmith };
    std::atomic<bool> m_scratchBypass { false };
    // Set whenever the stretcher has to re-enter the signal path from a
    // discontinuity (keylock toggled on, scratch released, new track). The
    // audio thread consumes it on the first block it renders through keylock.
    std::atomic<bool> m_keylockSeedPending { false };
    // Audio-thread only: whether the previous block was rendered through the
    // stretcher. Comparing it against the current decision is what detects a
    // transition without any cross-thread handshake.
    bool m_keylockRenderActive = false;
    // Seeding runs on the worker: it costs several FFT frames and would blow a
    // small callback budget. While a seed is in flight the audio thread bridges
    // through the direct path at unlocked pitch until the worker catches up,
    // rather than dropping audio or blocking on the worker.
    std::atomic<SeedState> m_seedState { SeedState::Idle };
    std::atomic<int> m_seedSlot { -1 };
    juce::AudioBuffer<float> m_seedSnapshot;
    int m_seedSnapshotLength = 0;
    bool m_seedNeedsReset = true; // published with the seed snapshot
    bool m_seedValid = false; // audio-thread-owned validity of the in-flight seed
    std::uint64_t m_seedHistoryEnd = 0;
    std::atomic<double> m_sampleRate { 44100.0 };
    std::atomic<int> m_maximumBlockSize { 512 };
    std::atomic<std::uint64_t> m_trackGeneration { 0 };
    std::atomic<std::uint64_t> m_desiredGeneration { 0 };
    std::atomic<std::uint64_t> m_activeGeneration { 0 };
    std::atomic<TimeStretchBackend> m_activeBackend { TimeStretchBackend::Signalsmith };
    std::atomic<int> m_reportedLatencySamples { 0 };
    std::atomic<int> m_switchFadeRemaining { 0 };
    juce::AudioBuffer<float> m_previousTail;
    // Ring of everything this processor has emitted. Audio-thread-owned.
    juce::AudioBuffer<float> m_outputHistory;
    int m_historyWrite = 0;
    std::uint64_t m_historySamples = 0;
    std::atomic<bool> m_accepting { false };
    std::atomic<bool> m_prepared { false };
    std::atomic<bool> m_stopRequested { false };
    std::thread m_worker;
    // Lock-free worker wakeup. The audio thread has to signal seed requests, and
    // a condition_variable notify from a callback both risks a lost wakeup and
    // touches a mutex; an atomic ticket has neither problem.
    std::atomic<std::uint32_t> m_workerTicket { 0 };

    std::atomic<std::uint64_t> m_prepareFromAudio { 0 }, m_resetFromAudio { 0 };
    std::atomic<std::uint64_t> m_prewarmFromAudio { 0 }, m_growthFromAudio { 0 };
    std::atomic<std::uint64_t> m_lockFromAudio { 0 }, m_switches { 0 };
    std::atomic<std::uint64_t> m_stale { 0 }, m_failures { 0 };
    std::atomic<std::uint64_t> m_keylockSeeds { 0 }, m_worstSeedMicros { 0 };
    std::atomic<std::uint64_t> m_seedBridgeBlocks { 0 }, m_inlineSeeds { 0 };
};
