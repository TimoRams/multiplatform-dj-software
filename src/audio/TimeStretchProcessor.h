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
    // Blocks spent on the direct path while the worker seeded the stretcher,
    // and seeds that had to run inline because the worker did not answer in
    // time. Inline seeds are the only ones that can overrun a callback.
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
    // A seed is only taken inside the callback when it fits with this much room
    // to spare, so several decks can transition in the same block without the
    // device deadline being at risk.
    static constexpr double kSeedBudgetHeadroom = 4.0;
    // For small realtime buffers (64..1024) even "affordable" seed spikes are
    // jitter-prone under real scheduler noise. Keep those transitions worker-
    // seeded and bridge briefly on the direct path instead of risking callback
    // overrun crackle.
    static constexpr int kMinimumInlineSeedBlockSamples = 2048;
    // Upper bound on how long playback may run unlocked while the worker seeds.
    static constexpr double kMaximumSeedBridgeSeconds = 0.03;

    explicit TimeStretchProcessor(juce::AudioSource* inSource);
    ~TimeStretchProcessor() override;

    void setTempoRatio(double ratio) noexcept;
    void setPitchLockEnabled(bool enabled) noexcept;
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
    // worker owns the stretcher exclusively while it is Seeding.
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
    void processSignalsmithPipeline(Pipeline& pipeline,
                                    const juce::AudioSourceChannelInfo& info) noexcept;
    void applySwitchFade(const juce::AudioSourceChannelInfo& info) noexcept;
    void beginTransitionFade() noexcept;
    void prepareKeylockTransition(Pipeline& pipeline) noexcept;
    [[nodiscard]] bool requestKeylockSeed(int slot) noexcept;
    bool serviceSeedRequest() noexcept;
    void recordSeedDuration(std::chrono::steady_clock::time_point started) noexcept;
    void wakeWorker() noexcept
    {
        m_workerTicket.fetch_add(1, std::memory_order_release);
        m_workerTicket.notify_all();
    }
    void appendOutputHistory(const juce::AudioSourceChannelInfo& info) noexcept;
    void readOutputHistory(juce::AudioBuffer<float>& destination, int count) const noexcept;
    void seedPipelineFromHistory(Pipeline& pipeline) noexcept;
    static bool validConfiguration(const TimeStretchConfiguration& config) noexcept;
    void resizeBuffer(juce::AudioBuffer<float>& buffer, int channels, int samples);

    juce::AudioSource* source = nullptr;
    std::array<Pipeline, 2> m_pipelines;
    std::atomic<int> m_activeSlot { -1 };
    std::atomic<double> m_targetTempoRatio { 1.0 };
    std::atomic<bool> m_pitchLockEnabled { false };
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
    // through the direct path, which is continuous audio at the wrong pitch for
    // a fraction of a millisecond instead of a dropout.
    std::atomic<SeedState> m_seedState { SeedState::Idle };
    std::atomic<int> m_seedSlot { -1 };
    juce::AudioBuffer<float> m_seedSnapshot;
    int m_seedSnapshotLength = 0;
    int m_seedBridgeSamples = 0;
    std::atomic<double> m_sampleRate { 44100.0 };
    std::atomic<int> m_maximumBlockSize { 512 };
    std::atomic<std::uint64_t> m_trackGeneration { 0 };
    std::atomic<std::uint64_t> m_desiredGeneration { 0 };
    std::atomic<std::uint64_t> m_activeGeneration { 0 };
    std::atomic<TimeStretchBackend> m_activeBackend { TimeStretchBackend::Signalsmith };
    std::atomic<int> m_reportedLatencySamples { 0 };
    std::atomic<int> m_switchFadeRemaining { 0 };
    juce::AudioBuffer<float> m_previousTail;
    // Ring of everything this processor has emitted, plus a linear scratch the
    // ring is unwrapped into. Only ever touched from the audio thread.
    juce::AudioBuffer<float> m_outputHistory;
    juce::AudioBuffer<float> m_historyScratch;
    int m_historyWrite = 0;
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
