#include "audio/DeckChannelProcessor.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <array>
#include <atomic>
#include <thread>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

float bufferPeak(const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        const float* data = buffer.getReadPointer(ch, startSample);
        for (int i = 0; i < numSamples; ++i)
            peak = std::max(peak, std::abs(data[i]));
    }
    return peak;
}

class ConstantSource final : public juce::AudioSource {
public:
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch)
            bufferToFill.buffer->clear(ch, bufferToFill.startSample, bufferToFill.numSamples);

        for (int ch = 0; ch < std::min(bufferToFill.buffer->getNumChannels(), 2); ++ch) {
            float* data = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
            for (int i = 0; i < bufferToFill.numSamples; ++i)
                data[i] = 1.0f;
        }
    }
};

class SineSource final : public juce::AudioSource {
public:
    explicit SineSource(float frequencyHz)
        : m_frequencyHz(frequencyHz)
    {
    }

    void prepareToPlay(int, double sampleRate) override
    {
        m_sampleRate = sampleRate;
    }

    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        const double phaseStep = juce::MathConstants<double>::twoPi * m_frequencyHz / m_sampleRate;
        for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch)
            bufferToFill.buffer->clear(ch, bufferToFill.startSample, bufferToFill.numSamples);

        for (int ch = 0; ch < std::min(bufferToFill.buffer->getNumChannels(), 2); ++ch) {
            float* data = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
            double phase = m_phase;
            for (int i = 0; i < bufferToFill.numSamples; ++i) {
                data[i] = std::sin(static_cast<float>(phase));
                phase += phaseStep;
            }
        }
        m_phase += phaseStep * bufferToFill.numSamples;
    }

private:
    float m_frequencyHz = 1000.0f;
    double m_sampleRate = 48000.0;
    double m_phase = 0.0;
};

float renderPeak(DeckChannelProcessor& mixer, int blockSize)
{
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::AudioSourceChannelInfo info(&buffer, 0, blockSize);
    mixer.getNextAudioBlock(info);
    return bufferPeak(buffer, 0, blockSize);
}

void settleMixer(DeckChannelProcessor& mixer, int blockSize, int blocks = 8)
{
    for (int i = 0; i < blocks; ++i)
        (void)renderPeak(mixer, blockSize);
}

void testTrimAttenuatesPeak()
{
    ConstantSource source;
    // Heap-allocate like the real engine (DjEngine uses make_unique). The source
    // carries large fixed delay/echo/brake buffers, so stack allocation here
    // overflows the test thread stack and crashes before any assertion runs.
    auto mixerPtr = std::make_unique<DeckChannelProcessor>(&source);
    DeckChannelProcessor& mixer = *mixerPtr;

    constexpr int blockSize = 512;
    constexpr double sampleRate = 48000.0;
    mixer.prepareToPlay(blockSize, sampleRate);

    mixer.setTrim(1.0f);
    settleMixer(mixer, blockSize);
    const float unityPeak = renderPeak(mixer, blockSize);
    expect(unityPeak > 0.95f, "unity trim passes source magnitude");

    mixer.setTrim(0.25f);
    settleMixer(mixer, blockSize);
    const float trimmedPeak = renderPeak(mixer, blockSize);
    expect(trimmedPeak < unityPeak * 0.45f, "trim 0.25 reduces peak vs unity");
    expect(trimmedPeak > unityPeak * 0.10f, "trim 0.25 still produces audible level");
}

void testHighEqBoostIncreasesHighFrequencyPeak()
{
    SineSource source(8000.0f);
    auto mixerPtr = std::make_unique<DeckChannelProcessor>(&source);
    DeckChannelProcessor& mixer = *mixerPtr;

    constexpr int blockSize = 512;
    constexpr double sampleRate = 48000.0;
    mixer.prepareToPlay(blockSize, sampleRate);

    mixer.setEq(0.0f, 0.0f, 0.0f);
    settleMixer(mixer, blockSize, 12);
    const float flatPeak = renderPeak(mixer, blockSize);

    mixer.setEq(0.0f, 0.0f, 1.0f);
    settleMixer(mixer, blockSize, 12);
    const float boostedPeak = renderPeak(mixer, blockSize);

    expect(flatPeak > 0.05f, "HF sine produces non-zero flat EQ peak");
    expect(boostedPeak > flatPeak * 1.15f, "high EQ boost increases HF peak");
}

bool bufferIsFinite(const juce::AudioBuffer<float>& buffer, int numSamples)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < numSamples; ++i)
            if (!std::isfinite(buffer.getSample(ch, i)))
                return false;
    return true;
}

void fillFxInput(juce::AudioBuffer<float>& buffer, int numSamples, int blockIndex)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        float* data = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            data[i] = 0.2f * std::sin(static_cast<float>((blockIndex * numSamples + i) * 0.031));
    }
}

void testFxSwitchingAtBlockBoundaries()
{
    auto fx = std::make_unique<FxProcessor>();
    fx->prepare(48000.0, 1024, 2);
    juce::AudioBuffer<float> buffer(2, 1024);
    constexpr std::array<int, 5> blockSizes { 64, 127, 256, 511, 1024 };

    int block = 0;
    for (int pass = 0; pass < 3; ++pass) {
        for (int rawType = static_cast<int>(EffectType::None);
             rawType <= static_cast<int>(EffectType::RollOut); ++rawType) {
            const auto type = static_cast<EffectType>(rawType);
            fx->setEffectType(type);
            fx->setEffectType(type); // repeated requests still produce a new generation
            fx->setAmount(static_cast<float>((rawType + pass) % 11) / 10.0f);
            fx->setPrimaryParam(static_cast<float>((rawType + 3) % 10) / 10.0f);
            fx->setSCKnobValue(static_cast<float>((rawType % 9) - 4) / 4.0f);
            const int n = blockSizes[static_cast<size_t>(block++) % blockSizes.size()];
            fillFxInput(buffer, n, block);
            fx->process(buffer, 0, n);
            expect(fx->getEffectType() == type, "FX command applied at block boundary");
            expect(bufferIsFinite(buffer, n), "FX switching produces finite samples");
        }
    }

    fx->setEffectType(EffectType::None);
    fx->setAmount(0.0f);
    fillFxInput(buffer, 256, block);
    fx->process(buffer, 0, 256);
    expect(fx->getEffectType() == EffectType::None, "FX returns to bypass");
    expect(bufferIsFinite(buffer, 256), "bypass after FX switching remains finite");
}

void testConcurrentFxAndParameterChanges()
{
    auto fx = std::make_unique<FxProcessor>();
    fx->prepare(48000.0, 512, 2);
    std::atomic<bool> start { false };
    std::atomic<bool> finite { true };

    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(2, 512);
        while (!start.load(std::memory_order_acquire)) {}
        for (int block = 0; block < 500; ++block) {
            const int n = 64 << (block % 4);
            fillFxInput(buffer, n, block);
            fx->process(buffer, 0, n);
            if (!bufferIsFinite(buffer, n))
                finite.store(false, std::memory_order_relaxed);
        }
    });

    std::thread control([&] {
        start.store(true, std::memory_order_release);
        for (int i = 0; i < 4000; ++i) {
            fx->setEffectType(static_cast<EffectType>(i % (static_cast<int>(EffectType::RollOut) + 1)));
            fx->setAmount(static_cast<float>(i % 101) / 100.0f);
            fx->setPrimaryParam(static_cast<float>((i * 3) % 101) / 100.0f);
            fx->setSCKnobValue(static_cast<float>((i % 201) - 100) / 100.0f);
            fx->setExternalDelayTime((i % 7 == 0) ? -1.0f : 0.0625f * static_cast<float>((i % 8) + 1));
            fx->setBeatSyncPosition(i * 0.125, 0.5);
        }
    });

    audio.join();
    control.join();
    expect(finite.load(std::memory_order_relaxed),
           "concurrent FX and parameter switching keeps samples finite");
}

} // namespace

int runMixerDspSmokeTests()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testTrimAttenuatesPeak();
    testHighEqBoostIncreasesHighFrequencyPeak();
    testFxSwitchingAtBlockBoundaries();
    testConcurrentFxAndParameterChanges();

    return g_failures;
}
