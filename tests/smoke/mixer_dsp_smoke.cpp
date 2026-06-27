#include "MixerDspSource.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

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

float renderPeak(MixerDspSource& mixer, int blockSize)
{
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::AudioSourceChannelInfo info(&buffer, 0, blockSize);
    mixer.getNextAudioBlock(info);
    return bufferPeak(buffer, 0, blockSize);
}

void settleMixer(MixerDspSource& mixer, int blockSize, int blocks = 8)
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
    auto mixerPtr = std::make_unique<MixerDspSource>(&source);
    MixerDspSource& mixer = *mixerPtr;

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
    auto mixerPtr = std::make_unique<MixerDspSource>(&source);
    MixerDspSource& mixer = *mixerPtr;

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

} // namespace

int runMixerDspSmokeTests()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testTrimAttenuatesPeak();
    testHighEqBoostIncreasesHighFrequencyPeak();

    return g_failures;
}
