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
    PitchShifter= 3
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
