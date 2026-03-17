#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>

class ScratchEngine
{
public:
    enum class BoundaryMode {
        Clamp,
        Wrap
    };

    ScratchEngine() = default;

    void setSourceBuffer(const juce::AudioBuffer<float>* source) { m_sourceBuffer = source; }

    void setSampleRate(double sampleRate)
    {
        m_sampleRate = std::max(1.0, sampleRate);
    }

    // Spring strength: higher means faster convergence to target position.
    void setSmoothingStiffness(double stiffness)
    {
        m_smoothingStiffness = std::clamp(stiffness, 1.0, 1200.0);
    }

    // Damping factor: higher means less oscillation/ringing.
    void setSmoothingDamping(double damping)
    {
        m_smoothingDamping = std::clamp(damping, 0.01, 200.0);
    }

    void setBoundaryMode(BoundaryMode mode) { m_boundaryMode = mode; }

    void setScratchingActive(bool active) { m_scratchingActive = active; }

    void setPlaybackSpeedSamples(double speedSamplesPerSample)
    {
        m_playbackSpeedSamplesPerSample = speedSamplesPerSample;
    }

    // Target position in source samples. Updated by mouse/jogwheel while scratching.
    void setTargetPlayPosition(double targetSamples)
    {
        m_targetPlayPosition = sanitizePosition(targetSamples);
    }

    // Immediate hard-set, useful for seek/track-load.
    void resetToPosition(double positionSamples)
    {
        const double p = sanitizePosition(positionSamples);
        m_currentPlayPosition = p;
        m_targetPlayPosition = p;
        m_currentVelocity = 0.0;
    }

    double currentPlayPosition() const { return m_currentPlayPosition; }
    double targetPlayPosition() const { return m_targetPlayPosition; }
    double currentVelocity() const { return m_currentVelocity; }

    // Render output by fractional reading from source via cubic Hermite interpolation.
    void processBlock(juce::AudioBuffer<float>& outputBuffer);

private:
    double sanitizePosition(double pos) const;
    double normalizePosition(double pos) const;
    int mapIndex(int i, int numSamples) const;
    float readInterpolatedSample(int channel, double samplePosition) const;

    const juce::AudioBuffer<float>* m_sourceBuffer = nullptr;

    // Step 1: required state variables.
    double m_currentPlayPosition = 0.0;  // in source samples
    double m_targetPlayPosition = 0.0;   // in source samples
    double m_currentVelocity = 0.0;      // samples per output sample

    double m_sampleRate = 44100.0;
    double m_smoothingStiffness = 220.0;
    double m_smoothingDamping = 24.0;
    double m_playbackSpeedSamplesPerSample = 1.0;

    bool m_scratchingActive = false;
    BoundaryMode m_boundaryMode = BoundaryMode::Clamp;
};
