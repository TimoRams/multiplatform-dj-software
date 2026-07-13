#include "DjMasterBus.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

std::atomic<float> DjMasterBus::s_masterVolume { 1.0f };
std::atomic<bool> DjMasterBus::s_antiClipEnabled { false };
std::atomic<float> DjMasterBus::s_gainReduction { 1.0f };
std::atomic<int> DjMasterBus::s_limiterLatencySamples { 0 };
std::atomic<uint64_t> DjMasterBus::s_callbackCount { 0 };
std::atomic<uint64_t> DjMasterBus::s_callbackTotalUsec { 0 };
std::atomic<uint64_t> DjMasterBus::s_callbackWorstUsec { 0 };
std::atomic<uint64_t> DjMasterBus::s_callbackOverruns { 0 };
std::atomic<int> DjMasterBus::s_masterFirstChannel { 1 };
std::atomic<int> DjMasterBus::s_boothFirstChannel { -1 };
std::atomic<int> DjMasterBus::s_headphonesFirstChannel { -1 };
std::atomic<bool> DjMasterBus::s_masterCueEnabled { false };
std::atomic<float> DjMasterBus::s_headphoneMix { 0.5f };
std::atomic<bool> DjMasterBus::s_masterClipDetected { false };

namespace {

class EndpointReadGuard final {
public:
    explicit EndpointReadGuard(std::atomic<std::uint32_t>& readers) noexcept
        : m_readers(readers)
    {
        m_readers.fetch_add(1, std::memory_order_seq_cst);
    }
    ~EndpointReadGuard()
    {
        m_readers.fetch_sub(1, std::memory_order_seq_cst);
    }

private:
    std::atomic<std::uint32_t>& m_readers;
};

bool sanitizeStereo(juce::AudioBuffer<float>& buffer, int samples) noexcept
{
    bool foundNonFinite = false;
    for (int channel = 0; channel < std::min(2, buffer.getNumChannels()); ++channel) {
        float* data = buffer.getWritePointer(channel);
        for (int sample = 0; sample < samples; ++sample) {
            if (!std::isfinite(data[sample])) {
                data[sample] = 0.0f;
                foundNonFinite = true;
            }
        }
    }
    return foundNonFinite;
}

bool containsNonFiniteStereo(const juce::AudioBuffer<float>& buffer, int samples) noexcept
{
    if (buffer.getNumChannels() < 1 || buffer.getNumSamples() < samples)
        return false;
    for (int channel = 0; channel < std::min(2, buffer.getNumChannels()); ++channel) {
        const float* data = buffer.getReadPointer(channel);
        for (int sample = 0; sample < samples; ++sample)
            if (!std::isfinite(data[sample]))
                return true;
    }
    return false;
}

} // namespace

DjMasterBus::DeckRegistration::DeckRegistration(DjMasterBus* bus, std::size_t slot,
                                                 std::uint64_t generation,
                                                 IDeckAudioEndpoint* endpoint) noexcept
    : m_bus(bus), m_endpoint(endpoint), m_slot(slot), m_generation(generation)
{
}

DjMasterBus::DeckRegistration::~DeckRegistration()
{
    reset();
}

DjMasterBus::DeckRegistration::DeckRegistration(DeckRegistration&& other) noexcept
    : m_bus(std::exchange(other.m_bus, nullptr))
    , m_endpoint(std::exchange(other.m_endpoint, nullptr))
    , m_slot(std::exchange(other.m_slot, 0))
    , m_generation(std::exchange(other.m_generation, 0))
{
}

DjMasterBus::DeckRegistration& DjMasterBus::DeckRegistration::operator=(
    DeckRegistration&& other) noexcept
{
    if (this != &other) {
        reset();
        m_bus = std::exchange(other.m_bus, nullptr);
        m_endpoint = std::exchange(other.m_endpoint, nullptr);
        m_slot = std::exchange(other.m_slot, 0);
        m_generation = std::exchange(other.m_generation, 0);
    }
    return *this;
}

void DjMasterBus::DeckRegistration::reset() noexcept
{
    if (m_bus)
        m_bus->unregisterDeck(m_slot, m_generation, m_endpoint);
    m_bus = nullptr;
    m_endpoint = nullptr;
    m_slot = 0;
    m_generation = 0;
}

DjMasterBus::AuxRegistration::AuxRegistration(DjMasterBus* bus, std::uint64_t generation,
                                               IMasterBusAuxEndpoint* endpoint) noexcept
    : m_bus(bus), m_endpoint(endpoint), m_generation(generation)
{
}

DjMasterBus::AuxRegistration::~AuxRegistration()
{
    reset();
}

DjMasterBus::AuxRegistration::AuxRegistration(AuxRegistration&& other) noexcept
    : m_bus(std::exchange(other.m_bus, nullptr))
    , m_endpoint(std::exchange(other.m_endpoint, nullptr))
    , m_generation(std::exchange(other.m_generation, 0))
{
}

DjMasterBus::AuxRegistration& DjMasterBus::AuxRegistration::operator=(
    AuxRegistration&& other) noexcept
{
    if (this != &other) {
        reset();
        m_bus = std::exchange(other.m_bus, nullptr);
        m_endpoint = std::exchange(other.m_endpoint, nullptr);
        m_generation = std::exchange(other.m_generation, 0);
    }
    return *this;
}

void DjMasterBus::AuxRegistration::reset() noexcept
{
    if (m_bus)
        m_bus->unregisterAux(m_generation, m_endpoint);
    m_bus = nullptr;
    m_endpoint = nullptr;
    m_generation = 0;
}

DjMasterBus::DjMasterBus() = default;

DjMasterBus::~DjMasterBus()
{
    beginShutdown();
    m_sourcePlayer.setSource(nullptr);
}

DjMasterBus::DeckRegistration DjMasterBus::registerDeck(IDeckAudioEndpoint& endpoint,
                                                         int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= static_cast<int>(kMaximumDecks))
        return {};

    std::lock_guard lock(m_registrationMutex);
    if (m_shuttingDown.load(std::memory_order_acquire))
        return {};
    for (const auto& slot : m_deckSlots)
        if (slot.endpoint.load(std::memory_order_acquire) == &endpoint)
            return {};

    auto& slot = m_deckSlots[static_cast<std::size_t>(slotIndex)];
    if (slot.endpoint.load(std::memory_order_acquire))
        return {};

    if (m_isPrepared.load(std::memory_order_acquire))
        endpoint.prepareToPlay(kProcessingChunkSize,
                               m_sampleRate.load(std::memory_order_acquire));

    const std::uint64_t generation = m_nextGeneration.fetch_add(1, std::memory_order_relaxed);
    slot.generation.store(generation, std::memory_order_release);
    slot.endpoint.store(&endpoint, std::memory_order_release);
    return {this, static_cast<std::size_t>(slotIndex), generation, &endpoint};
}

DjMasterBus::AuxRegistration DjMasterBus::registerAuxEndpoint(IMasterBusAuxEndpoint& endpoint)
{
    std::lock_guard lock(m_registrationMutex);
    if (m_shuttingDown.load(std::memory_order_acquire)
        || m_auxEndpoint.load(std::memory_order_acquire))
        return {};

    if (m_isPrepared.load(std::memory_order_acquire))
        endpoint.prepareAuxAudio(kProcessingChunkSize,
                                 m_sampleRate.load(std::memory_order_acquire));

    const std::uint64_t generation = m_nextGeneration.fetch_add(1, std::memory_order_relaxed);
    m_auxGeneration.store(generation, std::memory_order_release);
    m_auxEndpoint.store(&endpoint, std::memory_order_release);
    return {this, generation, &endpoint};
}

void DjMasterBus::unregisterDeck(std::size_t slotIndex, std::uint64_t generation,
                                 IDeckAudioEndpoint* endpoint) noexcept
{
    if (slotIndex >= kMaximumDecks || !endpoint)
        return;
    {
        std::lock_guard lock(m_registrationMutex);
        auto& slot = m_deckSlots[slotIndex];
        if (slot.generation.load(std::memory_order_acquire) != generation
            || slot.endpoint.load(std::memory_order_acquire) != endpoint)
            return;
        slot.endpoint.store(nullptr, std::memory_order_seq_cst);
        slot.generation.fetch_add(1, std::memory_order_acq_rel);
    }
    waitForEndpointReaders();
}

void DjMasterBus::unregisterAux(std::uint64_t generation,
                                IMasterBusAuxEndpoint* endpoint) noexcept
{
    if (!endpoint)
        return;
    {
        std::lock_guard lock(m_registrationMutex);
        if (m_auxGeneration.load(std::memory_order_acquire) != generation
            || m_auxEndpoint.load(std::memory_order_acquire) != endpoint)
            return;
        m_auxEndpoint.store(nullptr, std::memory_order_seq_cst);
        m_auxGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    waitForEndpointReaders();
}

void DjMasterBus::waitForEndpointReaders() const noexcept
{
    while (m_activeEndpointReaders.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
}

void DjMasterBus::beginShutdown() noexcept
{
    m_shuttingDown.store(true, std::memory_order_release);
    {
        std::lock_guard lock(m_registrationMutex);
        for (auto& slot : m_deckSlots) {
            slot.endpoint.store(nullptr, std::memory_order_seq_cst);
            slot.generation.fetch_add(1, std::memory_order_acq_rel);
        }
        m_auxEndpoint.store(nullptr, std::memory_order_seq_cst);
        m_auxGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    waitForEndpointReaders();
}

void DjMasterBus::registerCallback(juce::AudioDeviceManager& adm)
{
    m_sourcePlayer.setSource(this);
    adm.addAudioCallback(&m_sourcePlayer);
}

void DjMasterBus::unregisterCallback(juce::AudioDeviceManager& adm)
{
    adm.removeAudioCallback(&m_sourcePlayer);
    m_sourcePlayer.setSource(nullptr);
    waitForEndpointReaders();
}

void DjMasterBus::prepareToPlay(int, double sampleRate)
{
    const double validRate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    m_sampleRate.store(validRate, std::memory_order_release);

    m_deckScratch.setSize(2, kProcessingChunkSize, false, true, true);
    m_masterBuf.setSize(2, kProcessingChunkSize, false, true, true);
    m_previewScratch.setSize(2, kProcessingChunkSize, false, true, true);
    m_limiter.prepare(validRate, kProcessingChunkSize, 2);
    s_limiterLatencySamples.store(m_limiter.getLookaheadSamples(), std::memory_order_relaxed);

    EndpointReadGuard guard(m_activeEndpointReaders);
    for (auto& slot : m_deckSlots)
        if (auto* endpoint = slot.endpoint.load(std::memory_order_seq_cst))
            endpoint->prepareToPlay(kProcessingChunkSize, validRate);
    if (auto* aux = m_auxEndpoint.load(std::memory_order_seq_cst))
        aux->prepareAuxAudio(kProcessingChunkSize, validRate);
    m_isPrepared.store(true, std::memory_order_release);
}

void DjMasterBus::releaseResources()
{
    m_isPrepared.store(false, std::memory_order_release);
    EndpointReadGuard guard(m_activeEndpointReaders);
    for (auto& slot : m_deckSlots)
        if (auto* endpoint = slot.endpoint.load(std::memory_order_seq_cst))
            endpoint->releaseResources();
    if (auto* aux = m_auxEndpoint.load(std::memory_order_seq_cst))
        aux->releaseAuxAudio();
}

void DjMasterBus::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    const auto callbackStartTicks = juce::Time::getHighResolutionTicks();
    auto* output = bufferToFill.buffer;
    const int start = bufferToFill.startSample;
    const int totalSamples = bufferToFill.numSamples;
    if (!output || totalSamples <= 0 || start < 0
        || start > output->getNumSamples()
        || totalSamples > output->getNumSamples() - start)
        return;

    for (int channel = 0; channel < output->getNumChannels(); ++channel)
        output->clear(channel, start, totalSamples);
    if (!m_isPrepared.load(std::memory_order_acquire))
        return;

    if (totalSamples > kProcessingChunkSize)
        m_oversizedCallbacks.fetch_add(1, std::memory_order_relaxed);

    EndpointReadGuard guard(m_activeEndpointReaders);
    std::array<IDeckAudioEndpoint*, kMaximumDecks> endpoints {};
    for (std::size_t index = 0; index < kMaximumDecks; ++index) {
        const auto generationBefore = m_deckSlots[index].generation.load(std::memory_order_acquire);
        auto* endpoint = m_deckSlots[index].endpoint.load(std::memory_order_seq_cst);
        const auto generationAfter = m_deckSlots[index].generation.load(std::memory_order_acquire);
        if (endpoint && generationBefore == generationAfter && generationBefore != 0)
            endpoints[index] = endpoint;
        else if (endpoint)
            m_staleGenerationReads.fetch_add(1, std::memory_order_relaxed);
    }
    const auto auxGenerationBefore = m_auxGeneration.load(std::memory_order_acquire);
    auto* aux = m_auxEndpoint.load(std::memory_order_seq_cst);
    const auto auxGenerationAfter = m_auxGeneration.load(std::memory_order_acquire);
    if (aux && (auxGenerationBefore == 0 || auxGenerationBefore != auxGenerationAfter)) {
        m_staleGenerationReads.fetch_add(1, std::memory_order_relaxed);
        aux = nullptr;
    }

    float callbackPeakL = 0.0f;
    float callbackPeakR = 0.0f;
    float minimumGainReduction = 1.0f;
    for (int offset = 0; offset < totalSamples; offset += kProcessingChunkSize) {
        const int chunkSamples = std::min(kProcessingChunkSize, totalSamples - offset);
        processChunk(*output, start + offset, chunkSamples, endpoints, aux,
                     callbackPeakL, callbackPeakR, minimumGainReduction);
    }

    m_masterPeakL.store(callbackPeakL, std::memory_order_relaxed);
    m_masterPeakR.store(callbackPeakR, std::memory_order_relaxed);
    s_masterClipDetected.store(callbackPeakL > 1.001f || callbackPeakR > 1.001f,
                               std::memory_order_relaxed);
    s_gainReduction.store(s_antiClipEnabled.load(std::memory_order_relaxed)
                              ? minimumGainReduction : 1.0f,
                          std::memory_order_relaxed);

    const auto elapsedTicks = juce::Time::getHighResolutionTicks() - callbackStartTicks;
    const auto frequency = juce::Time::getHighResolutionTicksPerSecond();
    const uint64_t elapsedUsec = frequency > 0
        ? static_cast<uint64_t>((static_cast<long double>(elapsedTicks) * 1000000.0L)
                                / static_cast<long double>(frequency))
        : 0;
    s_callbackCount.fetch_add(1, std::memory_order_relaxed);
    s_callbackTotalUsec.fetch_add(elapsedUsec, std::memory_order_relaxed);
    uint64_t observedWorst = s_callbackWorstUsec.load(std::memory_order_relaxed);
    while (elapsedUsec > observedWorst
           && !s_callbackWorstUsec.compare_exchange_weak(observedWorst, elapsedUsec,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
    }
    const double rate = m_sampleRate.load(std::memory_order_relaxed);
    if (rate > 0.0) {
        const uint64_t budgetUsec = static_cast<uint64_t>(
            static_cast<double>(totalSamples) * 1000000.0 / rate);
        if (budgetUsec > 0 && elapsedUsec > budgetUsec)
            s_callbackOverruns.fetch_add(1, std::memory_order_relaxed);
    }
}

void DjMasterBus::processChunk(
    juce::AudioBuffer<float>& output, int outputStart, int samples,
    const std::array<IDeckAudioEndpoint*, kMaximumDecks>& endpoints,
    IMasterBusAuxEndpoint* aux, float& peakL, float& peakR,
    float& minimumGainReduction) noexcept
{
    m_masterBuf.clear(0, 0, samples);
    m_masterBuf.clear(1, 0, samples);

    for (auto* endpoint : endpoints) {
        if (!endpoint)
            continue;
        m_deckScratch.clear(0, 0, samples);
        m_deckScratch.clear(1, 0, samples);
        juce::AudioSourceChannelInfo info(&m_deckScratch, 0, samples);
        endpoint->getNextAudioBlock(info);
        bool nonFinite = sanitizeStereo(m_deckScratch, samples);
        const auto& preFader = endpoint->preFaderBuffer();
        if (preFader.getNumChannels() < 1 || preFader.getNumSamples() < samples) {
            m_invalidEndpointReads.fetch_add(1, std::memory_order_relaxed);
        } else {
            nonFinite = containsNonFiniteStereo(preFader, samples) || nonFinite;
        }
        if (nonFinite)
            m_nonFiniteDeckBlocks.fetch_add(1, std::memory_order_relaxed);
        m_masterBuf.addFrom(0, 0, m_deckScratch, 0, 0, samples);
        m_masterBuf.addFrom(1, 0, m_deckScratch, 1, 0, samples);
    }

    if (aux)
        aux->mixAuxAudio(m_masterBuf, m_previewScratch, samples);
    sanitizeStereo(m_masterBuf, samples);

    const float masterGain = s_masterVolume.load(std::memory_order_relaxed);
    if (std::abs(masterGain - 1.0f) > 0.001f) {
        m_masterBuf.applyGain(0, 0, samples, masterGain);
        m_masterBuf.applyGain(1, 0, samples, masterGain);
    }

    peakL = std::max(peakL, m_masterBuf.getMagnitude(0, 0, samples));
    peakR = std::max(peakR, m_masterBuf.getMagnitude(1, 0, samples));

    const bool limitEnabled = s_antiClipEnabled.load(std::memory_order_relaxed);
    m_limiter.setEnabled(limitEnabled);
    float* limiterChannels[2] = {m_masterBuf.getWritePointer(0),
                                 m_masterBuf.getWritePointer(1)};
    minimumGainReduction = std::min(
        minimumGainReduction,
        m_limiter.processBlock(limiterChannels, 2, 0, samples));

    const float* masterL = m_masterBuf.getReadPointer(0);
    const float* masterR = m_masterBuf.getReadPointer(1);
    const int masterChannel = s_masterFirstChannel.load(std::memory_order_relaxed);
    if (masterChannel >= 1)
        routeStereoToPair(output, masterL, masterR, outputStart, samples, masterChannel);

    const int headphonesChannel = s_headphonesFirstChannel.load(std::memory_order_relaxed);
    if (headphonesChannel >= 1) {
        const float mix = std::clamp(s_headphoneMix.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float cueGain = 1.0f - mix;
        const float masterCueGain = s_masterCueEnabled.load(std::memory_order_relaxed) ? mix : 0.0f;
        bool headphonesHaveSignal = false;
        for (auto* endpoint : endpoints) {
            if (!endpoint || !endpoint->cueEnabledForMix() || cueGain < 0.0001f)
                continue;
            const auto& preFader = endpoint->preFaderBuffer();
            if (preFader.getNumChannels() < 1 || preFader.getNumSamples() < samples)
                continue;
            const float* left = preFader.getReadPointer(0);
            const float* right = preFader.getNumChannels() > 1
                ? preFader.getReadPointer(1) : left;
            routeStereoToPair(output, left, right, outputStart, samples, headphonesChannel,
                              headphonesHaveSignal, cueGain);
            headphonesHaveSignal = true;
        }
        if (masterCueGain > 0.0001f)
            routeStereoToPair(output, masterL, masterR, outputStart, samples,
                              headphonesChannel, headphonesHaveSignal, masterCueGain);
    }

    const int boothChannel = s_boothFirstChannel.load(std::memory_order_relaxed);
    if (boothChannel >= 1) {
        bool add = false;
        for (auto* endpoint : endpoints) {
            if (!endpoint)
                continue;
            const auto& preFader = endpoint->preFaderBuffer();
            if (preFader.getNumChannels() < 1 || preFader.getNumSamples() < samples)
                continue;
            const float* left = preFader.getReadPointer(0);
            const float* right = preFader.getNumChannels() > 1
                ? preFader.getReadPointer(1) : left;
            routeStereoToPair(output, left, right, outputStart, samples, boothChannel, add);
            add = true;
        }
    }
}

void DjMasterBus::setMasterVolume(float value)
{
    s_masterVolume.store(std::clamp(value, 0.0f, 1.5f), std::memory_order_relaxed);
}

void DjMasterBus::setAntiClipEnabled(bool enabled)
{
    s_antiClipEnabled.store(enabled, std::memory_order_relaxed);
}

bool DjMasterBus::antiClipEnabled()
{
    return s_antiClipEnabled.load(std::memory_order_relaxed);
}

float DjMasterBus::gainReduction()
{
    return s_gainReduction.load(std::memory_order_relaxed);
}

int DjMasterBus::limiterLatencySamples()
{
    return s_limiterLatencySamples.load(std::memory_order_relaxed);
}

double DjMasterBus::callbackAverageUsec()
{
    const uint64_t count = s_callbackCount.load(std::memory_order_relaxed);
    return count == 0 ? 0.0
                      : static_cast<double>(s_callbackTotalUsec.load(std::memory_order_relaxed))
                            / static_cast<double>(count);
}

double DjMasterBus::callbackWorstUsec()
{
    return static_cast<double>(s_callbackWorstUsec.load(std::memory_order_relaxed));
}

uint64_t DjMasterBus::callbackCount()
{
    return s_callbackCount.load(std::memory_order_relaxed);
}

uint64_t DjMasterBus::callbackOverrunCount()
{
    return s_callbackOverruns.load(std::memory_order_relaxed);
}

void DjMasterBus::resetCallbackStats()
{
    s_callbackCount.store(0, std::memory_order_relaxed);
    s_callbackTotalUsec.store(0, std::memory_order_relaxed);
    s_callbackWorstUsec.store(0, std::memory_order_relaxed);
    s_callbackOverruns.store(0, std::memory_order_relaxed);
}

void DjMasterBus::setOutputRouting(int master, int booth, int headphones)
{
    s_masterFirstChannel.store(master, std::memory_order_relaxed);
    s_boothFirstChannel.store(booth, std::memory_order_relaxed);
    s_headphonesFirstChannel.store(headphones, std::memory_order_relaxed);
}

int DjMasterBus::masterFirstChannel()
{
    return s_masterFirstChannel.load(std::memory_order_relaxed);
}

int DjMasterBus::boothFirstChannel()
{
    return s_boothFirstChannel.load(std::memory_order_relaxed);
}

int DjMasterBus::headphonesFirstChannel()
{
    return s_headphonesFirstChannel.load(std::memory_order_relaxed);
}

void DjMasterBus::setMasterCueEnabled(bool enabled)
{
    s_masterCueEnabled.store(enabled, std::memory_order_relaxed);
}

bool DjMasterBus::masterCueEnabled()
{
    return s_masterCueEnabled.load(std::memory_order_relaxed);
}

void DjMasterBus::setHeadphoneMix(float mix)
{
    s_headphoneMix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

float DjMasterBus::headphoneMix()
{
    return s_headphoneMix.load(std::memory_order_relaxed);
}

bool DjMasterBus::masterClipDetected_s()
{
    return s_masterClipDetected.load(std::memory_order_relaxed);
}

MasterBusRealtimeStats DjMasterBus::realtimeStats() const noexcept
{
    return {
        m_allocationsRt.load(std::memory_order_relaxed),
        m_bufferGrowthsRt.load(std::memory_order_relaxed),
        m_blockingLocksRt.load(std::memory_order_relaxed),
        m_invalidEndpointReads.load(std::memory_order_relaxed),
        m_staleGenerationReads.load(std::memory_order_relaxed),
        m_oversizedCallbacks.load(std::memory_order_relaxed),
        m_silentOversizedCallbacks.load(std::memory_order_relaxed),
        m_nonFiniteDeckBlocks.load(std::memory_order_relaxed)
    };
}

void DjMasterBus::resetRealtimeStats() noexcept
{
    m_allocationsRt.store(0, std::memory_order_relaxed);
    m_bufferGrowthsRt.store(0, std::memory_order_relaxed);
    m_blockingLocksRt.store(0, std::memory_order_relaxed);
    m_invalidEndpointReads.store(0, std::memory_order_relaxed);
    m_staleGenerationReads.store(0, std::memory_order_relaxed);
    m_oversizedCallbacks.store(0, std::memory_order_relaxed);
    m_silentOversizedCallbacks.store(0, std::memory_order_relaxed);
    m_nonFiniteDeckBlocks.store(0, std::memory_order_relaxed);
}

void DjMasterBus::routeStereoToPair(juce::AudioBuffer<float>& buffer,
                                    const float* sourceLeft, const float* sourceRight,
                                    int start, int samples, int firstChannel,
                                    bool add, float gain) noexcept
{
    if (!sourceLeft || !sourceRight || samples <= 0 || firstChannel < 1
        || std::abs(gain) <= 0.0001f)
        return;
    const int leftChannel = firstChannel - 1;
    const int rightChannel = leftChannel + 1;
    if (rightChannel >= buffer.getNumChannels() || start < 0
        || start > buffer.getNumSamples()
        || samples > buffer.getNumSamples() - start)
        return;

    float* destinationLeft = buffer.getWritePointer(leftChannel, start);
    float* destinationRight = buffer.getWritePointer(rightChannel, start);
    for (int sample = 0; sample < samples; ++sample) {
        const float left = std::isfinite(sourceLeft[sample]) ? sourceLeft[sample] * gain : 0.0f;
        const float right = std::isfinite(sourceRight[sample]) ? sourceRight[sample] * gain : 0.0f;
        if (add) {
            destinationLeft[sample] += left;
            destinationRight[sample] += right;
        } else {
            destinationLeft[sample] = left;
            destinationRight[sample] = right;
        }
    }
}
