#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <vector>

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
//   setEffectType() and setAmount() write to std::atomics → safe to call from
//   the Qt main thread while the audio thread calls process().
//   prepareToPlay() must be called from the audio thread before process().
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
    SoundColorSweep  = 23    // animated bipolar filter sweep (rate/depth via SC param)
};

class FxProcessor
{
public:
    FxProcessor();
    // Must be defined in .cpp where PitchShifterImpl is a complete type
    ~FxProcessor();

    // Called once from prepareToPlay() on the audio thread.
    void prepare(double sampleRate, int maxBlockSize, int numChannels);

    // Called every audio block (audio thread only).
    // Processes buffer in-place with a smoothed wet/dry crossfade.
    void process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples);

    // ── Thread-safe parameter setters (main thread) ──────────────────────────
    void setEffectType(EffectType type);
    /// amount: 0.0 = full dry, 1.0 = full wet  (used by FX units 1/2)
    void setAmount(float amount);
    /// knob: -1.0 = max left, 0.0 = bypass, +1.0 = max right  (Sound Color only)
    void setSCKnobValue(float knob);
    /// param: 0.0..1.0 generic Sound Color parameter (mode-specific behavior).
    void setSCParamValue(float param);

    EffectType getEffectType() const { return static_cast<EffectType>(m_typeAtomic.load()); }
    float      getAmount()     const { return m_amountAtomic.load(); }
    float      getSCKnob()     const { return m_scKnobAtomic.load(); }
    float      getSCParam()    const { return m_scParamAtomic.load(); }

private:
    // ── Shared state ─────────────────────────────────────────────────────────
    std::atomic<int>   m_typeAtomic   { static_cast<int>(EffectType::None) };
    std::atomic<float> m_amountAtomic { 0.0f };
    std::atomic<float> m_scKnobAtomic { 0.0f };  // bipolar -1..+1 for Sound Color
    std::atomic<float> m_scParamAtomic{ 0.5f };  // generic 0..1 parameter for SC modes

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
    void updateReverbParams(float amount);

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
    void processPitchShifter(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Echo / Low-Cut Echo / MT Delay ────────────────────────────────────────
    static constexpr int kMaxDelaySamples = 192000; // up to ~4 s at 48 kHz
    struct DelayLine {
        std::vector<float> buf;
        int writePos = 0;
        float read(int delaySamples) const;
        void  write(float sample);
        void  prepare(int maxSamples);
    };
    struct DelayState {
        DelayLine lineL, lineR;
        // Low-cut echo: simple 1-pole HP filter state
        float hpStateL = 0.f, hpStateR = 0.f;
    };
    DelayState m_delayState;
    void prepareDelay();
    void processEcho(juce::AudioBuffer<float>& wet, int start, int n,
                     float amount, bool lowCut);
    void processMtDelay(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Spiral (chorus-style) ─────────────────────────────────────────────────
    struct SpiralState {
        std::vector<float> bufL, bufR;
        int   writePos = 0;
        float lfoPhase = 0.f;
    };
    SpiralState m_spiralState;
    void prepareSpiral();
    void processSpiral(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Flanger ───────────────────────────────────────────────────────────────
    struct FlangerState {
        std::vector<float> bufL, bufR;
        int   writePos = 0;
        float lfoPhase = 0.f;
    };
    FlangerState m_flangerState;
    void prepareFlanger();
    void processFlanger(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Phaser (4-stage all-pass) ─────────────────────────────────────────────
    struct PhaserState {
        // 1st-order all-pass per stage: y[n] = coeff*(x[n] - y[n-1]) + x[n-1]
        // We store x[n-1] and y[n-1] per channel per stage
        float xPrev[2][4] = {};  // [ch][stage] previous input
        float yPrev[2][4] = {};  // [ch][stage] previous output
        float lfoPhase = 0.f;
    };
    PhaserState m_phaserState;
    void processPhaser(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Trans (Tremolo) ───────────────────────────────────────────────────────
    struct TransState { float lfoPhase = 0.f; };
    TransState m_transState;
    void processTrans(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Enigma Jet (Phaser + Pitch-detune) ────────────────────────────────────
    struct EnigmaState {
        float xPrev[2][8] = {};  // [ch][stage] previous input for all-pass
        float yPrev[2][8] = {};  // [ch][stage] previous output for all-pass
        float lfoPhase = 0.f;
        // small detune delay
        std::vector<float> detBufL, detBufR;
        int detWritePos = 0;
        float detLfo    = 0.f;
    };
    EnigmaState m_enigmaState;
    void prepareEnigma();
    void processEnigmaJet(juce::AudioBuffer<float>& wet, int start, int n, float amount);

    // ── Stretch (granular freeze) ─────────────────────────────────────────────
    static constexpr int kStretchBuf = 8192;
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
    };
    RollState m_rollState;
    void processRoll(juce::AudioBuffer<float>& wet, int start, int n,
                     float amount, bool slip);

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
    // A 2-pole State-Variable Filter (SVF) that morphs LPF↔HPF based on sign.
    // knob < 0 → LPF (cutoff 20kHz→20Hz), knob > 0 → HPF (cutoff 20Hz→20kHz)
    struct SVFState {
        float s1[2] = {};  // integrator 1, per channel
        float s2[2] = {};  // integrator 2, per channel
    };

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
        float     lpL = 0.f, lpR = 0.f;  // tape-style low-pass in feedback
    };
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
    };

    SCFilterState  m_scFilterState;
    SCDubEchoState m_scDubEchoState;
    SCCrushState   m_scCrushState;
    SCSpaceState   m_scSpaceState;
    SCNoiseState   m_scNoiseState;
    SCSweepState   m_scSweepState;

    void prepareSCDelays();

    // SC process functions — all take raw bipolar knob value (-1..+1)
    void processSC_Filter  (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_DubEcho (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Crush   (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Space   (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Pitch   (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Noise   (juce::AudioBuffer<float>& buf, int start, int n, float knob);
    void processSC_Sweep   (juce::AudioBuffer<float>& buf, int start, int n, float knob, float param);

    // ── Helpers ───────────────────────────────────────────────────────────────
    // Copies [start, start+n) from src into a temporary buffer starting at 0,
    // or fills wet buffer with silence for "no-op" bypass.
    void copyToWet(const juce::AudioBuffer<float>& src,
                   juce::AudioBuffer<float>& wet, int start, int n);
    void mixWetDry(juce::AudioBuffer<float>& buffer,
                   const juce::AudioBuffer<float>& wetBuf,
                   int start, int n,
                   float wetGain, float dryGain);
};
