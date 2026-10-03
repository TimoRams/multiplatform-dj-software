#include "audio/AudioBusMixer.h"

#include <algorithm>
#include <cmath>

void MasterMixer::prepare(double sampleRate, int maximumBlockSize)
{
    m_sampleRate = std::max(1.0, sampleRate);
    m_masterFx.prepare(m_sampleRate, maximumBlockSize, 2);
    m_limiter.prepare(m_sampleRate, maximumBlockSize, 2);
    reset();
}

void MasterMixer::reset() noexcept
{
    m_crossfaderGain.fill(1.0f);
    for (auto& gain : m_publishedCrossfaderGain)
        gain.store(1.0f, std::memory_order_release);
    m_masterGain = 1.0f;
    m_meter = {};
    m_limiter.reset();
}

float MasterMixer::crossfaderTarget(float position,
                                    CrossfaderAssignment assignment,
                                    CrossfaderCurve curve) noexcept
{
    if (assignment == CrossfaderAssignment::Thru)
        return 1.0f;

    const float t = std::clamp((position + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float side = assignment == CrossfaderAssignment::A ? 1.0f - t : t;
    switch (curve) {
    case CrossfaderCurve::ConstantPower:
        return std::sin(side * juce::MathConstants<float>::halfPi);
    case CrossfaderCurve::Smooth:
        return side * side * (3.0f - 2.0f * side);
    case CrossfaderCurve::Scratch:
        return side >= 0.08f ? 1.0f : 0.0f;
    }
    return side;
}

void MasterMixer::mixPrograms(
    const std::array<const juce::AudioBuffer<float>*, kDeckCount>& programs,
    const std::array<const juce::AudioBuffer<float>*, kDeckCount>& tailReturns,
    const AudioParameters& parameters,
    juce::AudioBuffer<float>& master,
    int samples) noexcept
{
    master.clear(0, 0, samples);
    master.clear(1, 0, samples);

    const float smoothStep = 1.0f / static_cast<float>(
        std::max(1, static_cast<int>(m_sampleRate * 0.005)));
    for (std::size_t deck = 0; deck < programs.size(); ++deck) {
        const auto* program = programs[deck];
        if (!program || program->getNumChannels() < 1 || program->getNumSamples() < samples)
            continue;

        const float target = crossfaderTarget(parameters.crossfaderPosition,
                                              parameters.crossfaderAssignments[deck],
                                              parameters.crossfaderCurve);
        const float startGain = m_crossfaderGain[deck];
        const bool fastCut = parameters.crossfaderCurve == CrossfaderCurve::Scratch;
        const int right = std::min(1, program->getNumChannels() - 1);
        const float* sourceL = program->getReadPointer(0);
        const float* sourceR = program->getReadPointer(right);
        float* masterL = master.getWritePointer(0);
        float* masterR = master.getWritePointer(1);
        float gain = fastCut ? target : startGain;
        for (int sample = 0; sample < samples; ++sample) {
            if (!fastCut)
                gain = std::clamp(target, gain - smoothStep, gain + smoothStep);
            masterL[sample] += sourceL[sample] * gain;
            masterR[sample] += sourceR[sample] * gain;
        }
        m_crossfaderGain[deck] = gain;
        m_publishedCrossfaderGain[deck].store(gain, std::memory_order_release);
    }

    for (const auto* tail : tailReturns) {
        if (!tail || tail->getNumChannels() < 1 || tail->getNumSamples() < samples)
            continue;
        master.addFrom(0, 0, *tail, 0, 0, samples);
        master.addFrom(1, 0, *tail, std::min(1, tail->getNumChannels() - 1),
                       0, samples);
    }
}

void MasterMixer::finalize(const AudioParameters& parameters,
                           juce::AudioBuffer<float>& master,
                           int samples,
                           juce::AudioBuffer<float>* preMasterGainTap) noexcept
{
    const auto requestedFx = static_cast<EffectType>(parameters.masterFxType);
    if (m_masterFx.getRequestedEffectType() != requestedFx)
        m_masterFx.setEffectType(requestedFx);
    m_masterFx.setAmount(parameters.masterFxAmount);
    m_masterFx.setExternalDelayTime(parameters.masterFxExternalDelaySeconds);
    m_masterFx.setPrimaryParam(parameters.masterFxPrimaryParameter);
    m_masterFx.process(master, 0, samples);

    // MASTER CUE monitors the canonical mix (including Master FX), but it is
    // electrically upstream of the hardware MASTER LEVEL control. Preserve
    // that exact tap before applying output gain and the master limiter.
    if (preMasterGainTap
        && preMasterGainTap != &master
        && preMasterGainTap->getNumChannels() >= 2
        && preMasterGainTap->getNumSamples() >= samples) {
        preMasterGainTap->copyFrom(0, 0, master, 0, 0, samples);
        preMasterGainTap->copyFrom(1, 0, master, 1, 0, samples);
    }

    const float targetGain = std::clamp(parameters.masterGain, 0.0f, 1.5f);
    const float gainStep = 1.0f / static_cast<float>(
        std::max(1, static_cast<int>(m_sampleRate * 0.010)));
    float* masterL = master.getWritePointer(0);
    float* masterR = master.getWritePointer(1);
    for (int sample = 0; sample < samples; ++sample) {
        m_masterGain = std::clamp(targetGain,
                                  m_masterGain - gainStep,
                                  m_masterGain + gainStep);
        masterL[sample] *= m_masterGain;
        masterR[sample] *= m_masterGain;
    }

    m_meter.preLimiterPeakL = master.getMagnitude(0, 0, samples);
    m_meter.preLimiterPeakR = master.getMagnitude(1, 0, samples);

    m_limiter.setEnabled(parameters.limiterEnabled);
    float* channels[2] { master.getWritePointer(0), master.getWritePointer(1) };
    m_meter.minimumGainReduction = m_limiter.processBlock(channels, 2, 0, samples);
    m_meter.finalPeakL = master.getMagnitude(0, 0, samples);
    m_meter.finalPeakR = master.getMagnitude(1, 0, samples);
}

void HeadphoneBus::prepare(double sampleRate, int maximumBlockSize)
{
    m_limiter.prepare(std::max(1.0, sampleRate), maximumBlockSize, 2);
    m_limiter.setThreshold(0.90f);
    m_limiter.setCeiling(1.0f);
    m_limiter.setSaturationAmount(0.0f);
    m_limiter.setEnabled(true);
    m_sampleRate = std::max(1.0, sampleRate);
    m_gain = 1.0f;
    m_deckGain.fill(0.0f);
    m_masterTapGain = 0.0f;
}

void HeadphoneBus::mix(
    const std::array<const juce::AudioBuffer<float>*, kDeckCount>& pfl,
    const juce::AudioBuffer<float>& masterTap,
    const AudioParameters& parameters,
    juce::AudioBuffer<float>& output,
    int samples) noexcept
{
    output.clear(0, 0, samples);
    output.clear(1, 0, samples);

    const float mix = std::clamp(parameters.headphoneMix, 0.0f, 1.0f);
    const float cueGain = std::cos(mix * juce::MathConstants<float>::halfPi);
    const float masterGain = parameters.masterCueEnabled
        ? std::sin(mix * juce::MathConstants<float>::halfPi) : 0.0f;

    // Every contribution slews toward its target over ~5 ms instead of stepping
    // at the block boundary. Pressing a channel CUE button, switching MASTER CUE
    // or sweeping the CUE MIX knob otherwise puts a discontinuity straight into
    // the headphone feed, which is heard as a click.
    const float step = 1.0f / static_cast<float>(
        std::max(1, static_cast<int>(m_sampleRate * 0.005)));

    float* outputL = output.getWritePointer(0);
    float* outputR = output.getWritePointer(1);

    const auto addSlewed = [&](const float* sourceL, const float* sourceR,
                               float target, float& gainState) noexcept {
        float gain = gainState;
        for (int sample = 0; sample < samples; ++sample) {
            gain = std::clamp(target, gain - step, gain + step);
            outputL[sample] += sourceL[sample] * gain;
            outputR[sample] += sourceR[sample] * gain;
        }
        gainState = gain;
    };

    for (std::size_t deck = 0; deck < pfl.size(); ++deck) {
        const auto* source = pfl[deck];
        if (!source || source->getNumChannels() < 1 || source->getNumSamples() < samples) {
            // No usable audio this block — there is nothing to fade out through,
            // so the next block has to start from silence.
            m_deckGain[deck] = 0.0f;
            continue;
        }

        const float target = parameters.pflEnabled[deck] ? cueGain : 0.0f;
        if (target <= 0.0f && m_deckGain[deck] <= 0.0f)
            continue;

        const int right = std::min(1, source->getNumChannels() - 1);
        addSlewed(source->getReadPointer(0), source->getReadPointer(right),
                  target, m_deckGain[deck]);
    }

    if (masterTap.getNumChannels() >= 2 && masterTap.getNumSamples() >= samples) {
        if (masterGain > 0.0f || m_masterTapGain > 0.0f)
            addSlewed(masterTap.getReadPointer(0), masterTap.getReadPointer(1),
                      masterGain, m_masterTapGain);
    } else {
        m_masterTapGain = 0.0f;
    }

    const float targetGain = std::clamp(parameters.headphoneGain, 0.0f, 2.0f);
    output.applyGainRamp(0, 0, samples, m_gain, targetGain);
    output.applyGainRamp(1, 0, samples, m_gain, targetGain);
    m_gain = targetGain;

    float* channels[2] { output.getWritePointer(0), output.getWritePointer(1) };
    m_limiter.processBlock(channels, 2, 0, samples);
}
