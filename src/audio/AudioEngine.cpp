#include "audio/AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

AudioParameterStore AudioEngine::s_parameterStore;
std::atomic<float> AudioEngine::s_gainReduction { 1.0f };
std::atomic<int> AudioEngine::s_limiterLatencySamples { 0 };
std::atomic<uint64_t> AudioEngine::s_callbackCount { 0 };
std::atomic<uint64_t> AudioEngine::s_callbackTotalUsec { 0 };
std::atomic<uint64_t> AudioEngine::s_callbackWorstUsec { 0 };
std::atomic<uint64_t> AudioEngine::s_callbackOverruns { 0 };
std::atomic<bool> AudioEngine::s_masterClipDetected { false };

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

AudioEngine::AuxRegistration::AuxRegistration(AudioEngine* bus, std::uint64_t generation,
                                               IAuxAudioEndpoint* endpoint) noexcept
    : m_bus(bus), m_endpoint(endpoint), m_generation(generation)
{
}

AudioEngine::AuxRegistration::~AuxRegistration()
{
    reset();
}

AudioEngine::AuxRegistration::AuxRegistration(AuxRegistration&& other) noexcept
    : m_bus(std::exchange(other.m_bus, nullptr))
    , m_endpoint(std::exchange(other.m_endpoint, nullptr))
    , m_generation(std::exchange(other.m_generation, 0))
{
}

AudioEngine::AuxRegistration& AudioEngine::AuxRegistration::operator=(
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

void AudioEngine::AuxRegistration::reset() noexcept
{
    if (m_bus)
        m_bus->unregisterAux(m_generation, m_endpoint);
    m_bus = nullptr;
    m_endpoint = nullptr;
    m_generation = 0;
}

AudioEngine::AudioEngine(AudioPageCache& cache)
{
    for (auto& pipeline : m_decks)
        pipeline = std::make_unique<DeckAudioPipeline>(cache);
}

AudioEngine::~AudioEngine()
{
    beginShutdown();
    m_sourcePlayer.setSource(nullptr);
}

DeckAudioPipeline& AudioEngine::deck(std::size_t index) noexcept
{
    jassert(index < m_decks.size());
    return *m_decks[std::min(index, m_decks.size() - 1)];
}

const DeckAudioPipeline& AudioEngine::deck(std::size_t index) const noexcept
{
    jassert(index < m_decks.size());
    return *m_decks[std::min(index, m_decks.size() - 1)];
}

AudioEngine::AuxRegistration AudioEngine::registerAuxEndpoint(IAuxAudioEndpoint& endpoint)
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

void AudioEngine::unregisterAux(std::uint64_t generation,
                                IAuxAudioEndpoint* endpoint) noexcept
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

void AudioEngine::waitForEndpointReaders() const noexcept
{
    while (m_activeEndpointReaders.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
}

void AudioEngine::beginShutdown() noexcept
{
    m_shuttingDown.store(true, std::memory_order_release);
    {
        std::lock_guard lock(m_registrationMutex);
        m_auxEndpoint.store(nullptr, std::memory_order_seq_cst);
        m_auxGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    waitForEndpointReaders();
}

void AudioEngine::registerCallback(juce::AudioDeviceManager& adm)
{
    m_sourcePlayer.setSource(this);
    adm.addAudioCallback(&m_sourcePlayer);
}

void AudioEngine::unregisterCallback(juce::AudioDeviceManager& adm)
{
    adm.removeAudioCallback(&m_sourcePlayer);
    m_sourcePlayer.setSource(nullptr);
    waitForEndpointReaders();
}

void AudioEngine::prepareToPlay(int, double sampleRate)
{
    const double validRate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    m_sampleRate.store(validRate, std::memory_order_release);

    for (auto& buffer : m_deckBuffers)
        buffer.setSize(2, kProcessingChunkSize, false, true, true);
    m_masterBuf.setSize(2, kProcessingChunkSize, false, true, true);
    m_masterCueTap.setSize(2, kProcessingChunkSize, false, true, true);
    m_headphoneBuf.setSize(2, kProcessingChunkSize, false, true, true);
    m_previewScratch.setSize(2, kProcessingChunkSize, false, true, true);
    m_masterMixer.prepare(validRate, kProcessingChunkSize);
    m_headphoneBus.prepare(validRate, kProcessingChunkSize);
    s_limiterLatencySamples.store(m_masterMixer.limiterLatencySamples(),
                                  std::memory_order_relaxed);

    EndpointReadGuard guard(m_activeEndpointReaders);
    for (auto& pipeline : m_decks)
        pipeline->prepareToPlay(kProcessingChunkSize, validRate);
    if (auto* aux = m_auxEndpoint.load(std::memory_order_seq_cst))
        aux->prepareAuxAudio(kProcessingChunkSize, validRate);
    m_isPrepared.store(true, std::memory_order_release);
}

void AudioEngine::releaseResources()
{
    m_isPrepared.store(false, std::memory_order_release);
    EndpointReadGuard guard(m_activeEndpointReaders);
    for (auto& pipeline : m_decks)
        pipeline->releaseResources();
    if (auto* aux = m_auxEndpoint.load(std::memory_order_seq_cst))
        aux->releaseAuxAudio();
}

void AudioEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    const juce::ScopedNoDenormals noDenormals;
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
    std::array<DeckAudioPipeline*, kMaximumDecks> endpoints {};
    for (std::size_t index = 0; index < kMaximumDecks; ++index)
        endpoints[index] = m_decks[index].get();
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
    const AudioParameters parameters = s_parameterStore.snapshot();
    for (int offset = 0; offset < totalSamples; offset += kProcessingChunkSize) {
        const int chunkSamples = std::min(kProcessingChunkSize, totalSamples - offset);
        processChunk(*output, start + offset, chunkSamples, endpoints, aux, parameters,
                     callbackPeakL, callbackPeakR, minimumGainReduction);
    }

    m_masterPeakL.store(callbackPeakL, std::memory_order_relaxed);
    m_masterPeakR.store(callbackPeakR, std::memory_order_relaxed);
    s_masterClipDetected.store(callbackPeakL > 1.001f || callbackPeakR > 1.001f,
                               std::memory_order_relaxed);
    s_gainReduction.store(parameters.limiterEnabled ? minimumGainReduction : 1.0f,
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

void AudioEngine::processChunk(
    juce::AudioBuffer<float>& output, int outputStart, int samples,
    const std::array<DeckAudioPipeline*, kMaximumDecks>& endpoints,
    IAuxAudioEndpoint* aux, const AudioParameters& parameters,
    float& peakL, float& peakR,
    float& minimumGainReduction) noexcept
{
    std::array<const juce::AudioBuffer<float>*, kMaximumDecks> programs {};
    std::array<const juce::AudioBuffer<float>*, kMaximumDecks> tailReturns {};
    std::array<const juce::AudioBuffer<float>*, kMaximumDecks> pfl {};
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        auto* endpoint = endpoints[index];
        if (!endpoint)
            continue;
        auto& deckBuffer = m_deckBuffers[index];
        deckBuffer.clear(0, 0, samples);
        deckBuffer.clear(1, 0, samples);
        juce::AudioSourceChannelInfo info(&deckBuffer, 0, samples);
        endpoint->getNextAudioBlock(info);
        bool nonFinite = sanitizeStereo(deckBuffer, samples);
        const auto& preFader = endpoint->preFaderBuffer();
        if (preFader.getNumChannels() < 1 || preFader.getNumSamples() < samples) {
            m_invalidEndpointReads.fetch_add(1, std::memory_order_relaxed);
        } else {
            nonFinite = containsNonFiniteStereo(preFader, samples) || nonFinite;
        }
        if (nonFinite)
            m_nonFiniteDeckBlocks.fetch_add(1, std::memory_order_relaxed);
        programs[index] = &deckBuffer;
        tailReturns[index] = &endpoint->postFaderTailBuffer();
        pfl[index] = &preFader;
    }

    m_masterMixer.mixPrograms(programs, tailReturns, parameters, m_masterBuf, samples);
    if (aux)
        aux->mixAuxAudio(m_masterBuf, m_previewScratch, samples);
    sanitizeStereo(m_masterBuf, samples);
    // The master cue tap is refreshed unconditionally: the headphone bus fades
    // it in and out across a block, and fading through a buffer that stopped
    // being written the moment MASTER CUE was switched off would click.
    m_masterMixer.finalize(parameters, m_masterBuf, samples, &m_masterCueTap);
    const auto& meter = m_masterMixer.meter();
    peakL = std::max(peakL, meter.finalPeakL);
    peakR = std::max(peakR, meter.finalPeakR);
    minimumGainReduction = std::min(minimumGainReduction, meter.minimumGainReduction);

    m_headphoneBus.mix(pfl, m_masterCueTap, parameters,
                       m_headphoneBuf, samples);
    m_outputRouter.write(m_masterBuf, m_headphoneBuf, parameters,
                         output, outputStart, samples);
}

void AudioEngine::setMasterVolume(float value)
{
    s_parameterStore.update([value](AudioParameters& p) {
        p.masterGain = std::clamp(value, 0.0f, 1.5f);
    });
}

void AudioEngine::setAntiClipEnabled(bool enabled)
{
    s_parameterStore.update([enabled](AudioParameters& p) { p.limiterEnabled = enabled; });
}

bool AudioEngine::antiClipEnabled()
{
    return s_parameterStore.controlSnapshot().limiterEnabled;
}

float AudioEngine::gainReduction()
{
    return s_gainReduction.load(std::memory_order_relaxed);
}

int AudioEngine::limiterLatencySamples()
{
    return s_limiterLatencySamples.load(std::memory_order_relaxed);
}

double AudioEngine::callbackAverageUsec()
{
    const uint64_t count = s_callbackCount.load(std::memory_order_relaxed);
    return count == 0 ? 0.0
                      : static_cast<double>(s_callbackTotalUsec.load(std::memory_order_relaxed))
                            / static_cast<double>(count);
}

double AudioEngine::callbackWorstUsec()
{
    return static_cast<double>(s_callbackWorstUsec.load(std::memory_order_relaxed));
}

uint64_t AudioEngine::callbackCount()
{
    return s_callbackCount.load(std::memory_order_relaxed);
}

uint64_t AudioEngine::callbackOverrunCount()
{
    return s_callbackOverruns.load(std::memory_order_relaxed);
}

void AudioEngine::resetCallbackStats()
{
    s_callbackCount.store(0, std::memory_order_relaxed);
    s_callbackTotalUsec.store(0, std::memory_order_relaxed);
    s_callbackWorstUsec.store(0, std::memory_order_relaxed);
    s_callbackOverruns.store(0, std::memory_order_relaxed);
}

void AudioEngine::setOutputRouting(int master, int booth, int headphones)
{
    s_parameterStore.update([master, booth, headphones](AudioParameters& p) {
        p.masterFirstChannel = master;
        p.boothFirstChannel = booth;
        p.headphonesFirstChannel = headphones;
    });
}

int AudioEngine::masterFirstChannel()
{
    return s_parameterStore.controlSnapshot().masterFirstChannel;
}

int AudioEngine::boothFirstChannel()
{
    return s_parameterStore.controlSnapshot().boothFirstChannel;
}

int AudioEngine::headphonesFirstChannel()
{
    return s_parameterStore.controlSnapshot().headphonesFirstChannel;
}

void AudioEngine::setMasterCueEnabled(bool enabled)
{
    s_parameterStore.update([enabled](AudioParameters& p) { p.masterCueEnabled = enabled; });
}

bool AudioEngine::masterCueEnabled()
{
    return s_parameterStore.controlSnapshot().masterCueEnabled;
}

void AudioEngine::setHeadphoneMix(float mix)
{
    s_parameterStore.update([mix](AudioParameters& p) {
        p.headphoneMix = std::clamp(mix, 0.0f, 1.0f);
    });
}

float AudioEngine::headphoneMix()
{
    return s_parameterStore.controlSnapshot().headphoneMix;
}

void AudioEngine::setHeadphoneGain(float gain)
{
    s_parameterStore.update([gain](AudioParameters& p) {
        p.headphoneGain = std::clamp(gain, 0.0f, 2.0f);
    });
}

float AudioEngine::headphoneGain()
{
    return s_parameterStore.controlSnapshot().headphoneGain;
}

void AudioEngine::setPflEnabled(int deckIndex, bool enabled)
{
    if (deckIndex < 0 || deckIndex >= static_cast<int>(kMaximumDecks))
        return;
    s_parameterStore.update([deckIndex, enabled](AudioParameters& p) {
        p.pflEnabled[static_cast<std::size_t>(deckIndex)] = enabled;
    });
}

bool AudioEngine::pflEnabled(int deckIndex)
{
    if (deckIndex < 0 || deckIndex >= static_cast<int>(kMaximumDecks))
        return false;
    return s_parameterStore.controlSnapshot().pflEnabled[
        static_cast<std::size_t>(deckIndex)];
}

void AudioEngine::setCrossfaderPosition(float position)
{
    s_parameterStore.update([position](AudioParameters& p) {
        p.crossfaderPosition = std::clamp(position, -1.0f, 1.0f);
    });
}

void AudioEngine::setCrossfaderCurve(CrossfaderCurve curve)
{
    s_parameterStore.update([curve](AudioParameters& p) { p.crossfaderCurve = curve; });
}

void AudioEngine::setCrossfaderAssignment(int deckIndex,
                                          CrossfaderAssignment assignment)
{
    if (deckIndex < 0 || deckIndex >= static_cast<int>(kMaximumDecks))
        return;
    s_parameterStore.update([deckIndex, assignment](AudioParameters& p) {
        p.crossfaderAssignments[static_cast<std::size_t>(deckIndex)] = assignment;
    });
}

void AudioEngine::setMasterFx(EffectType type, float amount)
{
    s_parameterStore.update([type, amount](AudioParameters& parameters) {
        parameters.masterFxType = static_cast<int>(type);
        parameters.masterFxAmount = std::clamp(amount, 0.0f, 1.0f);
    });
}

void AudioEngine::setMasterFxTiming(float externalDelaySeconds, float primaryParameter)
{
    s_parameterStore.update([externalDelaySeconds, primaryParameter](AudioParameters& parameters) {
        parameters.masterFxExternalDelaySeconds = externalDelaySeconds;
        parameters.masterFxPrimaryParameter = std::clamp(primaryParameter, 0.0f, 1.0f);
    });
}

bool AudioEngine::masterClipDetected_s()
{
    return s_masterClipDetected.load(std::memory_order_relaxed);
}

AudioEngineRealtimeStats AudioEngine::realtimeStats() const noexcept
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

void AudioEngine::resetRealtimeStats() noexcept
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
