#include "FxProcessor.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <cassert>

namespace {
constexpr int kFxProfileCount = static_cast<int>(EffectType::RollOut) + 1;
std::array<std::atomic<uint64_t>, kFxProfileCount> s_fxCounts {};
std::array<std::atomic<uint64_t>, kFxProfileCount> s_fxTotalUsec {};
std::array<std::atomic<uint64_t>, kFxProfileCount> s_fxWorstUsec {};

int effectProfileIndex(EffectType type) noexcept
{
    const int index = static_cast<int>(type);
    return (index >= 0 && index < kFxProfileCount) ? index : 0;
}

void recordFxCpuUsec(EffectType type, uint64_t elapsedUsec) noexcept
{
    const int index = effectProfileIndex(type);
    s_fxCounts[static_cast<size_t>(index)].fetch_add(1, std::memory_order_relaxed);
    s_fxTotalUsec[static_cast<size_t>(index)].fetch_add(elapsedUsec, std::memory_order_relaxed);
    uint64_t observedWorst = s_fxWorstUsec[static_cast<size_t>(index)].load(std::memory_order_relaxed);
    while (elapsedUsec > observedWorst
           && !s_fxWorstUsec[static_cast<size_t>(index)].compare_exchange_weak(
               observedWorst,
               elapsedUsec,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}
}

// ─────────────────────────────────────────────────────────────────────────────
// PitchShifterImpl  –  simple ring-buffer pitch shifter (no FFT required).
//
// Algorithm: dual-head read-back through a delay ring buffer.
//   • A write pointer advances at sample rate.
//   • Two read pointers advance at  sampleRate * pitchRatio.
//   • A raised-cosine (Hann) crossfade window blends the two readers so there
//     are no audible discontinuities when one pointer wraps.
//
// Range: ±12 semitones (ratio 0.5 … 2.0).
// Latency: ~bufLen/2 samples (≈ 46 ms at 44.1 kHz with bufLen 4096).
//
// TODO: For production quality, replace with SoundTouch or Rubber Band:
//   #include <SoundTouch.h>
//   soundtouch::SoundTouch st;
//   st.setSampleRate(sampleRate);
//   st.setChannels(numChannels);
//   st.setPitchSemiTones(semitones);
//   // feed: st.putSamples(in, n);
//   // drain: st.receiveSamples(out, n);
// ─────────────────────────────────────────────────────────────────────────────
struct FxProcessor::PitchShifterImpl
{
    static constexpr int kBufLen = 4096;   // must be power-of-2

    // Per-channel ring buffer + fractional read positions
    struct Chan {
        float  buf[kBufLen] = {};
        double readPos0     = 0.0;
        double readPos1     = kBufLen / 2.0; // offset by half buffer
        int    writePos     = 0;
    };

    std::vector<Chan> chans;
    double pitchRatio = 1.0; // 2^(semitones/12)

    void prepare(int numChannels) {
        chans.assign(numChannels, Chan{});
    }

    void setPitchRatio(double ratio) { pitchRatio = ratio; }

    void resetRealtime() noexcept {
        pitchRatio = 1.0;
        for (auto& channel : chans) {
            channel.readPos0 = 0.0;
            channel.readPos1 = kBufLen / 2.0;
            channel.writePos = 0;
        }
    }

    // Hann window value at normalised position t ∈ [0,1)
    static float hann(double t) {
        return 0.5f * (1.0f - static_cast<float>(
            std::cos(2.0 * juce::MathConstants<double>::pi * t)));
    }

    void process(juce::AudioBuffer<float>& buf, int start, int n)
    {
        for (int ch = 0; ch < (int)chans.size() && ch < buf.getNumChannels(); ++ch)
        {
            auto& c = chans[ch];
            float* data = buf.getWritePointer(ch) + start;

            for (int i = 0; i < n; ++i)
            {
                // Write dry sample into ring buffer
                c.buf[c.writePos & (kBufLen - 1)] = data[i];

                // ── Reader 0 ──────────────────────────────────────────────────
                int   r0i  = static_cast<int>(c.readPos0) & (kBufLen - 1);
                float frac0 = static_cast<float>(c.readPos0 - std::floor(c.readPos0));
                int   r0i1 = (r0i + 1) & (kBufLen - 1);
                float s0   = c.buf[r0i] + frac0 * (c.buf[r0i1] - c.buf[r0i]); // lerp

                // ── Reader 1 ──────────────────────────────────────────────────
                int   r1i  = static_cast<int>(c.readPos1) & (kBufLen - 1);
                float frac1 = static_cast<float>(c.readPos1 - std::floor(c.readPos1));
                int   r1i1 = (r1i + 1) & (kBufLen - 1);
                float s1   = c.buf[r1i] + frac1 * (c.buf[r1i1] - c.buf[r1i]);

                // ── Crossfade windows based on fractional distance to write ptr ──
                double dist0 = std::fmod(
                    static_cast<double>(c.writePos) - c.readPos0 + kBufLen, kBufLen);
                double t0    = dist0 / kBufLen;
                float w0     = hann(t0);

                double dist1 = std::fmod(
                    static_cast<double>(c.writePos) - c.readPos1 + kBufLen, kBufLen);
                double t1    = dist1 / kBufLen;
                float w1     = hann(t1);

                // Normalise so the two windows always sum to 1 (avoids amplitude flutter)
                float wSum = w0 + w1;
                if (wSum > 1e-6f) { w0 /= wSum; w1 /= wSum; }

                data[i] = s0 * w0 + s1 * w1;

                // Advance read positions at pitch speed
                c.readPos0 += pitchRatio;
                c.readPos1 += pitchRatio;
                // Wrap fractional positions within buffer
                c.readPos0 = std::fmod(c.readPos0, static_cast<double>(kBufLen));
                c.readPos1 = std::fmod(c.readPos1, static_cast<double>(kBufLen));

                ++c.writePos;
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// FxProcessor
// ─────────────────────────────────────────────────────────────────────────────

FxProcessor::FxProcessor()
    : m_pitchShifter(std::make_unique<PitchShifterImpl>())
{
}

// Defined here so the compiler sees the complete PitchShifterImpl definition
FxProcessor::~FxProcessor() = default;

void FxProcessor::prepare(double sampleRate, int maxBlockSize, int numChannels)
{
    m_sampleRate   = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_numChannels  = numChannels;
    m_wetScratch.setSize(m_numChannels, m_maxBlockSize, false, true, true);

    // Smoothed wet/dry: ~20 ms ramp time to eliminate clicks
    const float rampSamples = static_cast<float>(sampleRate * 0.020);
    m_wetSmooth.reset(sampleRate, 0.020);
    m_drySmooth.reset(sampleRate, 0.020);
    m_wetSmooth.setCurrentAndTargetValue(0.0f);
    m_drySmooth.setCurrentAndTargetValue(1.0f);

    prepareReverb();

    m_bcState.assign(static_cast<size_t>(numChannels), BitcrusherState{});

    preparePitchShifter();
    prepareDelay();
    prepareSpiral();
    prepareFlanger();
    prepareEnigma();
    prepareSCDelays();
    m_phaserState.lfo.prepare(m_sampleRate);
    m_transState.lfo.prepare(m_sampleRate);

    // Delay-time smoothers: 30 ms ramp prevents clicks when the knob moves.
    // Initialised to a sensible mid-range so the first ramp is inaudible
    // (wet/dry is at 0 when the effect first activates).
    const float midDelay = static_cast<float>(sampleRate * 0.3);
    m_echoDelaySmooth  .reset(static_cast<float>(sampleRate), 0.030f);
    m_mtDelayTimeSmooth.reset(static_cast<float>(sampleRate), 0.030f);
    m_scDubDelaySmooth .reset(static_cast<float>(sampleRate), 0.030f);
    m_echoDelaySmooth  .setCurrentAndTargetValue(midDelay);
    m_mtDelayTimeSmooth.setCurrentAndTargetValue(midDelay);
    m_scDubDelaySmooth .setCurrentAndTargetValue(midDelay);

    // SC crush per-channel state
    m_scCrushState.bc.assign(static_cast<size_t>(numChannels), BitcrusherState{});

    // Roll/Slip/Mobius/Nobius buffers are stack-allocated (fixed size), just reset positions
    m_rollState   = RollState{};
    m_mobiusState = MobiusState{};
    m_stretchState = StretchState{};

    (void)rampSamples;
}

void FxProcessor::process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    applyPendingCommandAtBlockBoundary();
    beginBeatSyncBlock();

    const auto type = m_activeType;
    const float amount = m_amountAtomic.load(std::memory_order_relaxed);

    m_wetSmooth.setTargetValue(amount);
    m_drySmooth.setTargetValue(1.0f - amount);

    if (type == EffectType::None && !m_wetSmooth.isSmoothing() && amount < 1e-4f) {
        advanceBeatSyncBlock(numSamples);
        return;
    }

    const auto profileStartTicks = juce::Time::getHighResolutionTicks();
    const auto finishProfile = [type, profileStartTicks]()
    {
        const auto elapsedTicks = juce::Time::getHighResolutionTicks() - profileStartTicks;
        const auto freq = juce::Time::getHighResolutionTicksPerSecond();
        const uint64_t elapsedUsec = freq > 0
            ? static_cast<uint64_t>((static_cast<long double>(elapsedTicks) * 1000000.0L)
                                    / static_cast<long double>(freq))
            : 0;
        recordFxCpuUsec(type, elapsedUsec);
    };

    if (processSoundColorEffect(type, buffer, startSample, numSamples)) {
        advanceBeatSyncBlock(numSamples);
        finishProfile();
        return;
    }

    if (!ensureScratchCapacity(numSamples))
    {
        jassertfalse;
        advanceBeatSyncBlock(numSamples);
        finishProfile();
        return;
    }

    copyToWet(buffer, m_wetScratch, startSample, numSamples);
    processWetEffect(type, m_wetScratch, numSamples, amount);
    mixWetDrySmoothed(buffer, m_wetScratch, startSample, numSamples);
    advanceBeatSyncBlock(numSamples);
    finishProfile();
}

FxPlacement FxProcessor::placementForType(EffectType type) noexcept
{
    switch (type) {
    case EffectType::Reverb:
    case EffectType::Echo:
    case EffectType::LowCutEcho:
    case EffectType::MtDelay:
    case EffectType::Spiral:
        return FxPlacement::PostFaderTail;
    default:
        return FxPlacement::PreFaderInsert;
    }
}

void FxProcessor::processWetReturn(const juce::AudioBuffer<float>& input,
                                   juce::AudioBuffer<float>& wetReturn,
                                   int startSample,
                                   int numSamples)
{
    if (numSamples <= 0 || wetReturn.getNumChannels() < m_numChannels
        || wetReturn.getNumSamples() < numSamples) {
        return;
    }

    for (int channel = 0; channel < std::min(m_numChannels, wetReturn.getNumChannels()); ++channel)
        wetReturn.clear(channel, 0, numSamples);

    const EffectType type = m_activeType;
    if (placementForType(type) != FxPlacement::PostFaderTail || !ensureScratchCapacity(numSamples))
        return;
    beginBeatSyncBlock();

    const int inputChannels = input.getNumChannels();
    for (int channel = 0; channel < m_numChannels; ++channel) {
        if (inputChannels > 0 && startSample >= 0
            && startSample + numSamples <= input.getNumSamples()) {
            m_wetScratch.copyFrom(channel, 0, input,
                                  std::min(channel, inputChannels - 1),
                                  startSample, numSamples);
        } else {
            m_wetScratch.clear(channel, 0, numSamples);
        }
    }

    const float amount = std::clamp(m_amountAtomic.load(std::memory_order_relaxed), 0.0f, 1.0f);
    m_wetSmooth.setTargetValue(amount);
    processWetEffect(type, m_wetScratch, numSamples, amount);
    for (int sample = 0; sample < numSamples; ++sample) {
        const float wetGain = m_wetSmooth.getNextValue();
        for (int channel = 0; channel < m_numChannels; ++channel)
            wetReturn.setSample(channel, sample,
                                m_wetScratch.getSample(channel, sample) * wetGain);
    }
    advanceBeatSyncBlock(numSamples);
}

FxProcessor::CpuProfile FxProcessor::getCpuProfile(EffectType type)
{
    const int index = effectProfileIndex(type);
    const auto arrayIndex = static_cast<size_t>(index);
    return {
        .count = s_fxCounts[arrayIndex].load(std::memory_order_relaxed),
        .totalUsec = s_fxTotalUsec[arrayIndex].load(std::memory_order_relaxed),
        .worstUsec = s_fxWorstUsec[arrayIndex].load(std::memory_order_relaxed)
    };
}

const char* FxProcessor::effectTypeName(EffectType type) noexcept
{
    switch (type) {
        case EffectType::None: return "None";
        case EffectType::Reverb: return "Reverb";
        case EffectType::Bitcrusher: return "Bitcrusher";
        case EffectType::PitchShifter: return "PitchShifter";
        case EffectType::Echo: return "Echo";
        case EffectType::LowCutEcho: return "LowCutEcho";
        case EffectType::MtDelay: return "MtDelay";
        case EffectType::Spiral: return "Spiral";
        case EffectType::Flanger: return "Flanger";
        case EffectType::Phaser: return "Phaser";
        case EffectType::Trans: return "Trans";
        case EffectType::EnigmaJet: return "EnigmaJet";
        case EffectType::Stretch: return "Stretch";
        case EffectType::SlipRoll: return "SlipRoll";
        case EffectType::Roll: return "Roll";
        case EffectType::MobiusSaw: return "MobiusSaw";
        case EffectType::MobiusTri: return "MobiusTri";
        case EffectType::SoundColorFilter: return "SoundColorFilter";
        case EffectType::SoundColorDubEcho: return "SoundColorDubEcho";
        case EffectType::SoundColorCrush: return "SoundColorCrush";
        case EffectType::SoundColorSpace: return "SoundColorSpace";
        case EffectType::SoundColorPitch: return "SoundColorPitch";
        case EffectType::SoundColorNoise: return "SoundColorNoise";
        case EffectType::SoundColorSweep: return "SoundColorSweep";
        case EffectType::RollOut: return "RollOut";
    }
    return "Unknown";
}

bool FxProcessor::ensureScratchCapacity(int numSamples)
{
    return m_wetScratch.getNumChannels() >= m_numChannels
        && m_wetScratch.getNumSamples() >= numSamples;
}

void FxProcessor::processWetEffect(EffectType type,
                                   juce::AudioBuffer<float>& wetBuf,
                                   int n,
                                   float amount)
{
    switch (type)
    {
        case EffectType::Reverb:
            updateReverbParams();
            processReverb(wetBuf, 0, n);
            break;
        case EffectType::Bitcrusher:
            processBitcrusher(wetBuf, 0, n, amount);
            break;
        case EffectType::PitchShifter:
            processPitchShifter(wetBuf, 0, n, amount);
            break;
        case EffectType::Echo:
            processEcho(wetBuf, 0, n, amount, false);
            break;
        case EffectType::LowCutEcho:
            processEcho(wetBuf, 0, n, amount, true);
            break;
        case EffectType::MtDelay:
            processMtDelay(wetBuf, 0, n, amount);
            break;
        case EffectType::Spiral:
            processSpiral(wetBuf, 0, n, amount);
            break;
        case EffectType::Flanger:
            processFlanger(wetBuf, 0, n, amount);
            break;
        case EffectType::Phaser:
            processPhaser(wetBuf, 0, n, amount);
            break;
        case EffectType::Trans:
            processTrans(wetBuf, 0, n, amount);
            break;
        case EffectType::EnigmaJet:
            processEnigmaJet(wetBuf, 0, n, amount);
            break;
        case EffectType::Stretch:
            processStretch(wetBuf, 0, n, amount);
            break;
        case EffectType::SlipRoll:
            processRoll(wetBuf, 0, n, amount, true);
            break;
        case EffectType::Roll:
            processRoll(wetBuf, 0, n, amount, false);
            break;
        case EffectType::RollOut:
            processRollOut(wetBuf, 0, n, amount);
            break;
        case EffectType::MobiusSaw:
            processMobius(wetBuf, 0, n, amount, true);
            break;
        case EffectType::MobiusTri:
            processMobius(wetBuf, 0, n, amount, false);
            break;
        case EffectType::None:
        default:
            break;
    }
}

bool FxProcessor::processSoundColorEffect(EffectType type,
                                          juce::AudioBuffer<float>& buffer,
                                          int start,
                                          int n)
{
    switch (type)
    {
        case EffectType::SoundColorFilter:
        case EffectType::SoundColorDubEcho:
        case EffectType::SoundColorCrush:
        case EffectType::SoundColorSpace:
        case EffectType::SoundColorPitch:
        case EffectType::SoundColorNoise:
        case EffectType::SoundColorSweep:
            break;
        default:
            return false;
    }

    if (!ensureScratchCapacity(n))
    {
        jassertfalse;
        return true;
    }

    const float knob = m_scKnobAtomic.load(std::memory_order_relaxed);
    switch (type)
    {
        case EffectType::SoundColorFilter:
            processSC_Filter(buffer, start, n, knob);
            break;
        case EffectType::SoundColorDubEcho:
            processSC_DubEcho(buffer, start, n, knob);
            break;
        case EffectType::SoundColorCrush:
            processSC_Crush(buffer, start, n, knob);
            break;
        case EffectType::SoundColorSpace:
            processSC_Space(buffer, start, n, knob);
            break;
        case EffectType::SoundColorPitch:
            processSC_Pitch(buffer, start, n, knob);
            break;
        case EffectType::SoundColorNoise:
            processSC_Noise(buffer, start, n, knob);
            break;
        case EffectType::SoundColorSweep:
        {
            const float param = m_scParamAtomic.load(std::memory_order_relaxed);
            processSC_Sweep(buffer, start, n, knob, param);
            break;
        }
        default:
            break;
    }

    return true;
}

void FxProcessor::mixWetDrySmoothed(juce::AudioBuffer<float>& buffer,
                                    const juce::AudioBuffer<float>& wetBuf,
                                    int start,
                                    int n)
{
    const int numChannels = std::min(buffer.getNumChannels(), m_numChannels);
    for (int i = 0; i < n; ++i)
    {
        const float wet = m_wetSmooth.getNextValue();
        const float dry = m_drySmooth.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* mainPtr = buffer.getWritePointer(ch) + start + i;
            *mainPtr = (*mainPtr) * dry + wetBuf.getReadPointer(ch)[i] * wet;
        }
    }
}

void FxProcessor::setEffectType(EffectType type)
{
    const int rawType = static_cast<int>(type);
    if (rawType < static_cast<int>(EffectType::None)
        || rawType > static_cast<int>(EffectType::RollOut))
        type = EffectType::None;

    auto observed = m_pendingTypeCommand.load(std::memory_order_relaxed);
    std::uint64_t desired = 0;
    do {
        const auto generation = (observed >> 8U) + 1U;
        desired = (generation << 8U) | static_cast<std::uint8_t>(type);
    } while (!m_pendingTypeCommand.compare_exchange_weak(
        observed, desired, std::memory_order_release, std::memory_order_relaxed));
}

EffectType FxProcessor::getRequestedEffectType() const noexcept
{
    return static_cast<EffectType>(m_pendingTypeCommand.load(std::memory_order_acquire) & 0xffU);
}

void FxProcessor::applyPendingCommandAtBlockBoundary() noexcept
{
    const auto command = m_pendingTypeCommand.load(std::memory_order_acquire);
    if (command == m_appliedTypeCommand)
        return;

    m_appliedTypeCommand = command;
    m_activeType = static_cast<EffectType>(command & 0xffU);
    if (m_activeType == EffectType::PitchShifter)
        resetPitchShifterRealtime();
    if (m_activeType == EffectType::Roll
        || m_activeType == EffectType::RollOut
        || m_activeType == EffectType::SlipRoll)
        resetRollRealtime();
}

void FxProcessor::setAmount(float amount)
{
    m_amountAtomic.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
}

void FxProcessor::setSCKnobValue(float knob)
{
    m_scKnobAtomic.store(std::clamp(knob, -1.0f, 1.0f), std::memory_order_relaxed);
}

void FxProcessor::setSCParamValue(float param)
{
    m_scParamAtomic.store(std::clamp(param, 0.0f, 1.0f), std::memory_order_relaxed);
}

void FxProcessor::setExternalDelayTime(float seconds)
{
    m_externalDelaySeconds.store(seconds, std::memory_order_relaxed);
}

void FxProcessor::setBeatSyncPosition(double beatPosition, double beatDurationSec)
{
    m_beatPosition.store(beatPosition, std::memory_order_relaxed);
    m_beatDurationSeconds.store(std::max(0.001, beatDurationSec), std::memory_order_relaxed);
}

void FxProcessor::beginBeatSyncBlock()
{
    const double controlBeat = m_beatPosition.load(std::memory_order_relaxed);
    const double controlDur = m_beatDurationSeconds.load(std::memory_order_relaxed);
    if (std::abs(controlBeat - m_lastControlBeatPosition) > 1e-6) {
        m_audioBeatPosition = controlBeat;
        m_lastControlBeatPosition = controlBeat;
    }
    m_audioBeatDurationSeconds = std::max(0.001, controlDur);
}

void FxProcessor::advanceBeatSyncBlock(int numSamples)
{
    if (m_sampleRate <= 0.0 || m_audioBeatDurationSeconds <= 0.001 || numSamples <= 0)
        return;
    m_audioBeatPosition += static_cast<double>(numSamples) / (m_sampleRate * m_audioBeatDurationSeconds);
}

double FxProcessor::syncedDivisionPhase(float extSec) const
{
    const double beatDur = m_audioBeatDurationSeconds;
    if (extSec < 0.001f || beatDur <= 0.001)
        return 0.0;

    const double divBeats = std::max(0.015625, static_cast<double>(extSec) / beatDur);
    const double beatPos = m_audioBeatPosition;
    double phase = std::fmod(beatPos / divBeats, 1.0);
    if (phase < 0.0)
        phase += 1.0;
    return phase;
}

int FxProcessor::samplesUntilNextDivision(float extSec) const
{
    if (extSec < 0.001f || m_sampleRate <= 0.0)
        return 0;

    const double phase = syncedDivisionPhase(extSec);
    const double remaining = (phase <= 0.002 || phase >= 0.998)
        ? 0.0
        : (1.0 - phase) * static_cast<double>(extSec);
    return std::max(0, static_cast<int>(std::llround(remaining * m_sampleRate)));
}

// ─────────────────────────────────────────────────────────────────────────────
// Reverb
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareReverb()
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = m_sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(m_maxBlockSize);
    spec.numChannels      = static_cast<juce::uint32>(m_numChannels);
    m_reverb.prepare(spec);
    m_reverb.reset();
}

void FxProcessor::updateReverbParams()
{
    // m_primaryParamAtomic (0..1) controls room character.
    // amount (large knob) controls wet/dry through the outer SmoothedValue mixer.
    const float size = std::clamp(m_primaryParamAtomic.load(std::memory_order_relaxed), 0.05f, 1.0f);
    juce::dsp::Reverb::Parameters p;
    p.roomSize   = 0.15f + size * 0.80f;   // 0.15 (tiny) … 0.95 (hall)
    p.damping    = 0.75f - size * 0.45f;   // 0.75 (bright short) … 0.30 (dark long)
    p.wetLevel   = 1.0f;                   // external mixer handles wet/dry
    p.dryLevel   = 0.0f;
    p.width      = 0.55f + size * 0.45f;  // 0.55 … 1.0 (stereo width)
    p.freezeMode = 0.0f;
    m_reverb.setParameters(p);
}

void FxProcessor::setPrimaryParam(float v)
{
    m_primaryParamAtomic.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed);
}

void FxProcessor::processReverb(juce::AudioBuffer<float>& wet, int start, int n)
{
    juce::dsp::AudioBlock<float> block(wet);
    auto sub = block.getSubBlock(static_cast<size_t>(start), static_cast<size_t>(n));
    juce::dsp::ProcessContextReplacing<float> ctx(sub);
    m_reverb.process(ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bitcrusher
// ─────────────────────────────────────────────────────────────────────────────

// Shared soft limiter. Quantisation adds level and hot input used to be
// hard-clamped to ±1 — a second, much harsher distortion on top of the intended
// one. Below the knee the signal passes through untouched.
static inline float fxSoftClip(float x) noexcept
{
    constexpr float knee = 0.8f;
    const float a = std::abs(x);
    if (a <= knee) return x;
    return std::copysign(knee + (1.0f - knee) * std::tanh((a - knee) / (1.0f - knee)), x);
}

void FxProcessor::crushBlock(juce::AudioBuffer<float>& buf, int start, int n, int numChannels,
                             std::vector<BitcrusherState>& state, float rateRatio, float levels)
{
    rateRatio = std::clamp(rateRatio, 0.001f, 1.0f);
    levels    = std::max(1.0f, levels);
    const float invLevels = 1.0f / levels;
    const int numCh = std::min({ buf.getNumChannels(), numChannels,
                                 static_cast<int>(state.size()) });

    for (int ch = 0; ch < numCh; ++ch)
    {
        float* data = buf.getWritePointer(ch) + start;
        auto&  st   = state[static_cast<size_t>(ch)];

        for (int i = 0; i < n; ++i)
        {
            st.phase += rateRatio;
            if (st.phase >= 1.0f)
            {
                // Fractional carry keeps the average decimation rate exact, so
                // the rate sweeps smoothly instead of stepping sr/1, sr/2, sr/3…
                // The sample is taken instantaneously — the aliasing that comes
                // with that is the whole point of the effect.
                st.phase -= 1.0f;
                st.holdSample = fxSoftClip(std::round(data[i] * levels) * invLevels);
            }
            data[i] = st.holdSample;
        }
    }
}

void FxProcessor::processBitcrusher(juce::AudioBuffer<float>& wet,
                                    int start, int n, float amount)
{
    amount = std::clamp(amount, 0.0f, 1.0f);

    // ── Bit depth reduction ───────────────────────────────────────────────────
    // Bit reduction only becomes audible below ~10 bit, so the curve spends most
    // of the travel there: 0.0 → 16 bit (clean), 0.25 → ~10 bit, 0.5 → ~6 bit,
    // 1.0 → 2 bit.
    const float bitDepth = 2.0f + 14.0f * std::pow(1.0f - amount, 1.8f);
    const float levels   = std::pow(2.0f, bitDepth - 1.0f);

    // ── Sample-rate reduction ─────────────────────────────────────────────────
    // Exponential in frequency (one octave down per equal knob step), from the
    // full sample rate down to ~600 Hz.
    const float minRatio = std::clamp(600.0f / static_cast<float>(m_sampleRate), 0.001f, 1.0f);
    const float rateRatio = std::pow(minRatio, amount);

    crushBlock(wet, start, n, m_numChannels, m_bcState, rateRatio, levels);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pitch Shifter
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::preparePitchShifter()
{
    m_pitchShifter->prepare(m_numChannels);
}

void FxProcessor::resetPitchShifterRealtime() noexcept
{
    if (m_pitchShifter)
        m_pitchShifter->resetRealtime();
}

void FxProcessor::resetRollRealtime() noexcept
{
    m_rollState.writePos = 0;
    m_rollState.loopStart = 0;
    m_rollState.loopLen = 0;
    m_rollState.readPos = 0;
    m_rollState.loopActive = false;
    m_rollState.stepCounter = 0;
    m_rollState.doubleCount = 0;
    m_rollState.quantizedStartCountdown = -1;
}

void FxProcessor::processPitchShifter(juce::AudioBuffer<float>& wet,
                                      int start, int n, float amount)
{
    // amount 0.0 → -12 semitones (pitchRatio 0.5)
    // amount 0.5 → 0  semitones  (pitchRatio 1.0)
    // amount 1.0 → +12 semitones (pitchRatio 2.0)
    const double semitones  = (static_cast<double>(amount) - 0.5) * 24.0; // -12 … +12
    const double pitchRatio = std::pow(2.0, semitones / 12.0);
    m_pitchShifter->setPitchRatio(pitchRatio);
    m_pitchShifter->process(wet, start, n);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::copyToWet(const juce::AudioBuffer<float>& src,
                            juce::AudioBuffer<float>& wet,
                            int start, int n)
{
    for (int ch = 0; ch < std::min(src.getNumChannels(), wet.getNumChannels()); ++ch)
        wet.copyFrom(ch, 0, src, ch, start, n);
}

void FxProcessor::mixWetDry(juce::AudioBuffer<float>& buffer,
                            const juce::AudioBuffer<float>& wetBuf,
                            int start, int n,
                            float wetGain, float dryGain)
{
    for (int ch = 0; ch < buffer.getNumChannels() && ch < wetBuf.getNumChannels(); ++ch)
    {
        float* dst       = buffer.getWritePointer(ch) + start;
        const float* wet = wetBuf.getReadPointer(ch);

        for (int i = 0; i < n; ++i)
            dst[i] = dst[i] * dryGain + wet[i] * wetGain;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Echo / Low-Cut Echo
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareDelay()
{
    m_delayState.lineL.prepare(kMaxDelaySamples);
    m_delayState.lineR.prepare(kMaxDelaySamples);
    m_delayState.hpStateL = 0.f;
    m_delayState.hpStateR = 0.f;
    m_delayState.lpBiquadL.reset();
    m_delayState.lpBiquadR.reset();
}

void FxProcessor::processEcho(juce::AudioBuffer<float>& wet,
                              int start, int n, float amount, bool lowCut)
{
    const float extSec = m_externalDelaySeconds.load(std::memory_order_relaxed);
    const float targetDelay = std::clamp(
        extSec >= 0.f
            ? extSec * static_cast<float>(m_sampleRate)
            : static_cast<float>(m_sampleRate * (0.1 + amount * 0.5)),
        1.0f, static_cast<float>(kMaxDelaySamples - 1));
    m_echoDelaySmooth.setTargetValue(targetDelay);

    const float feedback  = 0.28f + amount * 0.32f;
    const float lpHz      = 7000.f - amount * 4500.f;
    const float scaledLp  = lpHz / static_cast<float>(m_sampleRate);
    const float hpAlpha   = lowCut
        ? 1.f / (1.f + 2.f * juce::MathConstants<float>::pi * 200.f
                       / static_cast<float>(m_sampleRate))
        : 0.f;

    // Configure biquad LP (Q=0.85 gives warm resonant roll-off in the feedback)
    m_delayState.lpBiquadL.lowpassQ(scaledLp, 0.85);
    m_delayState.lpBiquadR.lowpassQ(scaledLp, 0.85);

    for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
    {
        float* data      = wet.getWritePointer(ch) + start;
        DelayLine& line  = (ch == 0) ? m_delayState.lineL    : m_delayState.lineR;
        float& hpPrev    = (ch == 0) ? m_delayState.hpStateL : m_delayState.hpStateR;
        auto& lpBiquad   = (ch == 0) ? m_delayState.lpBiquadL : m_delayState.lpBiquadR;

        // Reset smoother for second channel to replay the same ramp.
        if (ch == 1) m_echoDelaySmooth.setCurrentAndTargetValue(
                         m_echoDelaySmooth.getTargetValue());

        for (int i = 0; i < n; ++i)
        {
            const float delayF = m_echoDelaySmooth.getNextValue();
            float delayed = line.readFrac(delayF);

            if (lowCut)
            {
                float hp = delayed - hpPrev;
                hpPrev   = delayed;
                delayed  = hp;
            }

            float warm = lpBiquad(delayed);
            float fb   = std::tanh(warm * feedback * 1.3f) / 1.3f;

            line.write(data[i] * 0.95f + fb);
            data[i] = data[i] + warm * 0.80f;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MT Delay (Multi-Tap: 3 taps at 1/4, 1/2, 3/4 of max delay)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processMtDelay(juce::AudioBuffer<float>& wet,
                                  int start, int n, float amount)
{
    const float extSec = m_externalDelaySeconds.load(std::memory_order_relaxed);
    const float targetMax = std::clamp(
        extSec >= 0.f
            ? extSec * static_cast<float>(m_sampleRate)
            : static_cast<float>(m_sampleRate * (0.1 + amount * 0.5)),
        4.0f, static_cast<float>(kMaxDelaySamples - 1));
    m_mtDelayTimeSmooth.setTargetValue(targetMax);

    const float feedback = 0.18f + amount * 0.30f;
    const float lpHz     = 8000.f - amount * 5000.f;
    const float scaledLp = lpHz / static_cast<float>(m_sampleRate);

    m_delayState.lpBiquadL.lowpassQ(scaledLp, 0.85);
    m_delayState.lpBiquadR.lowpassQ(scaledLp, 0.85);

    for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
    {
        float* data    = wet.getWritePointer(ch) + start;
        DelayLine& line = (ch == 0) ? m_delayState.lineL    : m_delayState.lineR;
        auto& lpBiquad  = (ch == 0) ? m_delayState.lpBiquadL : m_delayState.lpBiquadR;

        if (ch == 1) m_mtDelayTimeSmooth.setCurrentAndTargetValue(
                         m_mtDelayTimeSmooth.getTargetValue());

        for (int i = 0; i < n; ++i)
        {
            const float maxD = m_mtDelayTimeSmooth.getNextValue();
            const float t1 = line.readFrac(std::max(1.0f, maxD * 0.25f));
            const float t2 = line.readFrac(std::max(1.0f, maxD * 0.50f));
            const float t3 = line.readFrac(std::max(1.0f, maxD * 0.75f));
            const float tapMix = t1 * 0.43f + t2 * 0.34f + t3 * 0.23f;
            const float warm   = lpBiquad(tapMix);
            const float fb     = std::tanh(warm * feedback);
            const float out    = data[i] + fb * 0.90f;
            line.write(data[i] * 0.95f + fb * 0.80f);
            data[i] = out;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Spiral (chorus-style modulated delay)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareSpiral()
{
    const int bufSize = static_cast<int>(m_sampleRate * 0.05); // 50ms max
    m_spiralState.lineL.prepare(bufSize);
    m_spiralState.lineR.prepare(bufSize);
    m_spiralState.lfo.prepare(m_sampleRate);
    m_spiralState.lfo.setRateImmediate(0.5f);
}

void FxProcessor::processSpiral(juce::AudioBuffer<float>& wet,
                                 int start, int n, float amount)
{
    const float extSec    = m_externalDelaySeconds.load(std::memory_order_relaxed);
    const float lfoRate   = (extSec >= 0.001f)
        ? std::clamp(1.0f / extSec, 0.01f, 20.f)
        : 0.15f + amount * 2.35f;
    const float modDepth  = static_cast<float>(m_sampleRate) * (0.004f + amount * 0.014f);
    const float baseDelay = static_cast<float>(m_sampleRate) * 0.018f;

    m_spiralState.lfo.setRate(lfoRate);
    if (extSec >= 0.001f)
        m_spiralState.lfo.setPhase(syncedDivisionPhase(extSec));

    const int numCh = std::min(wet.getNumChannels(), 2);

    for (int i = 0; i < n; ++i)
    {
        m_spiralState.lfo.tick();

        for (int ch = 0; ch < numCh; ++ch)
        {
            float* data    = wet.getWritePointer(ch) + start;
            auto& line     = (ch == 0) ? m_spiralState.lineL : m_spiralState.lineR;
            // 0° for L, 90° offset for R (stereo chorus width)
            const float lfoVal = m_spiralState.lfo.sine(ch == 0 ? 0.f : 0.25f);

            const float delF   = std::max(1.0f, baseDelay + lfoVal * modDepth);
            const float delayed = line.readFrac(delF);
            line.write(data[i]);
            data[i] = data[i] * 0.70f + delayed * 0.40f;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Flanger
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareFlanger()
{
    const int bufSize = static_cast<int>(m_sampleRate * 0.015); // 15ms max
    m_flangerState.lineL.prepare(bufSize);
    m_flangerState.lineR.prepare(bufSize);
    m_flangerState.lfo.prepare(m_sampleRate);
    m_flangerState.lfo.setRateImmediate(0.3f);
}

void FxProcessor::processFlanger(juce::AudioBuffer<float>& wet,
                                  int start, int n, float amount)
{
    const float extSec    = m_externalDelaySeconds.load(std::memory_order_relaxed);
    const float lfoRate   = (extSec >= 0.001f)
        ? std::clamp(1.0f / extSec, 0.01f, 20.f)
        : 0.05f + amount * 1.45f;
    const float modDepth  = static_cast<float>(m_sampleRate) * (0.0005f + amount * 0.0075f);
    const float baseDelay = static_cast<float>(m_sampleRate) * 0.002f;
    const float feedback  = 0.20f + amount * 0.62f;

    m_flangerState.lfo.setRate(lfoRate);
    if (extSec >= 0.001f)
        m_flangerState.lfo.setPhase(syncedDivisionPhase(extSec));

    const int numCh = std::min(wet.getNumChannels(), 2);

    for (int i = 0; i < n; ++i)
    {
        m_flangerState.lfo.tick();

        for (int ch = 0; ch < numCh; ++ch)
        {
            float* data  = wet.getWritePointer(ch) + start;
            auto& line   = (ch == 0) ? m_flangerState.lineL : m_flangerState.lineR;
            // 0° for L, 180° for R — maximum stereo spread
            const float lfoVal  = m_flangerState.lfo.sine(ch == 0 ? 0.f : 0.5f);
            const float delF    = std::max(1.0f, baseDelay + lfoVal * modDepth);
            const float delayed = line.readFrac(delF);

            line.write(data[i] + std::tanh(delayed * feedback) * 0.85f);
            data[i] = (data[i] + delayed) * 0.7071f;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Phaser (4-stage all-pass chain with LFO-swept frequency)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processPhaser(juce::AudioBuffer<float>& wet,
                                 int start, int n, float amount)
{
    const float extSec   = m_externalDelaySeconds.load(std::memory_order_relaxed);
    const float lfoRate  = (extSec >= 0.001f)
        ? std::clamp(1.0f / extSec, 0.01f, 20.f)
        : 0.1f + amount * 4.9f;
    const float fMin     = 100.f, fMax = 6000.f;
    const float feedback = 0.30f + amount * 0.55f;
    const float twoPiOverSr = 2.f * juce::MathConstants<float>::pi
                              / static_cast<float>(m_sampleRate);
    const int numCh = std::min(wet.getNumChannels(), 2);

    m_phaserState.lfo.setRate(lfoRate);
    if (extSec >= 0.001f)
        m_phaserState.lfo.setPhase(syncedDivisionPhase(extSec));

    for (int i = 0; i < n; ++i)
    {
        m_phaserState.lfo.tick();
        const float lfo   = m_phaserState.lfo.sineUnipolar();
        const float freq  = fMin + lfo * (fMax - fMin);
        const float tanH  = std::tan(twoPiOverSr * freq * 0.5f);
        const float coeff = (tanH - 1.f) / (tanH + 1.f);

        for (int ch = 0; ch < numCh; ++ch)
        {
            float x = wet.getWritePointer(ch)[start + i];
            for (int s = 0; s < 4; ++s)
            {
                float xn = x;
                float yn = coeff * (xn - m_phaserState.yPrev[ch][s])
                         + m_phaserState.xPrev[ch][s];
                m_phaserState.xPrev[ch][s] = xn;
                m_phaserState.yPrev[ch][s] = yn;
                x = yn;
            }
            wet.getWritePointer(ch)[start + i] += x * feedback;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Trans (Tremolo – amplitude LFO)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processTrans(juce::AudioBuffer<float>& wet,
                                int start, int n, float amount)
{
    const float extSec   = m_externalDelaySeconds.load(std::memory_order_relaxed);
    // Tremolo is most musical when the LFO rate matches the beat division.
    // At extSec = 0.5 (1/4 note, 120 BPM) lfoRate = 2 Hz = 2 trems per beat.
    // Divide by 2 so one LFO cycle = one beat division (dip then up = 1 cycle).
    const float lfoRate  = (extSec >= 0.001f)
        ? std::clamp(1.0f / extSec, 0.1f, 32.f)
        : 1.f + amount * 15.f;
    const float depth    = 0.25f + amount * 0.75f;
    const float hardness = 1.0f + amount * 10.0f;

    m_transState.lfo.setRate(lfoRate);
    if (extSec >= 0.001f)
        m_transState.lfo.setPhase(syncedDivisionPhase(extSec));

    for (int i = 0; i < n; ++i)
    {
        m_transState.lfo.tick();
        const float sineVal = m_transState.lfo.sine();
        float lfo  = std::tanh(sineVal * hardness);
        lfo = 0.5f * (1.f + lfo);
        const float gain = 1.f - depth * (1.f - lfo);

        for (int ch = 0; ch < wet.getNumChannels(); ++ch)
            wet.getWritePointer(ch)[start + i] *= gain;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Enigma Jet (deep phaser with 8 stages + small pitch-detune)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::prepareEnigma()
{
    const int bufSize = static_cast<int>(m_sampleRate * 0.02); // 20ms
    m_enigmaState.detLineL.prepare(bufSize);
    m_enigmaState.detLineR.prepare(bufSize);
    m_enigmaState.phaseLfo.prepare(m_sampleRate);
    m_enigmaState.phaseLfo.setRateImmediate(0.1f);
    m_enigmaState.detLfo.prepare(m_sampleRate);
    m_enigmaState.detLfo.setRateImmediate(0.5f);
}

void FxProcessor::processEnigmaJet(juce::AudioBuffer<float>& wet,
                                    int start, int n, float amount)
{
    const float extSec       = m_externalDelaySeconds.load(std::memory_order_relaxed);
    const float phaseLfoRate = (extSec >= 0.001f)
        ? std::clamp(1.0f / extSec, 0.01f, 10.f)
        : 0.03f + amount * 0.67f;
    const float twoPiOverSr  = 2.f * juce::MathConstants<float>::pi
                               / static_cast<float>(m_sampleRate);
    const float fMin     = 60.f, fMax = 10000.f;
    const float feedback = 0.65f + amount * 0.12f;

    const float detLfoRate = 0.2f + amount * 1.5f;
    const float detDepth   = static_cast<float>(m_sampleRate) * (0.003f + amount * 0.007f);

    m_enigmaState.phaseLfo.setRate(phaseLfoRate);
    if (extSec >= 0.001f)
        m_enigmaState.phaseLfo.setPhase(syncedDivisionPhase(extSec));
    m_enigmaState.detLfo.setRate(detLfoRate);

    const int numCh = std::min(wet.getNumChannels(), 2);

    for (int i = 0; i < n; ++i)
    {
        m_enigmaState.phaseLfo.tick();
        m_enigmaState.detLfo.tick();

        const float lfo      = m_enigmaState.phaseLfo.sineUnipolar();
        const float detLfoV  = m_enigmaState.detLfo.sine();
        const float freq     = fMin + lfo * (fMax - fMin);
        const float tanHalf  = std::tan(twoPiOverSr * freq * 0.5f);
        const float coeff    = (tanHalf - 1.f) / (tanHalf + 1.f);
        const float delF     = std::max(1.0f, detDepth * 0.5f * (1.f + detLfoV));

        for (int ch = 0; ch < numCh; ++ch)
        {
            float* data = wet.getWritePointer(ch) + start;
            auto& detLine = (ch == 0) ? m_enigmaState.detLineL : m_enigmaState.detLineR;

            float x = data[i];
            for (int s = 0; s < 8; ++s)
            {
                float xn = x;
                float yn = coeff * (xn - m_enigmaState.yPrev[ch][s])
                         + m_enigmaState.xPrev[ch][s];
                m_enigmaState.xPrev[ch][s] = xn;
                m_enigmaState.yPrev[ch][s] = yn;
                x = yn;
            }
            float phased  = data[i] + x * feedback;
            float delayed = detLine.readFrac(delF);
            detLine.write(phased);
            data[i] = phased * 0.6f + delayed * 0.4f;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stretch (granular freeze – slows read pointer to stretch audio in time)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processStretch(juce::AudioBuffer<float>& wet,
                                  int start, int n, float amount)
{
    // amount 0 = 1x (normal), amount 1 = 0.25x (4x slower)
    const double readSpeed = 1.0 - static_cast<double>(amount) * 0.75;
    const int    bufLen    = kStretchBuf;

    for (int i = 0; i < n; ++i)
    {
        // Write to buffer (both channels)
        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
            m_stretchState.buf[ch][m_stretchState.writePos % bufLen]
                = wet.getReadPointer(ch)[start + i];

        // Linear-interpolated read
        int    rp   = static_cast<int>(m_stretchState.readPos);
        double frac = m_stretchState.readPos - rp;
        int    rp0  = rp % bufLen;
        int    rp1  = (rp + 1) % bufLen;

        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
        {
            float s0 = m_stretchState.buf[ch][rp0];
            float s1 = m_stretchState.buf[ch][rp1];
            wet.getWritePointer(ch)[start + i] = s0 + static_cast<float>(frac) * (s1 - s0);
        }

        m_stretchState.writePos = (m_stretchState.writePos + 1) % bufLen;
        m_stretchState.readPos += readSpeed;
        if (static_cast<int>(m_stretchState.readPos) >= bufLen)
            m_stretchState.readPos -= bufLen;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Roll (stutter beat-repeat) / Slip Roll (with reverse slipping)
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processRoll(juce::AudioBuffer<float>& wet,
                               int start, int n, float amount, bool slip)
{
    const float extSec = m_externalDelaySeconds.load(std::memory_order_relaxed);
    const int targetLen = (extSec >= 0.f)
        ? static_cast<int>(extSec * static_cast<float>(m_sampleRate))
        : static_cast<int>(m_sampleRate * std::pow(2.f, -amount * 2.5f) * 0.5f);
    const int loopLen   = std::max(64, std::min(targetLen, kRollBuf / 2));

    auto& st = m_rollState;

    for (int i = 0; i < n; ++i)
    {
        if (!st.loopActive && extSec >= 0.f) {
            if (st.quantizedStartCountdown < 0)
                st.quantizedStartCountdown = samplesUntilNextDivision(extSec);
            if (st.quantizedStartCountdown > 0) {
                if (slip)
                    for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
                        st.buf[ch][st.writePos % kRollBuf] = wet.getReadPointer(ch)[start + i];
                st.writePos = (st.writePos + 1) % kRollBuf;
                --st.quantizedStartCountdown;
                continue;
            }
        }

        // For non-slip roll: only write during the initial fill phase (first loopLen samples).
        // After that the buffer is frozen — writePos never advances again, so the captured
        // loop region is never overwritten no matter how long the pad is held.
        // For slip roll: always write so the advancing loopStart reads fresh audio.
        const bool writing = slip || !st.loopActive || st.stepCounter < st.loopLen;

        if (writing)
            for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
                st.buf[ch][st.writePos % kRollBuf] = wet.getReadPointer(ch)[start + i];

        if (!st.loopActive || st.loopLen != loopLen)
        {
            st.loopStart   = st.writePos;
            st.loopLen     = loopLen;
            st.readPos     = st.loopStart;
            st.loopActive  = true;
            st.stepCounter = 0;
            st.quantizedStartCountdown = -1;
        }

        // Read from frozen loop
        const int rp = st.readPos % kRollBuf;
        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
            wet.getWritePointer(ch)[start + i] = st.buf[ch][rp];

        if (writing)
            st.writePos = (st.writePos + 1) % kRollBuf;
        ++st.stepCounter;

        if (slip)
        {
            // Slip roll: every loop iteration slips back one step
            const int slipAmount = std::max(1, loopLen / 16);
            if (st.stepCounter >= loopLen)
            {
                st.stepCounter = 0;
                st.loopStart   = (st.loopStart - slipAmount + kRollBuf) % kRollBuf;
                st.readPos     = st.loopStart;
            }
            else
            {
                st.readPos = (st.loopStart + st.stepCounter) % kRollBuf;
            }
        }
        else
        {
            // Regular roll: seamless loop repeat
            st.readPos = (st.loopStart + (st.stepCounter % st.loopLen)) % kRollBuf;
        }
    }
}

void FxProcessor::processRollOut(juce::AudioBuffer<float>& wet,
                                  int start, int n, float amount)
{
    const float extSec = m_externalDelaySeconds.load(std::memory_order_relaxed);
    const int baseLen  = std::max(64, (extSec >= 0.f)
        ? static_cast<int>(extSec * static_cast<float>(m_sampleRate))
        : static_cast<int>(m_sampleRate * std::pow(2.f, -amount * 2.5f) * 0.5f));

    auto& st = m_rollState;

    for (int i = 0; i < n; ++i)
    {
        if (!st.loopActive && extSec >= 0.f) {
            if (st.quantizedStartCountdown < 0)
                st.quantizedStartCountdown = samplesUntilNextDivision(extSec);
            if (st.quantizedStartCountdown > 0) {
                for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
                    st.buf[ch][st.writePos % kRollBuf] = wet.getReadPointer(ch)[start + i];
                st.writePos = (st.writePos + 1) % kRollBuf;
                --st.quantizedStartCountdown;
                continue;
            }
        }

        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
            st.buf[ch][st.writePos % kRollBuf] = wet.getReadPointer(ch)[start + i];

        if (!st.loopActive)
        {
            st.loopStart   = st.writePos;
            st.loopLen     = baseLen;
            st.readPos     = st.loopStart;
            st.loopActive  = true;
            st.stepCounter = 0;
            st.doubleCount = 0;
            st.quantizedStartCountdown = -1;
        }

        const int rp = st.readPos % kRollBuf;
        for (int ch = 0; ch < wet.getNumChannels() && ch < 2; ++ch)
            wet.getWritePointer(ch)[start + i] = st.buf[ch][rp];

        st.writePos = (st.writePos + 1) % kRollBuf;
        ++st.stepCounter;

        if (st.stepCounter >= st.loopLen)
        {
            st.stepCounter = 0;
            if (st.doubleCount < 4)
            {
                ++st.doubleCount;
                const int newLen = std::min(st.loopLen * 2, kRollBuf / 2);
                st.loopStart = st.writePos;  // capture fresh audio for longer loop
                st.loopLen   = newLen;
                st.readPos   = st.loopStart;
            }
            else
            {
                st.readPos = st.loopStart;  // hold at max length
            }
        }
        else
        {
            st.readPos = (st.loopStart + (st.stepCounter % st.loopLen)) % kRollBuf;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mobius Saw / Mobius Tri — endlessly gliding synth tone (Shepard glissando).
//
// kMobiusVoices oscillators sit an octave apart and all glide upward at the same
// rate. A raised-cosine window over the octave stack fades the top voice out as
// a fresh one fades in at the bottom, so the tone rises without bound while its
// spectrum stays put — it sounds like it climbs forever.
//   Saw: sawtooth oscillators, bright and buzzy (polyBLEP keeps the reset from
//        aliasing across the whole glide range).
//   Tri: triangle oscillators, soft and hollow.
// The glide period follows the beat division; the knob sets how loud the tone
// sits over the music.
// ─────────────────────────────────────────────────────────────────────────────

void FxProcessor::processMobius(juce::AudioBuffer<float>& wet,
                                 int start, int n, float amount, bool sawtooth)
{
    const float extSec = m_externalDelaySeconds.load(std::memory_order_relaxed);
    // One full trip through the octave stack per glide cycle. Beat-synced when
    // a length is supplied, otherwise the knob picks something musical.
    const double cycleSeconds = (extSec > 0.f)
        ? std::max(0.05, static_cast<double>(extSec) * 4.0)
        : (4.0 - 3.5 * static_cast<double>(amount));
    const double sweepPerSample =
        static_cast<double>(kMobiusVoices) / (cycleSeconds * m_sampleRate);

    // The tone rides on top of the incoming signal; the effect's wet/dry mix
    // then decides how far it comes forward.
    const float level = 0.12f + 0.5f * amount;

    auto& st = m_mobiusState;
    const int channels = std::min(2, wet.getNumChannels());

    // A polyBLEP step correction removes the alias burst a naive sawtooth
    // produces at its wrap; without it the upper voices buzz.
    const auto polyBlep = [](double t, double dt) noexcept -> double {
        if (t < dt)          { const double x = t / dt;       return x + x - x * x - 1.0; }
        if (t > 1.0 - dt)    { const double x = (t - 1.0) / dt; return x * x + x + x + 1.0; }
        return 0.0;
    };

    for (int i = 0; i < n; ++i)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            // Half a voice of offset between the channels widens the stack
            // without detuning it.
            const double sweep = st.sweep + (ch == 1 ? 0.5 : 0.0);
            double tone = 0.0;
            double norm = 0.0;

            for (int v = 0; v < kMobiusVoices; ++v)
            {
                double octave = sweep + static_cast<double>(v);
                if (octave >= kMobiusVoices)
                    octave -= kMobiusVoices;

                const double freq = kMobiusBaseHz * std::pow(2.0, octave);
                const double dt   = freq / m_sampleRate;
                if (dt >= 0.5)      // above Nyquist, nothing useful left to add
                    continue;

                double& phase = st.phase[ch][v];
                phase += dt;
                if (phase >= 1.0)
                    phase -= 1.0;

                double value;
                if (sawtooth)
                    value = 2.0 * phase - 1.0 - polyBlep(phase, dt);
                else
                    value = 4.0 * std::abs(phase - 0.5) - 1.0;

                // Raised cosine over the stack: silent at both ends, so voices
                // enter and leave without a step.
                const double window = 0.5 - 0.5 * std::cos(
                    2.0 * juce::MathConstants<double>::pi
                        * octave / static_cast<double>(kMobiusVoices));
                tone += value * window;
                norm += window;
            }

            if (norm > 1.0e-6)
                tone /= norm;

            float* const out = wet.getWritePointer(ch);
            out[start + i] += static_cast<float>(tone) * level;
        }

        st.sweep += sweepPerSample;
        if (st.sweep >= kMobiusVoices)
            st.sweep -= kMobiusVoices;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// Sound Color FX — Pioneer DDJ-FLX10 style
//
// All SC effects receive a bipolar knob value: -1.0 (max left) … 0.0 (centre/
// bypass) … +1.0 (max right).  At 0.0 the signal passes through 100% dry.
//
// Architecture: [Effect core] → [Bipolar SVF LPF/HPF] → mix back into buffer.
//
// The SVF (State Variable Filter) is used for smooth morphing because it:
//  • Has no instability risk across the full frequency range
//  • Naturally separates LP and HP outputs in one pass
//  • Allows Q to be modulated without artefacts
// ─────────────────────────────────────────────────────────────────────────────

// ── Prepare SC delay lines ────────────────────────────────────────────────────
void FxProcessor::prepareSCDelays()
{
    m_scDubEchoState.lineL.prepare(kMaxDelaySamples);
    m_scDubEchoState.lineR.prepare(kMaxDelaySamples);
    m_scDubEchoState.lpL = 0.f;
    m_scDubEchoState.lpR = 0.f;
    m_scDubEchoState.svf.prepare(m_sampleRate);
    m_scFilterState.svfA.prepare(m_sampleRate);
    m_scFilterState.svfB.prepare(m_sampleRate);
    m_scSpaceState.svf  .prepare(m_sampleRate);
    m_scNoiseState.svf  .prepare(m_sampleRate);
    m_scNoiseState.seed  = { 12345u, 987654321u };
    m_scCrushState.svf  .prepare(m_sampleRate);
    m_scSweepState.svfA .prepare(m_sampleRate);
    m_scSweepState.svfB .prepare(m_sampleRate);

    const float sr = static_cast<float>(m_sampleRate);

    // Shared wet/dry smoother for all SC effects: 15 ms ramp
    m_scAbsKSmooth.reset(sr, 0.015f);
    m_scAbsKSmooth.setCurrentAndTargetValue(0.f);

    // DubEcho dedicated feedback smoother: 20 ms ramp prevents gain spikes
    m_scDubFbSmooth.reset(sr, 0.020f);
    m_scDubFbSmooth.setCurrentAndTargetValue(0.5f);
}

// ── SC shared wet/dry mixer ───────────────────────────────────────────────────
//
// Blends wet into dst using m_scAbsKSmooth for per-sample gain ramping.
// Caller must call m_scAbsKSmooth.setTargetValue() before invoking this.
// Samples outer / channels inner so the smoother advances exactly n times.
void FxProcessor::mixSCSmoothed(juce::AudioBuffer<float>& dst,
                                 const juce::AudioBuffer<float>& wet,
                                 int start, int n)
{
    const int nc = std::min(dst.getNumChannels(), m_numChannels);
    const float* srcs[2] = {
        wet.getReadPointer(0),
        nc > 1 ? wet.getReadPointer(1) : wet.getReadPointer(0)
    };
    float* dsts[2] = {
        dst.getWritePointer(0) + start,
        nc > 1 ? dst.getWritePointer(1) + start : dst.getWritePointer(0) + start
    };
    for (int i = 0; i < n; ++i) {
        const float w = m_scAbsKSmooth.getNextValue();
        const float d = 1.f - w;
        for (int ch = 0; ch < nc; ++ch)
            dsts[ch][i] = dsts[ch][i] * d + srcs[ch][i] * w;
    }
}

// ── SC knob response curves ────────────────────────────────────────────────────
// t ∈ [0,1] → [0,1]. Applied to |knob| before DSP so the perceptual response
// matches professional DJ mixer behavior — most control in the musically useful range.
static float scMapSCurve(float t)    { return t * t * (3.f - 2.f * t); }
// Cutoff travel for the filter-style modes. An S-curve races through the ends
// of the sweep, so three quarters of the knob already sat below 100 Hz; this
// keeps a small dead zone at the detent and spends the rest of the travel in
// the range that is actually musical (half travel ≈ 1.2 kHz).
static float scMapFilterTravel(float t) { return std::pow(t, 1.35f); }
static float scMapExpRamp(float t)   { return std::pow(t, 1.5f); }
static float scMapFastOnset(float t) { return 1.f - (1.f - t) * (1.f - t); }

// ── Bipolar SVF helper ────────────────────────────────────────────────────────
//
// Delegates per-sample processing to dsp::SvfSmoothed so filter coefficients
// ramp smoothly within the block — no zipper noise when turning the knob.
// knob < 0 → LPF, knob > 0 → HPF, knob = 0 → bypass.
// Returns |knob| as wet gain (0 = centre/bypass, 1 = full effect).
float FxProcessor::applySCFilter(juce::AudioBuffer<float>& buf, int start, int n,
                                  float knob, SVFState& state)
{
    const float absK = std::abs(knob);
    if (absK < 0.005f) return 0.f;

    // Exponential cutoff mapping: same curve as before, now set as smooth target.
    float fc;
    if (knob < 0.f)
        fc = 20000.f * std::pow(20.f / 20000.f, absK);
    else
        fc = 20.f * std::pow(20000.f / 20.f, absK);
    fc = std::clamp(fc, 20.f, 20000.f);

    const float q = 0.7f + absK * 1.1f;

    state.setTargets(fc, q);
    state.process(buf, start, n, knob < 0.f);

    return absK;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. SC FILTER — pure dual resonant LPF/HPF
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Filter(juce::AudioBuffer<float>& buffer,
                                    int start, int n, float knob)
{
    const float absK   = std::abs(knob);
    const float mapped = scMapFilterTravel(absK);
    // A filter is a series element, not a parallel one: it goes fully wet as
    // soon as the knob leaves the detent (where the cutoff sits at the end of
    // its range and is inaudible anyway). Blending dry signal back in across the
    // whole travel is what made the filter sound half-engaged in mid positions.
    const float wet = std::min(1.f, absK * 10.f);
    m_scAbsKSmooth.setTargetValue(wet);
    if (!m_scAbsKSmooth.isSmoothing() && wet < 0.005f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);
    const float q     = std::max(0.2f, 0.70f + param * 9.30f);

    // Cutoff travel stops just short of the audible edges: full left is a deep
    // rumble rather than digital silence, full right keeps a trace of air.
    constexpr float kFcLow = 25.f, kFcHigh = 15000.f;
    const float fc = (knob < 0.f)
        ? kFcHigh * std::pow(kFcLow / kFcHigh, mapped)
        : kFcLow  * std::pow(kFcHigh / kFcLow, mapped);

    auto& wetBuf = m_wetScratch;
    copyToWet(buffer, wetBuf, start, n);

    // 24 dB/oct: two cascaded stages. Only the second one carries the resonance
    // — stacking two resonant stages multiplies the peak into a +20 dB spike.
    const bool lp = (knob < 0.f);
    m_scFilterState.svfA.setTargets(fc, 0.707f);
    m_scFilterState.svfB.setTargets(fc, q);
    m_scFilterState.svfA.process(wetBuf, 0, n, lp);
    m_scFilterState.svfB.process(wetBuf, 0, n, lp);

    // Soft saturation for resonance edge emphasis. Normalised by tanh(gain) so
    // it colours the signal without pulling the overall level down, and skipped
    // entirely at low resonance where it would only cost headroom.
    const float edgeGain = 1.0f + param * 0.9f;
    if (edgeGain > 1.01f) {
        const float norm = 1.0f / edgeGain;   // unity for small signals
        const int numChEdge = std::min(wetBuf.getNumChannels(), m_numChannels);
        for (int ch = 0; ch < numChEdge; ++ch) {
            float* d = wetBuf.getWritePointer(ch);
            for (int i = 0; i < n; ++i)
                d[i] = std::tanh(d[i] * edgeGain) * norm;
        }
    }

    mixSCSmoothed(buffer, wetBuf, start, n);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. SC DUB ECHO — delay + bipolar LPF/HPF on the wet tail
//    knob < 0 → dark dub echo (delay + LPF in feedback)
//    knob > 0 → bright sibilant echo (delay + HPF in feedback)
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_DubEcho(juce::AudioBuffer<float>& buffer,
                                     int start, int n, float knob)
{
    const float absK   = std::abs(knob);
    const float mapped = scMapSCurve(absK);
    // The wet buffer carries the echo tail alone, so the blend has to leave dry
    // signal in the mix — at 100 % wet with the tail filtered to the edge of the
    // spectrum the channel used to disappear completely at the knob extremes.
    const float wet = mapped * 0.70f;
    m_scAbsKSmooth.setTargetValue(wet < 0.005f ? 0.f : wet);
    if (!m_scAbsKSmooth.isSmoothing() && wet < 0.005f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);

    const double delaySec = 0.125 + static_cast<double>(param) * (1.50 - 0.125);
    const float targetDelay = std::clamp(
        static_cast<float>(m_sampleRate * delaySec),
        1.0f, static_cast<float>(kMaxDelaySamples - 1));
    m_scDubDelaySmooth.setTargetValue(targetDelay);

    // Feedback smoothed independently — prevents gain spikes on rapid knob moves
    const float targetFb = std::clamp(0.30f + 0.45f * param + 0.15f * absK, 0.2f, 0.88f);
    m_scDubFbSmooth.setTargetValue(targetFb);

    const float tapeLpHz  = 12000.0f - param * 11000.0f;
    const float tapeAlpha = std::clamp(2.0f * juce::MathConstants<float>::pi
                                       * (tapeLpHz / static_cast<float>(m_sampleRate)),
                                       0.001f, 0.99f);

    auto& wetBuf = m_wetScratch;
    copyToWet(buffer, wetBuf, start, n);

    const int numCh = std::min(wetBuf.getNumChannels(), 2);
    for (int ch = 0; ch < numCh; ++ch)
    {
        float* data     = wetBuf.getWritePointer(ch);
        DelayLine& line = (ch == 0) ? m_scDubEchoState.lineL : m_scDubEchoState.lineR;

        // Ch 1 resets both delay and feedback smoothers to their target so both
        // channels track the same final value throughout the block.
        if (ch == 1) {
            m_scDubDelaySmooth.setCurrentAndTargetValue(m_scDubDelaySmooth.getTargetValue());
            m_scDubFbSmooth   .setCurrentAndTargetValue(m_scDubFbSmooth   .getTargetValue());
        }

        for (int i = 0; i < n; ++i)
        {
            const float delayF   = m_scDubDelaySmooth.getNextValue();
            const float feedback = m_scDubFbSmooth   .getNextValue();
            float delayed = line.readFrac(delayF);

            float& lpState = (ch == 0) ? m_scDubEchoState.lpL : m_scDubEchoState.lpR;
            lpState += tapeAlpha * (delayed - lpState);

            float fb  = std::tanh(lpState * feedback);
            float out = data[i] + fb;
            line.write(out);
            data[i] = lpState;
        }
    }

    // Partial filter travel keeps the tail dark/bright rather than silent.
    applySCFilter(wetBuf, 0, n, (knob < 0.f ? -1.f : 1.f) * mapped * 0.55f, m_scDubEchoState.svf);
    mixSCSmoothed(buffer, wetBuf, start, n);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. SC CRUSH — bitcrusher + bipolar LPF/HPF
//    knob < 0 → crunchy + dark (LPF closes as crushing increases)
//    knob > 0 → crunchy + bright/thin (HPF opens as crushing increases)
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Crush(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob)
{
    const float absK      = std::abs(knob);
    const float mapped    = scMapFastOnset(absK);
    const float param     = m_scParamAtomic.load(std::memory_order_relaxed);
    const float intensity = std::clamp(mapped * (0.15f + 0.85f * param), 0.0f, 1.0f);

    m_scAbsKSmooth.setTargetValue(intensity);
    if (!m_scAbsKSmooth.isSmoothing() && intensity < 0.001f) return;

    auto& wetBuf = m_wetScratch;
    copyToWet(buffer, wetBuf, start, n);

    // Same curves as the Beat FX crusher: 16 → 2 bit, full rate → ~600 Hz.
    const float bitDepth  = 2.f + 14.f * std::pow(1.f - intensity, 1.8f);
    const float levels    = std::pow(2.f, bitDepth - 1.f);
    const float minRatio  = std::clamp(600.f / static_cast<float>(m_sampleRate), 0.001f, 1.f);
    const float rateRatio = std::pow(minRatio, intensity);

    jassert(m_scCrushState.bc.size() >= static_cast<size_t>(m_numChannels));
    if (m_scCrushState.bc.size() < static_cast<size_t>(m_numChannels))
        return;

    crushBlock(wetBuf, 0, n, m_numChannels, m_scCrushState.bc, rateRatio, levels);

    // Tone shaping: only the outer half of the knob travel engages the filter,
    // and only over a limited range — the first half stays pure crush, and the
    // end of the travel darkens/thins the sound instead of filtering it away.
    const float toneAmount = std::max(0.f, (mapped - 0.5f) * 2.f) * 0.6f;
    applySCFilter(wetBuf, 0, n, (knob < 0.f ? -1.f : 1.f) * toneAmount, m_scCrushState.svf);
    mixSCSmoothed(buffer, wetBuf, start, n);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. SC SPACE — reverb + bipolar LPF/HPF on wet
//    knob < 0 → dark hall (LPF on reverb tail)
//    knob > 0 → bright icy hall (HPF on reverb tail, only sibilants echo)
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Space(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob)
{
    const float absK   = std::abs(knob);
    const float mapped = scMapSCurve(absK);
    // Reverb-only wet buffer — keep dry in the mix (see processSC_DubEcho).
    const float wet = mapped * 0.65f;
    m_scAbsKSmooth.setTargetValue(wet < 0.005f ? 0.f : wet);
    if (!m_scAbsKSmooth.isSmoothing() && wet < 0.005f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);

    auto& wetBuf = m_wetScratch;
    copyToWet(buffer, wetBuf, start, n);

    juce::dsp::Reverb::Parameters p;
    p.roomSize   = 0.18f + param * 0.82f;
    p.damping    = 0.85f - param * 0.70f;
    p.wetLevel   = 1.f;
    p.dryLevel   = 0.f;
    p.width      = 0.55f + param * 0.45f;
    p.freezeMode = std::clamp((param - 0.90f) * 2.0f, 0.0f, 1.0f);
    m_reverb.setParameters(p);

    processReverb(wetBuf, 0, n);
    applySCFilter(wetBuf, 0, n, (knob < 0.f ? -1.f : 1.f) * mapped * 0.55f, m_scSpaceState.svf);
    mixSCSmoothed(buffer, wetBuf, start, n);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. SC PITCH — ±12 semitones, no filter, wet fades in quickly from centre
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Pitch(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob)
{
    const float absK = std::abs(knob);
    // Fast ramp: full wet at |knob| > 0.15
    const float targetWet = std::min(1.f, absK * 6.f);
    m_scAbsKSmooth.setTargetValue(targetWet);
    if (!m_scAbsKSmooth.isSmoothing() && targetWet < 0.001f) return;

    const float param = m_scParamAtomic.load(std::memory_order_relaxed);

    auto& wetBuf = m_wetScratch;
    copyToWet(buffer, wetBuf, start, n);

    const double maxRangeSemitones = 1.0 + static_cast<double>(param) * 35.0;
    const double semitones  = static_cast<double>(knob) * maxRangeSemitones;
    const double pitchRatio = std::pow(2.0, semitones / 12.0);
    m_pitchShifter->setPitchRatio(pitchRatio);
    m_pitchShifter->process(wetBuf, 0, n);

    mixSCSmoothed(buffer, wetBuf, start, n);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. SC NOISE — additive white noise through bipolar LPF/HPF
//    knob < 0 → low-pass filtered noise (wind/rumble sweep)
//    knob > 0 → high-pass filtered noise (sibilant hiss sweep)
//    Track audio runs 100% dry underneath — noise is purely additive.
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Noise(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob)
{
    const float absK   = std::abs(knob);
    const float mapped = scMapExpRamp(absK);
    const float param  = m_scParamAtomic.load(std::memory_order_relaxed);

    // Cutoff sweep. The band edges stay inside the audible range: sweeping a
    // high-pass all the way to 20 kHz (or a low-pass to 20 Hz) filtered the
    // noise away exactly where the knob asks for the most of it, which is why
    // the mode was inaudible at its extremes.
    constexpr float kNoiseFcMin = 120.f;
    constexpr float kNoiseFcMax = 14000.f;
    const float fc = (knob < 0.f)
        ? kNoiseFcMax * std::pow(kNoiseFcMin / kNoiseFcMax, mapped)   // LP: rumble
        : kNoiseFcMin * std::pow(kNoiseFcMax / kNoiseFcMin, mapped);  // HP: hiss

    // Bandwidth compensation: a narrow band passes proportionally less noise
    // power, so make up the level (√ of the bandwidth ratio) to keep the
    // perceived loudness constant across the whole sweep.
    const float nyquist = static_cast<float>(m_sampleRate) * 0.5f;
    const float band = (knob < 0.f) ? fc / nyquist : (nyquist - fc) / nyquist;
    const float comp = std::min(10.0f, 1.0f / std::sqrt(std::clamp(band, 0.01f, 1.0f)));

    // The gain multiplies the *filtered* noise, whose amplitude already shrank
    // by ≈√band — so gain·comp lands at a near-constant loudness across the
    // sweep. Peaks are caught by the soft limiter below, not by a hard cap.
    // Level uses a gentler curve than the cutoff sweep so the noise is already
    // usable at half travel instead of only appearing at the very end.
    const float level = std::pow(absK, 0.7f);
    const float targetGain = level * (0.10f + param * 0.40f) * comp;
    m_scAbsKSmooth.setTargetValue(targetGain);
    if (!m_scAbsKSmooth.isSmoothing() && targetGain < 0.001f) return;

    auto& noiseBuf = m_wetScratch;
    for (int ch = 0; ch < noiseBuf.getNumChannels(); ++ch)
        noiseBuf.clear(ch, 0, n);

    const int numCh = std::min(buffer.getNumChannels(), m_numChannels);
    for (int i = 0; i < n; ++i)
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            // Independent stream per channel — decorrelated noise is wide and
            // sits around the track instead of collapsing into the centre.
            auto& seed = m_scNoiseState.seed[static_cast<size_t>(std::min(ch, 1))];
            seed = seed * 1664525u + 1013904223u;
            noiseBuf.getWritePointer(ch)[i] =
                static_cast<float>(static_cast<int32_t>(seed)) / static_cast<float>(0x7fffffff);
        }
    }

    m_scNoiseState.svf.setTargets(fc, 0.8f);
    m_scNoiseState.svf.process(noiseBuf, 0, n, knob < 0.f);

    // Additive blend: dry is preserved 100%, noise ramps in/out via m_scAbsKSmooth
    const float* srcs[2] = {
        noiseBuf.getReadPointer(0),
        numCh > 1 ? noiseBuf.getReadPointer(1) : noiseBuf.getReadPointer(0)
    };
    float* dsts[2] = {
        buffer.getWritePointer(0) + start,
        numCh > 1 ? buffer.getWritePointer(1) + start : buffer.getWritePointer(0) + start
    };
    for (int i = 0; i < n; ++i) {
        const float ng = m_scAbsKSmooth.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            dsts[ch][i] += fxSoftClip(srcs[ch][i] * ng);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. SC SWEEP — LP/HP sweep controlled by the SC knob position
//    knob < 0 → notch/LP flavor (downward frequency sweep)
//    knob > 0 → HP flavor (upward frequency sweep)
//    param controls resonance: 0 = subtle, 1 = aggressive/whistling
// ─────────────────────────────────────────────────────────────────────────────
void FxProcessor::processSC_Sweep(juce::AudioBuffer<float>& buffer,
                                   int start, int n, float knob, float param)
{
    const float absK   = std::abs(knob);
    const float mapped = scMapFilterTravel(absK);
    // Series filter — fully wet just off the detent (see processSC_Filter).
    const float wet = std::min(1.f, absK * 10.f);
    m_scAbsKSmooth.setTargetValue(wet);
    if (!m_scAbsKSmooth.isSmoothing() && wet < 0.005f) return;

    auto& wetBuf = m_wetScratch;
    copyToWet(buffer, wetBuf, start, n);

    // Resonance stays inside the SVF's stable range; the previous topology
    // self-oscillated into garbage above roughly a quarter of the sample rate,
    // so the upper half of the sweep never produced a usable sound.
    const float q = 0.7f + param * 9.3f;

    constexpr float kSweepLow = 80.f, kSweepHigh = 15000.f;
    const float fc = (knob < 0.f)
        ? kSweepHigh * std::pow(kSweepLow / kSweepHigh, mapped)
        : kSweepLow  * std::pow(kSweepHigh / kSweepLow, mapped);

    const auto mode = (knob < 0.f) ? SVFState::Mode::Notch : SVFState::Mode::HighPass;
    m_scSweepState.svfA.setTargets(fc, 0.707f);
    m_scSweepState.svfB.setTargets(fc, q);
    m_scSweepState.svfA.process(wetBuf, 0, n, mode);
    m_scSweepState.svfB.process(wetBuf, 0, n, mode);

    mixSCSmoothed(buffer, wetBuf, start, n);
}
