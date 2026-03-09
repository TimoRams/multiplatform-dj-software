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
    Mobius      = 16
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
    /// amount: 0.0 = full dry, 1.0 = full wet
    void setAmount(float amount);

    EffectType getEffectType() const { return static_cast<EffectType>(m_typeAtomic.load()); }
    float      getAmount()     const { return m_amountAtomic.load(); }

private:
    // ── Shared state ─────────────────────────────────────────────────────────
    std::atomic<int>   m_typeAtomic   { static_cast<int>(EffectType::None) };
    std::atomic<float> m_amountAtomic { 0.0f };

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
        // 4 all-pass stages per channel
        float ap[2][4] = {};   // [ch][stage]
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
        float ap[2][8] = {};   // 8 all-pass stages per channel
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
