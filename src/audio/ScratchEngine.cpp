#include "ScratchEngine.h"

void ScratchEngine::processBlock(juce::AudioBuffer<float>& outputBuffer)
{
    outputBuffer.clear();

    if (m_sourceBuffer == nullptr)
        return;

    const int srcSamples = m_sourceBuffer->getNumSamples();
    if (srcSamples < 4)
        return;

    const int outChannels = outputBuffer.getNumChannels();
    const int outSamples = outputBuffer.getNumSamples();
    const int srcChannels = m_sourceBuffer->getNumChannels();
    const int channelsToRender = std::min(outChannels, std::max(1, srcChannels));

    // dt is one sample in DSP-time domain.
    constexpr double dt = 1.0;

    for (int n = 0; n < outSamples; ++n) {
        if (m_scratchingActive) {
            // Step 1: spring-damper smoothing toward targetPlayPosition.
            // a = k*(target-current) - d*v
            const double displacement = m_targetPlayPosition - m_currentPlayPosition;
            const double accel = (m_smoothingStiffness * displacement) - (m_smoothingDamping * m_currentVelocity);
            m_currentVelocity += accel * dt / m_sampleRate;
            m_currentPlayPosition += m_currentVelocity * dt;
        } else {
            m_currentVelocity = m_playbackSpeedSamplesPerSample;
            m_currentPlayPosition += m_currentVelocity;
        }

        m_currentPlayPosition = normalizePosition(m_currentPlayPosition);

        for (int ch = 0; ch < channelsToRender; ++ch) {
            const int srcCh = std::min(ch, srcChannels - 1);
            const float sample = readInterpolatedSample(srcCh, m_currentPlayPosition);
            outputBuffer.setSample(ch, n, sample);
        }
    }
}

double ScratchEngine::sanitizePosition(double pos) const
{
    if (m_sourceBuffer == nullptr || m_sourceBuffer->getNumSamples() <= 0)
        return 0.0;

    const int srcSamples = m_sourceBuffer->getNumSamples();
    if (m_boundaryMode == BoundaryMode::Wrap)
        return normalizePosition(pos);

    return std::clamp(pos, 0.0, static_cast<double>(srcSamples - 1));
}

double ScratchEngine::normalizePosition(double pos) const
{
    if (m_sourceBuffer == nullptr)
        return 0.0;

    const int srcSamples = m_sourceBuffer->getNumSamples();
    if (srcSamples <= 0)
        return 0.0;

    if (m_boundaryMode == BoundaryMode::Wrap) {
        const double size = static_cast<double>(srcSamples);
        double wrapped = std::fmod(pos, size);
        if (wrapped < 0.0)
            wrapped += size;
        return wrapped;
    }

    return std::clamp(pos, 0.0, static_cast<double>(srcSamples - 1));
}

int ScratchEngine::mapIndex(int i, int numSamples) const
{
    if (numSamples <= 0)
        return 0;

    if (m_boundaryMode == BoundaryMode::Wrap) {
        int idx = i % numSamples;
        if (idx < 0)
            idx += numSamples;
        return idx;
    }

    return std::clamp(i, 0, numSamples - 1);
}

float ScratchEngine::readInterpolatedSample(int channel, double samplePosition) const
{
    // Step 2: 4-point cubic/Hermite interpolation around floor(samplePosition).
    const int srcSamples = m_sourceBuffer->getNumSamples();
    if (srcSamples <= 0)
        return 0.0f;

    const double pos = normalizePosition(samplePosition);
    const int i1 = static_cast<int>(std::floor(pos));
    const int i0 = mapIndex(i1 - 1, srcSamples);
    const int i2 = mapIndex(i1 + 1, srcSamples);
    const int i3 = mapIndex(i1 + 2, srcSamples);
    const int i1m = mapIndex(i1, srcSamples);

    const float y0 = m_sourceBuffer->getSample(channel, i0);
    const float y1 = m_sourceBuffer->getSample(channel, i1m);
    const float y2 = m_sourceBuffer->getSample(channel, i2);
    const float y3 = m_sourceBuffer->getSample(channel, i3);

    const float t = static_cast<float>(pos - std::floor(pos));
    const float t2 = t * t;
    const float t3 = t2 * t;

    // Cubic Hermite (Catmull-Rom tangents) for smooth fractional reading.
    const float m1 = 0.5f * (y2 - y0);
    const float m2 = 0.5f * (y3 - y1);

    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;

    return h00 * y1 + h10 * m1 + h01 * y2 + h11 * m2;
}
