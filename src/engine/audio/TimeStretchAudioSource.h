#pragma once

#include <atomic>
#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>
#include <rubberband/RubberBandStretcher.h>

class TimeStretchAudioSource : public juce::AudioSource {
public:
    static constexpr int kMinPullSize = 64;
    static constexpr int kMaxPullSize = 512;
    static constexpr int kFifoCapacity = 65536;
    static constexpr int kMaxPrefillSamples = 2048;
    static constexpr int kDefaultPrefillCapSamples = 256;
    static constexpr int kPrefillMaxBlocks = 1;
    static constexpr int kPullLoopLimit = 24;
    static constexpr int kSwitchFadeSamples = 256;
    static constexpr double kPrefillDeadbandTempoDelta = 0.01;
    static constexpr double kPrefillDynamicFactor = 0.005;
    static constexpr double kPrefillExtremeThreshold = 0.30;
    static constexpr double kPrefillExtremeFactor = 0.010;
    static constexpr double kTempoUpdateEpsilon = 0.0005;
    static constexpr double kMaxTempoRatioStepPerBlock = 0.025;

    explicit TimeStretchAudioSource(juce::AudioSource* inSource);

    void setTempoRatio(double ratio);
    void setPitchLockEnabled(bool enabled);
    void setScratchBypass(bool enabled);
    void enterScratchBypass() noexcept;
    void endScratchBypass() noexcept;

    void prepareToPlay(int samplesPerBlockExpected, double sr) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;

    [[nodiscard]] int getLatencySamples() const;

private:
    void updateReportedLatency(int targetSamples);
    [[nodiscard]] int computeDesiredPrefillSamples() const;
    void updatePrefillTarget(int desiredPrefill);
    void trimStretcherOutput(int samplesToTrim);
    void resetRealtimePipeline(bool prewarm);
    void prewarmStretcher();
    void updateStretcherRatios();
    void applyPendingRatioChange();
    void applySwitchFade(const juce::AudioSourceChannelInfo& info);

    juce::AudioSource* source = nullptr;
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    juce::AudioBuffer<float> scratchBuffer;
    juce::AudioBuffer<float> outputBuffer;
    juce::AudioBuffer<float> trimBuffer;
    juce::AudioBuffer<float> prewarmZeroBuffer;
    std::unique_ptr<juce::AbstractFifo> fifo;
    double sampleRate = 44100.0;
    std::atomic<double> m_targetTempoRatio { 1.0 };
    double m_appliedTempoRatio = 1.0;
    std::atomic<bool> m_pitchLockEnabled { false };
    std::atomic<bool> m_scratchBypass { false };
    std::atomic<int> m_reportedLatencySamples { 0 };
    std::atomic<int> m_startPadRemaining { 0 };
    std::atomic<int> m_startDelayTrimRemaining { 0 };
    std::atomic<bool> m_resetPipelineRequested { false };
    std::atomic<int> m_switchFadeRemaining { 0 };
    int m_maxProcessSize = 512;
    int m_prefillTargetSamples = 0;
    int m_prefillHardCapSamples = kMaxPrefillSamples;
};
