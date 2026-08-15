#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
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
    // about 40 ms in total at 44.1 kHz — noticeably less than the 56 ms the
    // previous 50 ms window cost, and short enough to stay out of the way when
    // riding the tempo fader.
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
    void applySwitchFade(const juce::AudioSourceChannelInfo& info) noexcept;
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
    std::atomic<bool> m_scratchExitRequested { false };
    std::atomic<bool> m_scratchExitFadePending { false };
    std::atomic<bool> m_scratchRefreshInFlight { false };
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
    std::mutex m_workerMutex;
    std::condition_variable m_workerWake;

    std::atomic<std::uint64_t> m_prepareFromAudio { 0 }, m_resetFromAudio { 0 };
    std::atomic<std::uint64_t> m_prewarmFromAudio { 0 }, m_growthFromAudio { 0 };
    std::atomic<std::uint64_t> m_lockFromAudio { 0 }, m_switches { 0 };
    std::atomic<std::uint64_t> m_stale { 0 }, m_failures { 0 };
};
