#pragma once

#include "audio/AudioParameters.h"
#include "fx/FxProcessor.h"
#include "audio/internal/MixerFilterCoefficients.h"

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

class DeckChannelProcessor : public juce::AudioSource {
public:
    struct RealtimeStats { std::uint64_t coefficientBuildsFromAudioThread=0,prepareCallsFromAudioThread=0,bufferGrowthsFromAudioThread=0,blockingLockAttempts=0,objectConstructionsFromAudioThread=0,coefficientSnapshotSwitches=0,staleSnapshots=0,invalidCoefficientSets=0; };
    static constexpr int kFxChainSlots = 3;

    explicit DeckChannelProcessor(juce::AudioSource* inSource);

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
    // Transport search gate. The control thread publishes only the desired
    // state; the audio thread applies a short click-free ramp.
    void setSearchMuted(bool muted) noexcept;
    void armClickFreeTransition();

    [[nodiscard]] const juce::AudioBuffer<float>& getPflBuffer() const;
    [[nodiscard]] const juce::AudioBuffer<float>& getPostFaderTailBuffer() const;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    void setTrim(float val);
    void setFader(float val);
    void setEq(float l, float m, float h);
    void setFilterVal(float f);
    void setPolarityInverted(bool inverted);
    [[nodiscard]] RealtimeStats realtimeStats() const noexcept;
    [[nodiscard]] float channelFaderGain() const noexcept
    { return m_channelFaderGain.load(std::memory_order_acquire); }

    std::atomic<float> m_peakL { 0.0f };
    std::atomic<float> m_peakR { 0.0f };
    std::atomic<float> m_preFaderPeakL { 0.0f };
    std::atomic<float> m_preFaderPeakR { 0.0f };

private:
    struct Parameters {
        float trim = 1.0f;
        float fader = 1.0f;
        float eqLow = 0.0f;
        float eqMid = 0.0f;
        float eqHigh = 0.0f;
        float filter = 0.0f;
        bool polarityInverted = false;
    };

    void setStopEffectWanted(std::atomic<bool>& flag, bool active);
    void applyClickFreeTransition(const juce::AudioSourceChannelInfo& bufferToFill);
    [[nodiscard]] int clickFreeBridgeSamples() const;
    [[nodiscard]] static float stopTailGain(float value, float fadeStart);
    [[nodiscard]] float getDecibelsFromKnob(float kb) const;
    void publishFilterSnapshot() noexcept;
    void activateFilterSnapshot() noexcept;
    void processPreparedFilters(const juce::AudioSourceChannelInfo&) noexcept;
    FxProcessor* fxChainSlot(int slot);

    juce::AudioSource* source = nullptr;
    double m_sampleRate = 0;

    RealtimeSnapshotStore<Parameters> m_parameters;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_trimSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_faderSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_searchMuteSmooth;
    std::atomic<bool> m_searchMuted { false };
    // Audio-thread-published routing gain. Consumers use this instead of
    // reconstructing mixer routing from GUI/controller parameter copies.
    std::atomic<float> m_channelFaderGain { 1.0f };

    enum class SnapshotState:std::uint8_t{Empty,Writing,Ready};
    struct SnapshotSlot{MixerCoefficientSnapshot snapshot;std::atomic<SnapshotState> state{SnapshotState::Empty};};
    std::array<SnapshotSlot,2> m_coefficientSlots;
    std::array<MixerFilterBank,2> m_filterBanks;
    std::atomic<std::uint64_t> m_parameterGeneration{0},m_deviceGeneration{0};
    std::atomic<double> m_filterSampleRate{0.0};
    int m_activeFilterBank=0,m_filterFadeRemaining=0;
    static constexpr int kFilterFadeSamples=128;
    std::atomic<std::uint64_t> m_coeffBuildRt{0},m_prepareRt{0},m_growthRt{0},m_lockRt{0},m_constructRt{0},m_snapshotSwitches{0},m_staleSnapshots{0},m_invalidSets{0};

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
    juce::AudioBuffer<float> m_postFaderTailReturn;
    juce::AudioBuffer<float> m_tailScratch;
    std::atomic<bool> m_pendingClickFreeBridge { false };
    float m_lastOutputSample[2] { 0.0f, 0.0f };
    bool  m_lastOutputValid = false;
};
