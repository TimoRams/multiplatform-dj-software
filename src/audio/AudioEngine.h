#pragma once

#include "audio/AudioOutputRouter.h"
#include "audio/AudioParameters.h"
#include "audio/DeckAudioPipeline.h"
#include "audio/HeadphoneBus.h"
#include "audio/MasterMixer.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

enum class EffectType;

struct AudioEngineRealtimeStats {
    std::uint64_t allocationsFromAudioThread = 0;
    std::uint64_t bufferGrowthsFromAudioThread = 0;
    std::uint64_t blockingLockAttempts = 0;
    std::uint64_t invalidEndpointReads = 0;
    std::uint64_t staleGenerationReads = 0;
    std::uint64_t oversizedCallbacks = 0;
    std::uint64_t silentOversizedCallbacks = 0;
    std::uint64_t nonFiniteDeckBlocks = 0;
};

class IAuxAudioEndpoint {
public:
    virtual ~IAuxAudioEndpoint() = default;
    virtual void prepareAuxAudio(int maximumBlockSize, double sampleRate) = 0;
    virtual void releaseAuxAudio() = 0;
    virtual void mixAuxAudio(juce::AudioBuffer<float>& masterBuffer,
                             juce::AudioBuffer<float>& scratchBuffer,
                             int numberOfSamples) noexcept = 0;
};

// Application-wide, audio-only summing endpoint. Registration and retirement are
// control-thread operations; getNextAudioBlock() never locks or allocates.
class AudioEngine final : public juce::AudioSource {
public:
    static constexpr std::size_t kMaximumDecks = 4;
    static constexpr int kProcessingChunkSize = 2048;

    class AuxRegistration final {
    public:
        AuxRegistration() = default;
        ~AuxRegistration();
        AuxRegistration(const AuxRegistration&) = delete;
        AuxRegistration& operator=(const AuxRegistration&) = delete;
        AuxRegistration(AuxRegistration&& other) noexcept;
        AuxRegistration& operator=(AuxRegistration&& other) noexcept;
        void reset() noexcept;
        [[nodiscard]] bool isValid() const noexcept { return m_bus != nullptr; }

    private:
        friend class AudioEngine;
        AuxRegistration(AudioEngine* bus, std::uint64_t generation,
                        IAuxAudioEndpoint* endpoint) noexcept;
        AudioEngine* m_bus = nullptr;
        IAuxAudioEndpoint* m_endpoint = nullptr;
        std::uint64_t m_generation = 0;
    };

    explicit AudioEngine(AudioPageCache& cache);
    ~AudioEngine() override;

    [[nodiscard]] AuxRegistration registerAuxEndpoint(IAuxAudioEndpoint& endpoint);
    [[nodiscard]] DeckAudioPipeline& deck(std::size_t index) noexcept;
    [[nodiscard]] const DeckAudioPipeline& deck(std::size_t index) const noexcept;
    void beginShutdown() noexcept;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    void registerCallback(juce::AudioDeviceManager& adm);
    void unregisterCallback(juce::AudioDeviceManager& adm);

    static void setMasterVolume(float v);
    static void setAntiClipEnabled(bool enabled);
    static bool antiClipEnabled();
    static float gainReduction();
    static int limiterLatencySamples();
    static double callbackAverageUsec();
    static double callbackWorstUsec();
    static uint64_t callbackCount();
    static uint64_t callbackOverrunCount();
    static void resetCallbackStats();

    static void setOutputRouting(int masterFirstCh, int boothFirstCh, int headphonesFirstCh);
    static int masterFirstChannel();
    static int boothFirstChannel();
    static int headphonesFirstChannel();

    static void setMasterCueEnabled(bool enabled);
    static bool masterCueEnabled();
    static void setHeadphoneMix(float mix);
    static float headphoneMix();
    static void setHeadphoneGain(float gain);
    static float headphoneGain();
    static void setPflEnabled(int deckIndex, bool enabled);
    static bool pflEnabled(int deckIndex);

    static void setCrossfaderPosition(float position);
    static void setCrossfaderCurve(CrossfaderCurve curve);
    static void setCrossfaderAssignment(int deckIndex, CrossfaderAssignment assignment);
    static void setMasterFx(EffectType type, float amount);
    static void setMasterFxTiming(float externalDelaySeconds, float primaryParameter);

    [[nodiscard]] float masterVuL() const noexcept
    { return m_masterPeakL.load(std::memory_order_relaxed); }
    [[nodiscard]] float masterVuR() const noexcept
    { return m_masterPeakR.load(std::memory_order_relaxed); }
    [[nodiscard]] bool masterClipDetected() const noexcept
    { return s_masterClipDetected.load(std::memory_order_relaxed); }
    static bool masterClipDetected_s();

    [[nodiscard]] AudioEngineRealtimeStats realtimeStats() const noexcept;
    [[nodiscard]] const juce::AudioBuffer<float>& masterTap() const noexcept
    { return m_masterBuf; }
    void resetRealtimeStats() noexcept;

private:
    void unregisterAux(std::uint64_t generation, IAuxAudioEndpoint* endpoint) noexcept;
    void waitForEndpointReaders() const noexcept;
    void processChunk(juce::AudioBuffer<float>& output, int outputStart, int samples,
                      const std::array<DeckAudioPipeline*, kMaximumDecks>& endpoints,
                      IAuxAudioEndpoint* aux,
                      const AudioParameters& parameters,
                      float& peakL, float& peakR,
                      float& minimumGainReduction) noexcept;

    std::array<std::unique_ptr<DeckAudioPipeline>, kMaximumDecks> m_decks;
    std::atomic<IAuxAudioEndpoint*> m_auxEndpoint { nullptr };
    std::atomic<std::uint64_t> m_auxGeneration { 0 };
    std::atomic<std::uint64_t> m_nextGeneration { 1 };
    mutable std::atomic<std::uint32_t> m_activeEndpointReaders { 0 };
    std::mutex m_registrationMutex;
    std::atomic<bool> m_shuttingDown { false };
    std::atomic<bool> m_isPrepared { false };
    std::atomic<double> m_sampleRate { 44100.0 };

    std::array<juce::AudioBuffer<float>, kMaximumDecks> m_deckBuffers;
    juce::AudioBuffer<float> m_masterBuf;
    juce::AudioBuffer<float> m_masterCueTap;
    juce::AudioBuffer<float> m_headphoneBuf;
    juce::AudioBuffer<float> m_previewScratch;
    MasterMixer m_masterMixer;
    HeadphoneBus m_headphoneBus;
    AudioOutputRouter m_outputRouter;
    juce::AudioSourcePlayer m_sourcePlayer;

    std::atomic<float> m_masterPeakL { 0.0f };
    std::atomic<float> m_masterPeakR { 0.0f };
    std::atomic<std::uint64_t> m_allocationsRt { 0 };
    std::atomic<std::uint64_t> m_bufferGrowthsRt { 0 };
    std::atomic<std::uint64_t> m_blockingLocksRt { 0 };
    std::atomic<std::uint64_t> m_invalidEndpointReads { 0 };
    std::atomic<std::uint64_t> m_staleGenerationReads { 0 };
    std::atomic<std::uint64_t> m_oversizedCallbacks { 0 };
    std::atomic<std::uint64_t> m_silentOversizedCallbacks { 0 };
    std::atomic<std::uint64_t> m_nonFiniteDeckBlocks { 0 };

    static AudioParameterStore s_parameterStore;
    static std::atomic<float> s_gainReduction;
    static std::atomic<int> s_limiterLatencySamples;
    static std::atomic<uint64_t> s_callbackCount;
    static std::atomic<uint64_t> s_callbackTotalUsec;
    static std::atomic<uint64_t> s_callbackWorstUsec;
    static std::atomic<uint64_t> s_callbackOverruns;
    static std::atomic<bool> s_masterClipDetected;
};
