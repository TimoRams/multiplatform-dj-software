#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>

namespace RubberBand { class RubberBandStretcher; }

class TurntableSimulation : public juce::AudioSource
{
public:
    TurntableSimulation() = default;
    ~TurntableSimulation() override;

    void setSourceBuffer(const juce::AudioBuffer<float>* sourceBuffer);

    // Motor target speed: 1.0 = forward play, 0.0 = stop, -1.0 = reverse play.
    void setMotorSpeed(double speed);
    double motorSpeed() const;

    // True while platter/vinyl is grabbed by mouse/jogwheel touch.
    void setTouched(bool touched);
    bool isTouched() const;

    // Logical scratch state (can mirror touched state from UI).
    void setScratching(bool scratching);
    bool isScratching() const;

    void setKeylockEnabled(bool enabled);
    bool keylockEnabled() const;

    // Scratch velocity in samples-per-sample (can be negative).
    // Typically derived from mouse delta over time.
    void setScratchVelocity(double velocity);
    double scratchVelocity() const;

    // Main state variables required by the architecture.
    double platterSpeed() const;
    double currentPhase() const;

    void setCurrentPhase(double phase);

    // Physics constants.
    void setSpinUpFriction(double v);
    void setSpinDownBrake(double v);

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

private:
    static float cubicHermite(float y0, float y1, float y2, float y3, float t);
    int clampIndex(int idx, int totalSamples) const;
    void renderFractionalBypass(juce::AudioBuffer<float>& buffer, int outStart, int outSamples);
    bool renderKeylockPath(const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int outSamples);

    const juce::AudioBuffer<float>* m_sourceBuffer = nullptr;

    // Step 1: required physical model variables.
    std::atomic<double> m_motorSpeed { 0.0 };
    double m_platterSpeed = 0.0;
    std::atomic<bool> m_isTouched { false };
    std::atomic<bool> m_isScratching { false };
    std::atomic<double> m_scratchVelocity { 0.0 };
    std::atomic<bool> m_keylockEnabled { false };

    // Additional playback/read state.
    double m_currentPhase = 0.0; // sample-position pointer (fractional)
    double m_sampleRate = 44100.0;

    // Physical behavior constants.
    double m_spinUpFriction = 0.01;
    double m_spinDownBrake = 0.02;

    // Minimal LPF against touch jitter when grabbed.
    double m_touchVelocityLpf = 0.25;

    std::unique_ptr<RubberBand::RubberBandStretcher> m_stretcher;
    juce::AudioBuffer<float> m_dryBuffer;
    juce::AudioBuffer<float> m_wetBuffer;
    bool m_lastUsedBypass = true;
    int m_crossfadeRemaining = 0;
    static constexpr int kTransitionCrossfadeSamples = 128;
};
