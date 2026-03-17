#include "TurntableSimulation.h"

#include <algorithm>
#include <cmath>
#include <rubberband/RubberBandStretcher.h>

TurntableSimulation::~TurntableSimulation() = default;

void TurntableSimulation::setSourceBuffer(const juce::AudioBuffer<float>* sourceBuffer)
{
    m_sourceBuffer = sourceBuffer;
    if (!m_sourceBuffer || m_sourceBuffer->getNumSamples() <= 0) {
        m_currentPhase = 0.0;
        m_platterSpeed = 0.0;
        return;
    }

    const double maxPhase = static_cast<double>(m_sourceBuffer->getNumSamples() - 2);
    m_currentPhase = std::clamp(m_currentPhase, 0.0, std::max(0.0, maxPhase));
}

void TurntableSimulation::setMotorSpeed(double speed)
{
    m_motorSpeed.store(speed, std::memory_order_relaxed);
}

double TurntableSimulation::motorSpeed() const
{
    return m_motorSpeed.load(std::memory_order_relaxed);
}

void TurntableSimulation::setTouched(bool touched)
{
    m_isTouched.store(touched, std::memory_order_relaxed);
}

bool TurntableSimulation::isTouched() const
{
    return m_isTouched.load(std::memory_order_relaxed);
}

void TurntableSimulation::setScratching(bool scratching)
{
    m_isScratching.store(scratching, std::memory_order_relaxed);
}

bool TurntableSimulation::isScratching() const
{
    return m_isScratching.load(std::memory_order_relaxed);
}

void TurntableSimulation::setKeylockEnabled(bool enabled)
{
    m_keylockEnabled.store(enabled, std::memory_order_relaxed);
}

bool TurntableSimulation::keylockEnabled() const
{
    return m_keylockEnabled.load(std::memory_order_relaxed);
}

void TurntableSimulation::setScratchVelocity(double velocity)
{
    m_scratchVelocity.store(velocity, std::memory_order_relaxed);
}

double TurntableSimulation::scratchVelocity() const
{
    return m_scratchVelocity.load(std::memory_order_relaxed);
}

double TurntableSimulation::platterSpeed() const
{
    return m_platterSpeed;
}

double TurntableSimulation::currentPhase() const
{
    return m_currentPhase;
}

void TurntableSimulation::setCurrentPhase(double phase)
{
    m_currentPhase = phase;
    if (!m_sourceBuffer || m_sourceBuffer->getNumSamples() <= 0)
        return;

    const double maxPhase = static_cast<double>(m_sourceBuffer->getNumSamples() - 2);
    m_currentPhase = std::clamp(m_currentPhase, 0.0, std::max(0.0, maxPhase));
}

void TurntableSimulation::setSpinUpFriction(double v)
{
    m_spinUpFriction = std::clamp(v, 0.0001, 1.0);
}

void TurntableSimulation::setSpinDownBrake(double v)
{
    m_spinDownBrake = std::clamp(v, 0.0001, 1.0);
}

void TurntableSimulation::prepareToPlay(int, double sampleRate)
{
    m_sampleRate = std::max(1.0, sampleRate);
    m_stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
        m_sampleRate,
        2,
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionPitchHighQuality);
    m_lastUsedBypass = true;
    m_crossfadeRemaining = 0;
}

void TurntableSimulation::releaseResources()
{
    m_stretcher.reset();
    m_dryBuffer.setSize(0, 0);
    m_wetBuffer.setSize(0, 0);
}

void TurntableSimulation::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (!bufferToFill.buffer)
        return;

    auto* out = bufferToFill.buffer;
    const int outChannels = out->getNumChannels();
    const int outSamples = bufferToFill.numSamples;
    const int outStart = bufferToFill.startSample;
    out->clear(outStart, outSamples);

    if (!m_sourceBuffer || m_sourceBuffer->getNumSamples() < 4 || m_sourceBuffer->getNumChannels() <= 0) {
        return;
    }

    m_dryBuffer.setSize(outChannels, outSamples, false, false, true);
    m_wetBuffer.setSize(outChannels, outSamples, false, false, true);

    // First generate the robust fractional-reader output (vinyl path).
    renderFractionalBypass(m_dryBuffer, 0, outSamples);

    const bool scratching = m_isScratching.load(std::memory_order_relaxed)
                         || m_isTouched.load(std::memory_order_relaxed);

    // Step 1: automatic keylock bypass routing guard.
    const bool mustBypassKeylock = (m_platterSpeed < 0.0)
                                || scratching
                                || (std::abs(m_platterSpeed - 1.0) > 0.8)
                                || !m_keylockEnabled.load(std::memory_order_relaxed);

    bool usingBypass = mustBypassKeylock;

    if (!mustBypassKeylock) {
        // Leaving scratch/reverse mode back into keylock: reset phase vocoder state.
        if (m_lastUsedBypass && m_stretcher)
            m_stretcher->reset();

        if (!renderKeylockPath(m_dryBuffer, m_wetBuffer, outSamples))
            usingBypass = true;
    }

    if (usingBypass != m_lastUsedBypass)
        m_crossfadeRemaining = kTransitionCrossfadeSamples;

    const juce::AudioBuffer<float>& current = usingBypass ? m_dryBuffer : m_wetBuffer;
    const juce::AudioBuffer<float>& previous = m_lastUsedBypass ? m_dryBuffer : m_wetBuffer;

    const int fadeLen = std::min(outSamples, m_crossfadeRemaining);
    for (int ch = 0; ch < outChannels; ++ch) {
        if (fadeLen > 0) {
            for (int i = 0; i < fadeLen; ++i) {
                const float t = static_cast<float>(i + 1) / static_cast<float>(fadeLen);
                const float a = 1.0f - t;
                const float sPrev = previous.getSample(ch, i);
                const float sCurr = current.getSample(ch, i);
                out->setSample(ch, outStart + i, sPrev * a + sCurr * t);
            }
        }
        for (int i = fadeLen; i < outSamples; ++i)
            out->setSample(ch, outStart + i, current.getSample(ch, i));
    }

    m_crossfadeRemaining = std::max(0, m_crossfadeRemaining - outSamples);
    m_lastUsedBypass = usingBypass;
}

void TurntableSimulation::renderFractionalBypass(juce::AudioBuffer<float>& buffer, int outStart, int outSamples)
{
    buffer.clear();
    if (!m_sourceBuffer)
        return;

    const int totalSamples = m_sourceBuffer->getNumSamples();
    const int srcChannels = m_sourceBuffer->getNumChannels();
    const int outChannels = buffer.getNumChannels();

    for (int n = 0; n < outSamples; ++n) {
        if (m_isTouched.load(std::memory_order_relaxed)) {
            const double targetScratch = m_scratchVelocity.load(std::memory_order_relaxed);
            m_platterSpeed += (targetScratch - m_platterSpeed) * m_touchVelocityLpf;
        } else {
            const double motor = m_motorSpeed.load(std::memory_order_relaxed);
            const double response = (std::abs(motor) < 1e-9) ? m_spinDownBrake : m_spinUpFriction;
            m_platterSpeed += (motor - m_platterSpeed) * response;
        }

        m_currentPhase += m_platterSpeed;

        // Step 4: crash-safe clamping before reading support points.
        const double maxPhase = static_cast<double>(totalSamples - 2);
        if (m_currentPhase < 0.0) {
            m_currentPhase = 0.0;
            m_platterSpeed = 0.0;
        } else if (m_currentPhase > maxPhase) {
            m_currentPhase = maxPhase;
            m_platterSpeed = 0.0;
        }

        // Step 2: reverse-safe fractional decomposition as requested.
        long index = static_cast<long>(m_currentPhase);
        double frac = m_currentPhase - static_cast<double>(index);
        if (frac < 0.0) {
            frac += 1.0;
            index -= 1;
        }

        const int i0 = clampIndex(static_cast<int>(index - 1), totalSamples);
        const int i1 = clampIndex(static_cast<int>(index), totalSamples);
        const int i2 = clampIndex(static_cast<int>(index + 1), totalSamples);
        const int i3 = clampIndex(static_cast<int>(index + 2), totalSamples);

        for (int ch = 0; ch < outChannels; ++ch) {
            const int srcCh = std::min(ch, srcChannels - 1);
            const float y0 = m_sourceBuffer->getSample(srcCh, i0);
            const float y1 = m_sourceBuffer->getSample(srcCh, i1);
            const float y2 = m_sourceBuffer->getSample(srcCh, i2);
            const float y3 = m_sourceBuffer->getSample(srcCh, i3);

            const float s = cubicHermite(y0, y1, y2, y3, static_cast<float>(frac));
            buffer.setSample(ch, outStart + n, s);
        }
    }
}

bool TurntableSimulation::renderKeylockPath(const juce::AudioBuffer<float>& input,
                                            juce::AudioBuffer<float>& output,
                                            int outSamples)
{
    if (!m_stretcher)
        return false;

    output.clear();

    const int channels = std::min(2, input.getNumChannels());
    if (channels <= 0)
        return false;

    const float* inPtrs[2] = { nullptr, nullptr };
    float* outPtrs[2] = { nullptr, nullptr };

    inPtrs[0] = input.getReadPointer(0);
    inPtrs[1] = (channels > 1) ? input.getReadPointer(1) : input.getReadPointer(0);

    outPtrs[0] = output.getWritePointer(0);
    outPtrs[1] = (output.getNumChannels() > 1) ? output.getWritePointer(1) : output.getWritePointer(0);

    m_stretcher->setTimeRatio(1.0);
    m_stretcher->setPitchScale(1.0);
    m_stretcher->process(inPtrs, outSamples, false);

    const int avail = m_stretcher->available();
    if (avail <= 0)
        return false;

    const int toRead = std::min(outSamples, avail);
    m_stretcher->retrieve(outPtrs, toRead);

    if (toRead < outSamples) {
        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, toRead, input, std::min(ch, input.getNumChannels() - 1), toRead, outSamples - toRead);
    }

    return true;
}

float TurntableSimulation::cubicHermite(float y0, float y1, float y2, float y3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;

    const float m1 = 0.5f * (y2 - y0);
    const float m2 = 0.5f * (y3 - y1);

    const float h00 = (2.0f * t3) - (3.0f * t2) + 1.0f;
    const float h10 = t3 - (2.0f * t2) + t;
    const float h01 = (-2.0f * t3) + (3.0f * t2);
    const float h11 = t3 - t2;

    return (h00 * y1) + (h10 * m1) + (h01 * y2) + (h11 * m2);
}

int TurntableSimulation::clampIndex(int idx, int totalSamples) const
{
    return std::clamp(idx, 0, std::max(0, totalSamples - 1));
}
