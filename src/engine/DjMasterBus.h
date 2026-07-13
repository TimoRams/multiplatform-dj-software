#pragma once

#include "MasterBusAudioEndpoint.h"
#include "../fx/BrickwallLimiter.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

struct MasterBusRealtimeStats {
    std::uint64_t allocationsFromAudioThread = 0;
    std::uint64_t bufferGrowthsFromAudioThread = 0;
    std::uint64_t blockingLockAttempts = 0;
    std::uint64_t invalidEndpointReads = 0;
    std::uint64_t staleGenerationReads = 0;
    std::uint64_t oversizedCallbacks = 0;
    std::uint64_t silentOversizedCallbacks = 0;
    std::uint64_t nonFiniteDeckBlocks = 0;
};

// Application-wide, audio-only summing endpoint. Registration and retirement are
// control-thread operations; getNextAudioBlock() never locks or allocates.
class DjMasterBus final : public juce::AudioSource {
public:
    static constexpr std::size_t kMaximumDecks = 4;
    static constexpr int kProcessingChunkSize = 2048;

    class DeckRegistration final {
    public:
        DeckRegistration() = default;
        ~DeckRegistration();
        DeckRegistration(const DeckRegistration&) = delete;
        DeckRegistration& operator=(const DeckRegistration&) = delete;
        DeckRegistration(DeckRegistration&& other) noexcept;
        DeckRegistration& operator=(DeckRegistration&& other) noexcept;
        void reset() noexcept;
        [[nodiscard]] bool isValid() const noexcept { return m_bus != nullptr; }

    private:
        friend class DjMasterBus;
        DeckRegistration(DjMasterBus* bus, std::size_t slot, std::uint64_t generation,
                         IDeckAudioEndpoint* endpoint) noexcept;
        DjMasterBus* m_bus = nullptr;
        IDeckAudioEndpoint* m_endpoint = nullptr;
        std::size_t m_slot = 0;
        std::uint64_t m_generation = 0;
    };

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
        friend class DjMasterBus;
        AuxRegistration(DjMasterBus* bus, std::uint64_t generation,
                        IMasterBusAuxEndpoint* endpoint) noexcept;
        DjMasterBus* m_bus = nullptr;
        IMasterBusAuxEndpoint* m_endpoint = nullptr;
        std::uint64_t m_generation = 0;
    };

    DjMasterBus();
    ~DjMasterBus() override;

    [[nodiscard]] DeckRegistration registerDeck(IDeckAudioEndpoint& endpoint, int slotIndex);
    [[nodiscard]] AuxRegistration registerAuxEndpoint(IMasterBusAuxEndpoint& endpoint);
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

    [[nodiscard]] float masterVuL() const noexcept
    { return m_masterPeakL.load(std::memory_order_relaxed); }
    [[nodiscard]] float masterVuR() const noexcept
    { return m_masterPeakR.load(std::memory_order_relaxed); }
    [[nodiscard]] bool masterClipDetected() const noexcept
    { return s_masterClipDetected.load(std::memory_order_relaxed); }
    static bool masterClipDetected_s();

    [[nodiscard]] MasterBusRealtimeStats realtimeStats() const noexcept;
    void resetRealtimeStats() noexcept;

private:
    struct DeckSlot {
        std::atomic<IDeckAudioEndpoint*> endpoint { nullptr };
        std::atomic<std::uint64_t> generation { 0 };
    };

    void unregisterDeck(std::size_t slot, std::uint64_t generation,
                        IDeckAudioEndpoint* endpoint) noexcept;
    void unregisterAux(std::uint64_t generation, IMasterBusAuxEndpoint* endpoint) noexcept;
    void waitForEndpointReaders() const noexcept;
    void processChunk(juce::AudioBuffer<float>& output, int outputStart, int samples,
                      const std::array<IDeckAudioEndpoint*, kMaximumDecks>& endpoints,
                      IMasterBusAuxEndpoint* aux, float& peakL, float& peakR,
                      float& minimumGainReduction) noexcept;
    static void routeStereoToPair(juce::AudioBuffer<float>& buffer,
                                  const float* srcL, const float* srcR,
                                  int start, int n, int firstChannel,
                                  bool add = false, float gain = 1.0f) noexcept;

    std::array<DeckSlot, kMaximumDecks> m_deckSlots;
    std::atomic<IMasterBusAuxEndpoint*> m_auxEndpoint { nullptr };
    std::atomic<std::uint64_t> m_auxGeneration { 0 };
    std::atomic<std::uint64_t> m_nextGeneration { 1 };
    mutable std::atomic<std::uint32_t> m_activeEndpointReaders { 0 };
    std::mutex m_registrationMutex;
    std::atomic<bool> m_shuttingDown { false };
    std::atomic<bool> m_isPrepared { false };
    std::atomic<double> m_sampleRate { 44100.0 };

    juce::AudioBuffer<float> m_deckScratch;
    juce::AudioBuffer<float> m_masterBuf;
    juce::AudioBuffer<float> m_previewScratch;
    BrickwallLimiter m_limiter;
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

    static std::atomic<float> s_masterVolume;
    static std::atomic<bool> s_antiClipEnabled;
    static std::atomic<float> s_gainReduction;
    static std::atomic<int> s_limiterLatencySamples;
    static std::atomic<uint64_t> s_callbackCount;
    static std::atomic<uint64_t> s_callbackTotalUsec;
    static std::atomic<uint64_t> s_callbackWorstUsec;
    static std::atomic<uint64_t> s_callbackOverruns;
    static std::atomic<int> s_masterFirstChannel;
    static std::atomic<int> s_boothFirstChannel;
    static std::atomic<int> s_headphonesFirstChannel;
    static std::atomic<bool> s_masterCueEnabled;
    static std::atomic<float> s_headphoneMix;
    static std::atomic<bool> s_masterClipDetected;
};
