#pragma once

#include "fx/FxProcessor.h"

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

class MixerDspSource : public juce::AudioSource {
public:
    static constexpr int kFxChainSlots = 3;

    explicit MixerDspSource(juce::AudioSource* inSource);

    void setFxEffectType(EffectType type);
    void setFxAmount(float amount);
    void setFxSCKnob(float knob);
    void setFxSCParam(float param);
    void setFxExternalDelayTime(float seconds);
    void setFxPrimaryParam(float v);

    void setFxSlotEffectType(int slot, EffectType type);
    void setFxSlotAmount(int slot, float amount);
    void setFxSlotExternalDelayTime(int slot, float seconds);
    void setFxSlotPrimaryParam(int slot, float v);
    void setBeatSyncPosition(double beatPosition, double beatDurationSec);

    void setPadFxEffectType(EffectType type);
    void setPadFxAmount(float amount);
    void clearPadFx();
    void setVinylBrakeActive(bool active);
    void setEchoOutActive(bool active);
    void setBackspinActive(bool active);
    void setRollOutActive(bool active);
    void setScratchTimbre(float amount);
    void armClickFreeTransition();

    [[nodiscard]] const juce::AudioBuffer<float>& getPflBuffer() const;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    void setTrim(float val);
    void setFader(float val);
    void setEq(float l, float m, float h);
    void setFilterVal(float f);

    std::atomic<float> m_peakL { 0.0f };
    std::atomic<float> m_peakR { 0.0f };
    std::atomic<float> m_preFaderPeakL { 0.0f };
    std::atomic<float> m_preFaderPeakR { 0.0f };

private:
    void setStopEffectWanted(std::atomic<bool>& flag, bool active);
    void applyClickFreeTransition(const juce::AudioSourceChannelInfo& bufferToFill);
    [[nodiscard]] int clickFreeBridgeSamples() const;
    [[nodiscard]] static float stopTailGain(float value, float fadeStart);
    [[nodiscard]] float getDecibelsFromKnob(float kb) const;
    void updateFilters();
    void updateFiltersFromValues(float low, float mid, float high, float filter);
    void maybeRefreshFilterCoefficients(int numSamples);
    FxProcessor* fxChainSlot(int slot);

    juce::AudioSource* source = nullptr;
    double m_sampleRate = 0;

    std::atomic<float> trimVal{1.0f};
    std::atomic<float> faderVal{1.0f};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_trimSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_faderSmooth;

    std::atomic<float> lowVol{0.0f};
    std::atomic<float> midVol{0.0f};
    std::atomic<float> highVol{0.0f};
    std::atomic<float> filterVal{0.0f};
    std::atomic<bool> m_filtersDirty { false };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_eqLowSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_eqMidSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_eqHighSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_filterSmooth;
    float m_appliedEqLow    = 0.0f;
    float m_appliedEqMid    = 0.0f;
    float m_appliedEqHigh   = 0.0f;
    float m_appliedFilter   = 0.0f;
    int   m_filterHoldSamples = 0;

    using FilterType = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    FilterType lowEq;
    FilterType midEq;
    FilterType highEq;
    FilterType colorFilter;

    FxProcessor m_colorFx;
    std::array<FxProcessor, kFxChainSlots> m_fxChain;
    FxProcessor m_padFx;

    static constexpr int kVinylBrakeBuf  = 1 << 18;
    static constexpr int kVinylBrakeMask = kVinylBrakeBuf - 1;
    std::atomic<bool> m_vinylBrakeWanted { false };
    std::atomic<bool> m_backspinWanted   { false };
    float    m_vinylBrakeFactor   = 1.0f;
    float    m_vinylBrakeRampDown = 0.0f;
    uint32_t m_vinylBrakeWritePos = 0;
    float    m_vinylBrakeReadPos  = 0.0f;
    float    m_backspinReadPos    = 0.0f;
    bool     m_vinylBrakeNeedSync = true;
    bool     m_backspinNeedSync   = true;
    std::array<float, kVinylBrakeBuf> m_vinylBrakeBufL {};
    std::array<float, kVinylBrakeBuf> m_vinylBrakeBufR {};

    static constexpr int   kEchoOutBuf      = 65536;
    static constexpr int   kEchoOutMask     = kEchoOutBuf - 1;
    static constexpr float kEchoOutFeedback = 0.68f;
    std::atomic<bool> m_echoOutWanted       { false };
    bool             m_echoOutAudioActive  = false;
    uint32_t         m_echoOutWritePos     = 0;
    int            m_echoOutDelaySamples    = 0;
    float          m_echoOutLpCoef          = 0.0f;
    float          m_echoOutLpStateL        = 0.0f;
    float          m_echoOutLpStateR        = 0.0f;
    std::array<float, kEchoOutBuf> m_echoOutBufL {};
    std::array<float, kEchoOutBuf> m_echoOutBufR {};

    float m_backspinSpeed        = 0.0f;
    float m_backspinSpeedRampDown = 0.0f;

    std::atomic<bool> m_rollOutWanted    { false };
    bool  m_rollOutNeedSync  = true;
    int   m_rollOutLoopStart = 0;
    float m_rollOutOffset    = 0.0f;
    float m_rollOutGain      = 1.0f;
    float m_rollOutRampDown  = 0.0f;
    int   m_rollOutLoopLen   = 0;
    juce::AudioBuffer<float> m_preFaderScratch;
    std::atomic<float> scratchTimbre { 0.0f };
    float m_scratchWarmLpState[8] {};
    bool  m_scratchLpWasActive = false;
    std::atomic<bool> m_pendingClickFreeBridge { false };
    float m_lastOutputSample[2] { 0.0f, 0.0f };
    bool  m_lastOutputValid = false;
};
