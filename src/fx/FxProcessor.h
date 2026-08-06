#pragma once

#include "audio/AudioRouting.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "dsp/SsDelay.h"
#include "dsp/SsLfo.h"
#include "dsp/SvfSmoothed.h"
#include "filters.h"

// ─────────────────────────────────────────────────────────────────────────────
// FxProcessor
//
// A single effect slot that can run one of three algorithms:
//   None        – pure bypass (zero overhead)
//   Reverb      – juce::dsp::Reverb, wet/dry controlled by `amount`
//   Bitcrusher  – inline bit-depth + sample-rate reduction
//   PitchShifter– phase-vocoder stub that passes audio cleanly;
//                 a real FFT-based implementation (or SoundTouch hook) is
//                 isolated to PitchShifterImpl so it can be swapped later.
//
// THREAD SAFETY
//   Control-thread setters publish atomics only. Effect changes are consumed at
//   an audio-block boundary; all DSP resources are allocated by prepare().
// ─────────────────────────────────────────────────────────────────────────────

enum class EffectType : int {
    None        = 0,
    Reverb      = 1,
    Bitcrusher  = 2,
    PitchShifter= 3,
    Echo        = 4,
    LowCutEcho  = 5,
    MtDelay     = 6,
    Spiral      = 7,
    Flanger     = 8,
    Phaser      = 9,
    Trans       = 10,
    EnigmaJet   = 11,
    Stretch     = 12,
    SlipRoll    = 13,
    Roll        = 14,
    Nobius      = 15,
    Mobius      = 16,
    // ── Sound Color FX (bipolar knob -1..+1) ─────────────────────────────────
    SoundColorFilter = 17,   // dual LPF/HPF with resonance
    SoundColorDubEcho= 18,   // echo + bipolar LPF/HPF on wet tail
    SoundColorCrush  = 19,   // bitcrusher + bipolar filter
    SoundColorSpace  = 20,   // reverb + bipolar filter on wet
    SoundColorPitch  = 21,   // pure pitch shift ±12 semitones
    SoundColorNoise  = 22,   // white noise through bipolar filter
    SoundColorSweep  = 23,   // animated bipolar filter sweep (rate/depth via SC param)
    RollOut          = 24    // roll that doubles loop length every repetition
};

class FxProcessor
{
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "FX command handoff must be lock-free on supported targets");
    FxProcessor();
    // Must be defined in .cpp where PitchShifterImpl is a complete type
    ~FxProcessor();

    // Called once from prepareToPlay() on the audio thread.
    void prepare(double sampleRate, int maxBlockSize, int numChannels);

    // Called every audio block (audio thread only).
    // Processes buffer in-place with a smoothed wet/dry crossfade.
    void process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void processWetReturn(const juce::AudioBuffer<float>& input,
                          juce::AudioBuffer<float>& wetReturn,
                          int startSample,
                          int numSamples);
    void applyPendingCommandAtBlockBoundary() noexcept;

    // ── Thread-safe parameter setters (main thread) ──────────────────────────
    void setEffectType(EffectType type);
    /// amount: 0.0 = full dry, 1.0 = full wet  (used by FX units 1/2)
    void setAmount(float amount);
    /// knob: -1.0 = max left, 0.0 = bypass, +1.0 = max right  (Sound Color only)
    void setSCKnobValue(float knob);
    /// param: 0.0..1.0 generic Sound Color parameter (mode-specific behavior).
    void setSCParamValue(float param);
    /// Override delay time for BPM-synced effects. seconds < 0 disables the
    /// override and falls back to amount-derived timing. Affects all timing-
    /// based effects (Echo, Flanger, Phaser, Tremolo, Roll, Mobius, etc.).
    void setExternalDelayTime(float seconds);
    /// Current deck beatgrid position. beatPosition is in beats relative to the
    /// analyzed grid; beatDurationSec is the local beat length at the playhead.
    void setBeatSyncPosition(double beatPosition, double beatDurationSec);
    /// Effect-specific primary parameter (0..1). Reverb: room size.
    void setPrimaryParam(float v);

    EffectType getEffectType() const noexcept { return m_activeType; }
    EffectType getRequestedEffectType() const noexcept;
    float      getAmount()     const { return m_amountAtomic.load(); }
    float      getSCKnob()     const { return m_scKnobAtomic.load(); }
    float      getSCParam()    const { return m_scParamAtomic.load(); }

    struct CpuProfile {
        uint64_t count = 0;
        uint64_t totalUsec = 0;
        uint64_t worstUsec = 0;
    };
    static CpuProfile getCpuProfile(EffectType type);
    static const char* effectTypeName(EffectType type) noexcept;

    // Returns true for Sound Color FX types (17-23) that belong pre-EQ in the signal chain.
    // All other non-None types are Beat FX that run post-fader.
    static bool isColorFxType(EffectType type) {
        return type >= EffectType::SoundColorFilter && type <= EffectType::SoundColorSweep;
    }
    static FxPlacement placementForType(EffectType type) noexcept;

private:
    // ── Shared state ─────────────────────────────────────────────────────────
    std::atomic<std::uint64_t> m_pendingTypeCommand { 0 };
    std::uint64_t m_appliedTypeCommand = 0;
    EffectType m_activeType = EffectType::None;
    std::atomic<float> m_amountAtomic { 0.0f };
    std::atomic<float> m_scKnobAtomic { 0.0f };  // bipolar -1..+1 for Sound Color
    std::atomic<float> m_scParamAtomic{ 0.5f };  // generic 0..1 parameter for SC modes
    // BPM sync: overrides amount-derived delay when ≥ 0. Stored in seconds;
    // processEcho/processMtDelay multiply by m_sampleRate. -1 = disabled.
    std::atomic<float> m_externalDelaySeconds { -1.f };
    std::atomic<double> m_beatPosition { 0.0 };
    std::atomic<double> m_beatDurationSeconds { 0.5 };
    double m_audioBeatPosition = 0.0;
    double m_audioBeatDurationSeconds = 0.5;
    double m_lastControlBeatPosition = 0.0;
    // Effect-specific primary parameter (0..1).  Meaning is per-effect:
    //   Reverb → room size (0 = tiny, 1 = huge)
    //   Others → reserved for future per-effect parameter expansion.
    std::atomic<float> m_primaryParamAtomic { 0.5f };

    double m_sampleRate    = 44100.0;
    int    m_maxBlockSize  = 512;
    int    m_numChannels   = 2;

    // Smoothed wet/dry — eliminates clicks when knob is turned or effect is switched.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_wetSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_drySmooth;

    // ── Reverb ────────────────────────────────────────────────────────────────
    juce::dsp::Reverb m_reverb;

    void prepareReverb();
    void processReverb(juce::AudioBuffer<float>& wet, int start, int n);
    void updateReverbParams(); // uses m_primaryParamAtomic for room character

    // ── Bitcrusher ────────────────────────────────────────────────────────────
    // Per-channel sample-and-hold state
    struct BitcrusherState {
        float holdSample   = 0.0f;
        int   holdCounter  = 0;
    };
    std::vector<BitcrusherState> m_bcState;

    void processBitcrusher(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Pitch Shifter ─────────────────────────────────────────────────────────
    // Phase-vocoder ring-buffer implementation.
    // amount 0.0 = -12 semitones, 0.5 = neutral, 1.0 = +12 semitones.
    //
    // The inner struct is self-contained so it can be replaced with a
    // SoundTouch or Rubber Band wrapper without touching FxProcessor.
    struct PitchShifterImpl;
    std::unique_ptr<PitchShifterImpl> m_pitchShifter;

    void preparePitchShifter();
    void resetPitchShifterRealtime() noexcept;
    void processPitchShifter(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Echo / Low-Cut Echo / MT Delay ────────────────────────────────────────
    static constexpr int kMaxDelaySamples = 192000; // up to ~4 s at 48 kHz
    // Fractional-delay line from the DSP layer — drop-in for the old hand-rolled
    // integer DelayLine, with an additional readFrac() for smooth time transitions.
    using DelayLine = dsp::SsDelay;
    struct DelayState {
        DelayLine lineL, lineR;
        float hpStateL = 0.f, hpStateR = 0.f;
        // Biquad LP in the feedback path — warmer and more musical than the
        // 1-pole it replaces; Q=0.85 gives a slight resonant bump at cutoff.
        signalsmith::filters::BiquadStatic<float> lpBiquadL, lpBiquadR;
    };
    DelayState m_delayState;
    // Per-sample delay-time smoothers for Echo and MT Delay: eliminates the
    // click that occurs when the amount knob moves between audio blocks.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_echoDelaySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_mtDelayTimeSmooth;
    void prepareDelay();
    void processEcho(juce::AudioBuffer<float>& wet, int start, int n,
                     float amount, bool lowCut);
    void processMtDelay(juce::AudioBuffer<float>& wet, int start, int n, float amount);
    void beginBeatSyncBlock();
    void advanceBeatSyncBlock(int numSamples);
    double syncedDivisionPhase(float extSec) const;
    int samplesUntilNextDivision(float extSec) const;

    // ── Spiral (chorus-style) ─────────────────────────────────────────────────
    struct SpiralState {
        dsp::SsDelay lineL, lineR;
        dsp::SsLfo   lfo;
    };
    SpiralState m_spiralState;
    void prepareSpiral();
    void processSpiral(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Flanger ───────────────────────────────────────────────────────────────
    struct FlangerState {
        dsp::SsDelay lineL, lineR;
        dsp::SsLfo   lfo;
    };
    FlangerState m_flangerState;
    void prepareFlanger();
    void processFlanger(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Phaser (4-stage all-pass) ─────────────────────────────────────────────
    struct PhaserState {
        float      xPrev[2][4] = {};  // [ch][stage] previous input
        float      yPrev[2][4] = {};  // [ch][stage] previous output
        dsp::SsLfo lfo;
    };
    PhaserState m_phaserState;
    void processPhaser(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Trans (Tremolo) ───────────────────────────────────────────────────────
    struct TransState { dsp::SsLfo lfo; };
    TransState m_transState;
    void processTrans(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Enigma Jet (Phaser + Pitch-detune) ────────────────────────────────────
    struct EnigmaState {
        float      xPrev[2][8] = {};
        float      yPrev[2][8] = {};
        dsp::SsLfo phaseLfo;      // slow sweep for 8-stage all-pass
        dsp::SsDelay detLineL, detLineR;  // chorus detune delay
        dsp::SsLfo detLfo;        // faster LFO for the detune modulation
    };
    EnigmaState m_enigmaState;
    void prepareEnigma();
    void processEnigmaJet(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Stretch (granular freeze) ─────────────────────────────────────────────
    static constexpr int kStretchBuf = 65536; // ~1.37 s @ 48 kHz — avoids rapid wrap-click
    struct StretchState {
        float  buf[2][kStretchBuf] = {};
        int    writePos = 0;
        double readPos  = 0.0;
    };
    StretchState m_stretchState;
    void processStretch(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Roll / Slip Roll ──────────────────────────────────────────────────────
    static constexpr int kRollBuf = 65536;
    struct RollState {
        float  buf[2][kRollBuf] = {};
        int    writePos    = 0;
        int    loopStart   = 0;
        int    loopLen     = 0;
        int    readPos     = 0;
        bool   loopActive  = false;
        int    stepCounter = 0;
        int    doubleCount = 0;  // for RollOut: how many times loop has doubled
        int    quantizedStartCountdown = -1;
    };
    RollState m_rollState;
    void resetRollRealtime() noexcept;
    void processRoll(juce::AudioBuffer<float>& wet, int start, int n,
                     float amount, bool slip);
    void processRollOut(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Nobius / Mobius (forward+reverse loop) ────────────────────────────────
    static constexpr int kMobiusBuf = 65536;
    struct MobiusState {
        float  buf[2][kMobiusBuf] = {};
        int    writePos  = 0;
        double readPos   = 0.0;
        bool   forward   = true;
        int    loopLen   = 0;
    };
    MobiusState m_mobiusState;
    void processMobius(juce::AudioBuffer<float>& wet, int start, int n,
                       float amount, bool nobius);

    // ── Sound Color: shared bipolar biquad filter helper ─────────────────────
    // A 2-pole State Variable Filter with per-sample parameter smoothing.
    // knob < 0 → LPF (cutoff 20kHz→20Hz), knob > 0 → HPF (cutoff 20Hz→20kHz)
    // Using dsp::SvfSmoothed eliminates zipper noise when the knob is swept.
    using SVFState = dsp::SvfSmoothed;

    // Apply the bipolar SVF in-place to the wet buffer.
    // knob: -1..+1  (0 = transparent/bypass)
    // Returns the amount of wet signal to mix (0 at center, 1 at extremes).
    float applySCFilter(juce::AudioBuffer<float>& buf, int start, int n,
                        float knob, SVFState& state);

    // ── Sound Color: per-effect state ─────────────────────────────────────────
    struct SCFilterState  { SVFState svfA; SVFState svfB; };
    struct SCDubEchoState {
        DelayLine lineL, lineR;
        SVFState  svf;
        float     lpL = 0.f, lpR = 0.f;
    };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_scDubDelaySmooth;
    struct SCCrushState   { SVFState svf; std::vector<BitcrusherState> bc; };
    struct SCSpaceState   { SVFState svf; };
    struct SCNoiseState   {
        uint32_t seed = 12345u;
        SVFState svf;
    };
    struct SCSweepState {
        float lp1[2] = {};
        float bp1[2] = {};
        float lp2[2] = {};
        float bp2[2] = {};
        // Per-sample smoothers — eliminate zipper noise when knob is swept
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> fcSmooth;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>         rSmooth;
    };

    SCFilterState  m_scFilterState;
    SCDubEchoState m_scDubEchoState;
    SCCrushState   m_scCrushState;
    SCSpaceState   m_scSpaceState;
    SCNoiseState   m_scNoiseState;
    SCSweepState   m_scSweepState;

    void prepareSCDelays();

    // Shared per-sample wet/dry smoother — one instance serves all SC effects
    // (only one runs per block).  Target is set by each process function.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_scAbsKSmooth;
    // SC Dub Echo: independent smoother for feedback level to prevent spikes
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_scDubFbSmooth;

    // Apply per-sample smoothed wet/dry blend of wetBuf into buffer.
    // m_scAbsKSmooth target must already be set by the caller.
    void mixSCSmoothed(juce::AudioBuffer<float>& dst,
                       const juce::AudioBuffer<float>& wet, int start, int n);

    // SC process functions — all take raw bipolar knob value (-1..+1)
    void processSC_Filter  (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_DubEcho (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Crush   (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Space   (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Pitch   (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Noise   (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Sweep   (juce::AudioBuffer<float>& buf, int start, int n, float knob, float param);

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool ensureScratchCapacity(int numSamples);
    void processWetEffect(EffectType type, juce::AudioBuffer<float>& wetBuf, int n, float amount);
    bool processSoundColorEffect(EffectType type, juce::AudioBuffer<float>& buffer, int start, int n);
    void mixWetDrySmoothed(juce::AudioBuffer<float>& buffer,
                           const juce::AudioBuffer<float>& wetBuf,
                           int start,
                           int n);

    // Copies [start, start+n) from src into a temporary buffer starting at 0,
    // or fills wet buffer with silence for "no-op" bypass.
    void copyToWet(const juce::AudioBuffer<float>& src,
                   juce::AudioBuffer<float>& wet, int start, int n);
    void mixWetDry(juce::AudioBuffer<float>& buffer,
                   const juce::AudioBuffer<float>& wetBuf,
                   int start, int n,
                   float wetGain, float dryGain);

    juce::AudioBuffer<float> m_wetScratch;
};
