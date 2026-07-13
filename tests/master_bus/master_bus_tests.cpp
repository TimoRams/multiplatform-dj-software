#include "engine/DjMasterBus.h"

#include <QCoreApplication>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <thread>

namespace {

class FakeDeckEndpoint final : public IDeckAudioEndpoint {
public:
    void prepareToPlay(int maximumBlockSize, double) override
    {
        m_preFader.setSize(2, maximumBlockSize, false, true, true);
        ++prepareCalls;
    }

    void releaseResources() override { ++releaseCalls; }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        ++processCalls;
        const float post = postFader.load(std::memory_order_relaxed);
        const float pre = preFader.load(std::memory_order_relaxed);
        const bool invalid = nonFinite.load(std::memory_order_relaxed);
        const float postValue = invalid ? std::numeric_limits<float>::quiet_NaN() : post;
        const float preValue = invalid ? std::numeric_limits<float>::infinity() : pre;
        for (int channel = 0; channel < std::min(2, info.buffer->getNumChannels()); ++channel) {
            float* destination = info.buffer->getWritePointer(channel, info.startSample);
            std::fill_n(destination, info.numSamples, postValue);
        }
        for (int channel = 0; channel < 2; ++channel) {
            float* destination = m_preFader.getWritePointer(channel);
            std::fill_n(destination, info.numSamples, preValue);
        }
    }

    [[nodiscard]] const juce::AudioBuffer<float>& preFaderBuffer() const noexcept override
    {
        return m_preFader;
    }

    [[nodiscard]] bool cueEnabledForMix() const noexcept override
    {
        return cue.load(std::memory_order_relaxed);
    }

    void setCueEnabledForMix(bool enabled) noexcept override
    {
        cue.store(enabled, std::memory_order_relaxed);
    }

    std::atomic<float> postFader { 0.25f };
    std::atomic<float> preFader { 0.5f };
    std::atomic<bool> cue { false };
    std::atomic<bool> nonFinite { false };
    std::atomic<std::uint64_t> processCalls { 0 };
    int prepareCalls = 0;
    int releaseCalls = 0;

private:
    juce::AudioBuffer<float> m_preFader;
};

class FakeAuxEndpoint final : public IMasterBusAuxEndpoint {
public:
    void prepareAuxAudio(int, double) override { ++prepareCalls; }
    void releaseAuxAudio() override { ++releaseCalls; }
    void mixAuxAudio(juce::AudioBuffer<float>& master, juce::AudioBuffer<float>&,
                     int samples) noexcept override
    {
        ++processCalls;
        for (int channel = 0; channel < 2; ++channel) {
            float* destination = master.getWritePointer(channel);
            for (int sample = 0; sample < samples; ++sample)
                destination[sample] += value;
        }
    }
    float value = 0.125f;
    int prepareCalls = 0;
    int releaseCalls = 0;
    std::atomic<std::uint64_t> processCalls { 0 };
};

void fillConstant(juce::AudioBuffer<float>& buffer, int channel, int start, int samples,
                  float value)
{
    float* destination = buffer.getWritePointer(channel, start);
    std::fill_n(destination, samples, value);
}

juce::AudioBuffer<float> render(DjMasterBus& bus, int samples, int channels = 6,
                                int startSample = 0)
{
    juce::AudioBuffer<float> output(channels, samples + startSample);
    for (int channel = 0; channel < channels; ++channel)
        fillConstant(output, channel, 0, output.getNumSamples(), 0.75f);
    bus.getNextAudioBlock({&output, startSample, samples});
    return output;
}

void assertFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            assert(std::isfinite(buffer.getSample(channel, sample)));
}

float tailAverage(const juce::AudioBuffer<float>& buffer, int channel, int samples = 32)
{
    const int count = std::min(samples, buffer.getNumSamples());
    float sum = 0.0f;
    for (int sample = buffer.getNumSamples() - count; sample < buffer.getNumSamples(); ++sample)
        sum += buffer.getSample(channel, sample);
    return sum / static_cast<float>(count);
}

void resetGlobalMixState()
{
    DjMasterBus::setMasterVolume(1.0f);
    DjMasterBus::setAntiClipEnabled(false);
    DjMasterBus::setOutputRouting(1, -1, -1);
    DjMasterBus::setMasterCueEnabled(false);
    DjMasterBus::setHeadphoneMix(0.5f);
    DjMasterBus::resetCallbackStats();
}

void basicRegistrationAndLifetime()
{
    resetGlobalMixState();
    DjMasterBus bus;
    FakeDeckEndpoint first;
    FakeDeckEndpoint second;
    assert(!bus.registerDeck(first, -1).isValid());
    assert(!bus.registerDeck(first, 4).isValid());
    auto registration = bus.registerDeck(first, 0);
    assert(registration.isValid());
    assert(!bus.registerDeck(first, 1).isValid());
    assert(!bus.registerDeck(second, 0).isValid());

    bus.prepareToPlay(64, 48'000.0);
    assert(first.prepareCalls == 1);
    render(bus, 256);
    assert(first.processCalls.load() > 0);

    auto moved = std::move(registration);
    assert(!registration.isValid() && moved.isValid());
    moved.reset();
    moved.reset();
    const auto callsAfterReset = first.processCalls.load();
    render(bus, 512);
    assert(first.processCalls.load() == callsAfterReset);

    auto replacement = bus.registerDeck(second, 0);
    assert(replacement.isValid());
    assert(second.prepareCalls == 1);
    render(bus, 256);
    assert(second.processCalls.load() > 0);
    replacement.reset();

    FakeAuxEndpoint aux;
    auto auxRegistration = bus.registerAuxEndpoint(aux);
    assert(auxRegistration.isValid());
    assert(!bus.registerAuxEndpoint(aux).isValid());
    render(bus, 256);
    assert(aux.processCalls.load() > 0);
    auxRegistration.reset();
    const auto auxCalls = aux.processCalls.load();
    render(bus, 256);
    assert(aux.processCalls.load() == auxCalls);

    bus.releaseResources();
    bus.prepareToPlay(8192, 44'100.0);
    bus.releaseResources();
    bus.beginShutdown();
    assert(!bus.registerDeck(first, 0).isValid());
}

void blockSizesAndMixing()
{
    resetGlobalMixState();
    DjMasterBus bus;
    std::array<FakeDeckEndpoint, 4> decks;
    std::array<DjMasterBus::DeckRegistration, 4> registrations;
    registrations[0] = bus.registerDeck(decks[0], 0);
    bus.prepareToPlay(512, 48'000.0);
    render(bus, 256); // fill the existing limiter lookahead path

    constexpr std::array sizes {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    for (const int size : sizes) {
        auto output = render(bus, size);
        assertFinite(output);
        assert(std::abs(tailAverage(output, 0) - 0.25f) < 0.001f);
        assert(std::abs(tailAverage(output, 1) - 0.25f) < 0.001f);
    }

    registrations[1] = bus.registerDeck(decks[1], 1);
    decks[1].postFader.store(0.5f);
    auto twoDecks = render(bus, 512);
    assert(std::abs(tailAverage(twoDecks, 0) - 0.75f) < 0.001f);

    registrations[2] = bus.registerDeck(decks[2], 2);
    registrations[3] = bus.registerDeck(decks[3], 3);
    decks[2].postFader.store(0.1f);
    decks[3].postFader.store(0.05f);
    auto fourDecks = render(bus, 512);
    assert(std::abs(tailAverage(fourDecks, 0) - 0.9f) < 0.001f);

    // Existing crossfader/channel-fader semantics arrive as post-fader endpoint audio.
    decks[0].postFader.store(0.0f); // A side cut
    decks[1].postFader.store(0.25f); // B side midpoint
    decks[2].postFader.store(0.1f); // Thru
    decks[3].postFader.store(0.0f); // disabled/muted deck
    auto crossfaded = render(bus, 512);
    assert(std::abs(tailAverage(crossfaded, 0) - 0.35f) < 0.001f);

    DjMasterBus::setMasterVolume(0.5f);
    auto gained = render(bus, 512);
    assert(std::abs(tailAverage(gained, 0) - 0.175f) < 0.001f);

    const auto stats = bus.realtimeStats();
    assert(stats.allocationsFromAudioThread == 0);
    assert(stats.bufferGrowthsFromAudioThread == 0);
    assert(stats.blockingLockAttempts == 0);
    assert(stats.invalidEndpointReads == 0);
    assert(stats.staleGenerationReads == 0);
    assert(stats.silentOversizedCallbacks == 0);
    assert(stats.oversizedCallbacks >= 3);
}

void cueBoothMeterLimiterAndFiniteProtection()
{
    resetGlobalMixState();
    DjMasterBus bus;
    FakeDeckEndpoint deckA;
    FakeDeckEndpoint deckB;
    deckA.postFader.store(0.0f);
    deckA.preFader.store(0.6f);
    deckB.postFader.store(0.0f);
    deckB.preFader.store(0.2f);
    deckA.setCueEnabledForMix(true);
    auto a = bus.registerDeck(deckA, 0);
    auto b = bus.registerDeck(deckB, 1);
    bus.prepareToPlay(512, 48'000.0);
    DjMasterBus::setOutputRouting(1, 3, 5);
    DjMasterBus::setHeadphoneMix(0.0f);
    auto cue = render(bus, 512);
    assert(std::abs(tailAverage(cue, 4) - 0.6f) < 0.001f);
    assert(std::abs(tailAverage(cue, 2) - 0.8f) < 0.001f);

    deckA.postFader.store(0.4f);
    DjMasterBus::setMasterCueEnabled(true);
    DjMasterBus::setHeadphoneMix(1.0f);
    auto masterCue = render(bus, 512);
    assert(std::abs(tailAverage(masterCue, 4) - 0.4f) < 0.001f);
    assert(bus.masterVuL() >= 0.39f && bus.masterVuR() >= 0.39f);

    deckA.postFader.store(8.0f);
    DjMasterBus::setAntiClipEnabled(true);
    auto limited = render(bus, 4096);
    assertFinite(limited);
    assert(limited.getMagnitude(0, 0, limited.getNumSamples()) <= 1.0011f);
    assert(DjMasterBus::gainReduction() < 1.0f);

    deckA.nonFinite.store(true);
    deckB.nonFinite.store(true);
    auto protectedOutput = render(bus, 8192);
    assertFinite(protectedOutput);
    assert(bus.realtimeStats().nonFiniteDeckBlocks > 0);
}

void concurrentRetirement()
{
    resetGlobalMixState();
    DjMasterBus bus;
    FakeDeckEndpoint endpoint;
    auto registration = bus.registerDeck(endpoint, 0);
    bus.prepareToPlay(512, 48'000.0);
    std::atomic<bool> running { true };
    std::thread audio([&] {
        juce::AudioBuffer<float> output(2, 512);
        while (running.load(std::memory_order_acquire))
            bus.getNextAudioBlock({&output, 0, output.getNumSamples()});
    });
    while (endpoint.processCalls.load(std::memory_order_acquire) < 10)
        std::this_thread::yield();
    registration.reset();
    const auto retiredCalls = endpoint.processCalls.load(std::memory_order_acquire);
    for (int spin = 0; spin < 100; ++spin)
        std::this_thread::yield();
    assert(endpoint.processCalls.load(std::memory_order_acquire) == retiredCalls);
    running.store(false, std::memory_order_release);
    audio.join();
    render(bus, 512);
    assert(endpoint.processCalls.load() == retiredCalls);
}

double measure(DjMasterBus& bus, int samples, int iterations, double& worstUsec)
{
    juce::AudioBuffer<float> output(6, samples);
    double total = 0.0;
    worstUsec = 0.0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        bus.getNextAudioBlock({&output, 0, samples});
        const double usec = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        total += usec;
        worstUsec = std::max(worstUsec, usec);
    }
    return total / static_cast<double>(iterations);
}

void deterministicStressAndPerformance()
{
    resetGlobalMixState();
    DjMasterBus bus;
    std::array<FakeDeckEndpoint, 4> decks;
    std::array<DjMasterBus::DeckRegistration, 4> registrations;
    for (std::size_t index = 0; index < decks.size(); ++index)
        registrations[index] = bus.registerDeck(decks[index], static_cast<int>(index));
    bus.prepareToPlay(512, 48'000.0);
    std::mt19937 random(0x4D425553u);
    constexpr std::array sizes {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    for (int step = 0; step < 500; ++step) {
        const auto deck = static_cast<std::size_t>(random() % decks.size());
        decks[deck].postFader.store(static_cast<float>(random() % 1000) / 2000.0f);
        decks[deck].preFader.store(static_cast<float>(random() % 1000) / 1000.0f);
        decks[deck].setCueEnabledForMix((random() & 1u) != 0);
        DjMasterBus::setHeadphoneMix(static_cast<float>(random() % 1000) / 1000.0f);
        DjMasterBus::setOutputRouting(1, 3, 5);
        auto output = render(bus, sizes[random() % sizes.size()]);
        assertFinite(output);
        if (step % 73 == 0) {
            registrations[deck].reset();
            const auto retiredCalls = decks[deck].processCalls.load();
            render(bus, 256);
            assert(decks[deck].processCalls.load() == retiredCalls);
            registrations[deck] = bus.registerDeck(decks[deck], static_cast<int>(deck));
        }
    }

    // Keep the normal measurement path representative: routed cue/master monitoring,
    // post-crossfader endpoint gains and the master limiter are all active.
    decks[0].setCueEnabledForMix(true);
    DjMasterBus::setMasterCueEnabled(true);
    DjMasterBus::setHeadphoneMix(0.5f);
    DjMasterBus::setAntiClipEnabled(true);

    double worstEmpty = 0.0;
    for (auto& registration : registrations)
        registration.reset();
    const double empty512 = measure(bus, 512, 500, worstEmpty);
    registrations[0] = bus.registerDeck(decks[0], 0);
    double worstOne = 0.0;
    const double one512 = measure(bus, 512, 500, worstOne);
    for (std::size_t index = 1; index < decks.size(); ++index)
        registrations[index] = bus.registerDeck(decks[index], static_cast<int>(index));
    double worstFour512 = 0.0;
    const double four512 = measure(bus, 512, 300, worstFour512);
    double worstFour2048 = 0.0;
    const double four2048 = measure(bus, 2048, 200, worstFour2048);
    double worstFour8192 = 0.0;
    const double four8192 = measure(bus, 8192, 80, worstFour8192);
    double worstFour16384 = 0.0;
    const double four16384 = measure(bus, 16384, 40, worstFour16384);

    const auto registerStart = std::chrono::steady_clock::now();
    registrations[3].reset();
    registrations[3] = bus.registerDeck(decks[3], 3);
    const double registrationUsec = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - registerStart).count();

    const auto stats = bus.realtimeStats();
    assert(stats.allocationsFromAudioThread == 0);
    assert(stats.bufferGrowthsFromAudioThread == 0);
    assert(stats.blockingLockAttempts == 0);
    assert(stats.invalidEndpointReads == 0);
    assert(stats.staleGenerationReads == 0);
    assert(stats.silentOversizedCallbacks == 0);

    std::cout << "PERF master_bus_empty_512_us=" << empty512
              << " one_deck_512_us=" << one512
              << " four_decks_512_us=" << four512
              << " four_decks_2048_us=" << four2048
              << " four_decks_8192_us=" << four8192
              << " four_decks_16384_us=" << four16384
              << " worst_16384_us=" << worstFour16384
              << " per_sample_16384_ns=" << four16384 * 1000.0 / 16384.0
              << " registration_cycle_us=" << registrationUsec
              << " preallocated_bytes="
              << 3 * 2 * DjMasterBus::kProcessingChunkSize * sizeof(float) << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    basicRegistrationAndLifetime();
    blockSizesAndMixing();
    cueBoothMeterLimiterAndFiniteProtection();
    concurrentRetirement();
    deterministicStressAndPerformance();
    std::cout << "DjMasterBus tests passed\n";
    return 0;
}
