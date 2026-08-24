#include "DeckChannelProcessor.h"

#include <algorithm>
#include <cmath>

namespace {
thread_local bool g_inMixerCallback=false;

struct StereoBlock {
    float* L;
    float* R;
    const float* rL;
    const float* rR;
    int n;
    explicit StereoBlock(const juce::AudioSourceChannelInfo& info) noexcept
        : L(nullptr)
        , R(nullptr)
        , rL(nullptr)
        , rR(nullptr)
        , n(info.numSamples)
    {
        const int numCh = std::min(info.buffer->getNumChannels(), 2);
        const int bStart = info.startSample;
        L = numCh > 0 ? info.buffer->getWritePointer(0, bStart) : nullptr;
        R = numCh > 1 ? info.buffer->getWritePointer(1, bStart) : nullptr;
        rL = numCh > 0 ? info.buffer->getReadPointer(0, bStart) : nullptr;
        rR = numCh > 1 ? info.buffer->getReadPointer(1, bStart) : nullptr;
    }
};

} // namespace

DeckChannelProcessor::DeckChannelProcessor(juce::AudioSource* inSource) : source(inSource) {}

void DeckChannelProcessor::setFxEffectType(EffectType type) { m_colorFx.setEffectType(type); }
void DeckChannelProcessor::setFxAmount(float amount) { m_colorFx.setAmount(amount); }
void DeckChannelProcessor::setFxSCKnob(float knob) { m_colorFx.setSCKnobValue(knob); }
void DeckChannelProcessor::setFxSCParam(float param) { m_colorFx.setSCParamValue(param); }
void DeckChannelProcessor::setFxExternalDelayTime(float seconds) { m_colorFx.setExternalDelayTime(seconds); }
void DeckChannelProcessor::setFxPrimaryParam(float v) { m_colorFx.setPrimaryParam(v); }

void DeckChannelProcessor::setFxSlotEffectType(int slot, EffectType type) { if (auto* fx = fxChainSlot(slot)) fx->setEffectType(type); }
void DeckChannelProcessor::setFxSlotAmount(int slot, float amount) { if (auto* fx = fxChainSlot(slot)) fx->setAmount(amount); }
void DeckChannelProcessor::setFxSlotExternalDelayTime(int slot, float seconds) { if (auto* fx = fxChainSlot(slot)) fx->setExternalDelayTime(seconds); }
void DeckChannelProcessor::setFxSlotPrimaryParam(int slot, float v) { if (auto* fx = fxChainSlot(slot)) fx->setPrimaryParam(v); }

void DeckChannelProcessor::setBeatSyncPosition(double beatPosition, double beatDurationSec) {
    m_colorFx.setBeatSyncPosition(beatPosition, beatDurationSec);
    for (auto& fx : m_fxChain)
        fx.setBeatSyncPosition(beatPosition, beatDurationSec);
    m_padFx.setBeatSyncPosition(beatPosition, beatDurationSec);
}

void DeckChannelProcessor::setPadFxEffectType(EffectType type) { m_padFx.setEffectType(type); }
void DeckChannelProcessor::setPadFxAmount(float amount) { m_padFx.setAmount(amount); }

void DeckChannelProcessor::clearPadFx() {
    m_padFx.setEffectType(EffectType::None);
    m_padFx.setAmount(0.0f);
    armClickFreeTransition();
}

void DeckChannelProcessor::setVinylBrakeActive(bool active) { setStopEffectWanted(m_vinylBrakeWanted, active); }
void DeckChannelProcessor::setEchoOutActive(bool active) { setStopEffectWanted(m_echoOutWanted, active); }
void DeckChannelProcessor::setBackspinActive(bool active) { setStopEffectWanted(m_backspinWanted, active); }
void DeckChannelProcessor::setRollOutActive(bool active) { setStopEffectWanted(m_rollOutWanted, active); }
void DeckChannelProcessor::armClickFreeTransition() { m_pendingClickFreeBridge.store(true, std::memory_order_release); }

const juce::AudioBuffer<float>& DeckChannelProcessor::getPflBuffer() const { return m_preFaderScratch; }
const juce::AudioBuffer<float>& DeckChannelProcessor::getPostFaderTailBuffer() const
{
    return m_postFaderTailReturn;
}

void DeckChannelProcessor::prepareToPlay(int samplesPerBlockExpected, double sampleRate) {
        if(g_inMixerCallback)m_prepareRt.fetch_add(1,std::memory_order_relaxed);
        if (source) source->prepareToPlay(samplesPerBlockExpected, sampleRate);

        if(g_inMixerCallback)m_growthRt.fetch_add(1,std::memory_order_relaxed);
        m_preFaderScratch.setSize(2, std::max(64, samplesPerBlockExpected), false, true, true);
        m_postFaderTailReturn.setSize(2, std::max(64, samplesPerBlockExpected), false, true, true);
        m_tailScratch.setSize(2, std::max(64, samplesPerBlockExpected), false, true, true);

        m_colorFx.prepare(sampleRate, samplesPerBlockExpected, 2);
        for (auto& fx : m_fxChain)
            fx.prepare(sampleRate, samplesPerBlockExpected, 2);
        m_padFx.prepare(sampleRate, samplesPerBlockExpected, 2);
        
        m_sampleRate = sampleRate;
        m_filterSampleRate.store(sampleRate,std::memory_order_release);
        m_deviceGeneration.fetch_add(1,std::memory_order_acq_rel);
        m_lastOutputSample[0] = 0.0f;
        m_lastOutputSample[1] = 0.0f;
        m_lastOutputValid = false;
        m_pendingClickFreeBridge.store(false, std::memory_order_release);

        // Initialise per-sample gain smoothers so the first block has no ramp glitch.
        const float sr = static_cast<float>(sampleRate);
        m_trimSmooth .reset(sr, 0.010f);   // 10 ms — trim rarely changes rapidly
        const Parameters parameters = m_parameters.controlSnapshot();
        m_trimSmooth .setCurrentAndTargetValue(parameters.trim);
        m_faderSmooth.reset(sr, 0.020f);   // 20 ms keeps channel-fader moves click-free
        m_faderSmooth.setCurrentAndTargetValue(parameters.fader);


        // 1.15 s ramp from full speed to a complete stop
        m_vinylBrakeRampDown  = 1.0f / (1.15f * static_cast<float>(sampleRate));
        m_vinylBrakeFactor    = 1.0f;
        m_vinylBrakeWritePos  = 0;
        m_vinylBrakeReadPos   = 0.0f;
        m_vinylBrakeNeedSync  = true;
        m_backspinNeedSync    = true;
        m_vinylBrakeBufL.fill(0.0f);
        m_vinylBrakeBufR.fill(0.0f);

        // Echo Out
        m_echoOutDelaySamples = std::min(kEchoOutBuf - 1,
                                         static_cast<int>(sampleRate * 0.375));
        m_echoOutLpCoef = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi
                                           * 4000.0f / static_cast<float>(sampleRate));
        m_echoOutBufL.fill(0.0f);
        m_echoOutBufR.fill(0.0f);
        m_echoOutWritePos  = 0;
        m_echoOutLpStateL  = 0.0f;
        m_echoOutLpStateR  = 0.0f;
        m_echoOutAudioActive = false;

        // Backspin: 2.5× initial speed, decelerates to 0 in ~2.0 s
        m_backspinSpeedRampDown = 2.5f / (2.0f * static_cast<float>(sampleRate));
        m_backspinSpeed         = 0.0f;

        // Roll Out
        m_rollOutRampDown  = 1.0f / (1.5f * static_cast<float>(sampleRate));
        m_rollOutNeedSync  = true;
        m_rollOutGain      = 1.0f;

        publishFilterSnapshot();
        activateFilterSnapshot();
    }

void DeckChannelProcessor::releaseResources() {
        if (source) source->releaseResources();
    }

void DeckChannelProcessor::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) {
        struct Scope{Scope(){g_inMixerCallback=true;}~Scope(){g_inMixerCallback=false;}} scope;
        const Parameters parameters = m_parameters.snapshot();
        m_colorFx.applyPendingCommandAtBlockBoundary();
        for (auto& fx : m_fxChain)
            fx.applyPendingCommandAtBlockBoundary();
        m_padFx.applyPendingCommandAtBlockBoundary();

        if (source) {
            if (bufferToFill.numSamples > 0) {
                source->getNextAudioBlock(bufferToFill);
            } else {
                // Keep this path side-effect free; some JACK states can report
                // zero-sized callbacks transiently. We only log in that case.
            }
        }

        if (m_sampleRate <= 0.0 || bufferToFill.buffer->getNumChannels() == 0 || bufferToFill.numSamples == 0) [[unlikely]]
            return;

        activateFilterSnapshot();

        juce::dsp::AudioBlock<float> block(*bufferToFill.buffer);
        auto fullBlock   = block.getSubBlock(bufferToFill.startSample, bufferToFill.numSamples);
        // EQ/filter chain is prepared for exactly 2 channels; clamp here so the
        // ProcessorDuplicator never accesses an uninitialised filter instance when
        // the device buffer is wider (e.g. user selects output channels 3+4).
        auto slicedBlock = fullBlock.getSubsetChannelBlock(
                               0, std::min(fullBlock.getNumChannels(), static_cast<size_t>(2)));

        // Apply trim pre-EQ/pre-fader with per-sample smoothing.
        m_trimSmooth.setTargetValue(parameters.trim);
        if (m_trimSmooth.isSmoothing() || std::abs(m_trimSmooth.getTargetValue() - 1.0f) > 0.001f) {
            const size_t nc = slicedBlock.getNumChannels();
            const int    ns = static_cast<int>(slicedBlock.getNumSamples());
            for (int i = 0; i < ns; ++i) {
                const float g = m_trimSmooth.getNextValue();
                for (size_t ch = 0; ch < nc; ++ch)
                    slicedBlock.getChannelPointer(ch)[i] *= g;
            }
        }

        if (parameters.polarityInverted) {
            const size_t nc = slicedBlock.getNumChannels();
            const int    ns = static_cast<int>(slicedBlock.getNumSamples());
            for (size_t ch = 0; ch < nc; ++ch) {
                float* w = slicedBlock.getChannelPointer(ch);
                for (int i = 0; i < ns; ++i)
                    w[i] = -w[i];
            }
        }

        // ── Color FX (pre-EQ: timbre-shaping effects that act on raw source color) ─
        if (FxProcessor::isColorFxType(m_colorFx.getEffectType()))
            m_colorFx.process(*bufferToFill.buffer,
                              bufferToFill.startSample,
                              bufferToFill.numSamples);

        processPreparedFilters(bufferToFill);

        // ── Circular buffer: always records; vinyl brake + backspin read from it ──
        {
            const bool wantBrake    = m_vinylBrakeWanted.load(std::memory_order_relaxed);
            const bool wantBackspin = m_backspinWanted.load(std::memory_order_relaxed);

            const StereoBlock block(bufferToFill);

            // Sync read positions on first block of activation.
            if (wantBrake && m_vinylBrakeNeedSync) {
                m_vinylBrakeReadPos  = static_cast<float>((m_vinylBrakeWritePos - 1 + kVinylBrakeBuf) & kVinylBrakeMask);
                m_vinylBrakeFactor   = 1.0f;
                m_vinylBrakeNeedSync = false;
            }
            if (!wantBrake)
                m_vinylBrakeNeedSync = true;

            if (wantBackspin && m_backspinNeedSync) {
                m_backspinReadPos  = static_cast<float>((m_vinylBrakeWritePos - 1 + kVinylBrakeBuf) & kVinylBrakeMask);
                m_backspinSpeed    = 2.5f;  // start at 2.5× backward speed → decelerates to 0
                m_backspinNeedSync = false;
            }
            if (!wantBackspin) {
                m_backspinNeedSync = true;
                m_backspinSpeed    = 0.0f;
            }

            // Pass 1: always record live audio into the circular buffer.
            {
                uint32_t wp = m_vinylBrakeWritePos;
                for (int i = 0; i < block.n; ++i, ++wp) {
                    const int idx = wp & kVinylBrakeMask;
                    m_vinylBrakeBufL[idx] = block.L ? block.L[i] : 0.0f;
                    m_vinylBrakeBufR[idx] = block.R ? block.R[i] : 0.0f;
                }
                m_vinylBrakeWritePos = wp;
            }

            // Pass 2: substitute output — wantBrake/wantBackspin are block-invariant so the
            // branch is hoisted here rather than re-evaluated per sample.
            // Live audio passes through unchanged when neither effect is active.
            if (wantBrake) {
                // Pitch ramps toward zero; read position advances slower than write.
                for (int i = 0; i < block.n; ++i) {
                    m_vinylBrakeFactor = std::max(0.0f, m_vinylBrakeFactor - m_vinylBrakeRampDown);
                    const int rp = static_cast<int>(m_vinylBrakeReadPos) & kVinylBrakeMask;
                    const float tailGain = stopTailGain(m_vinylBrakeFactor, 0.035f);
                    if (block.L) block.L[i] = m_vinylBrakeBufL[rp] * tailGain;
                    if (block.R) block.R[i] = m_vinylBrakeBufR[rp] * tailGain;
                    m_vinylBrakeReadPos += m_vinylBrakeFactor;
                    if (m_vinylBrakeReadPos >= static_cast<float>(kVinylBrakeBuf))
                        m_vinylBrakeReadPos -= static_cast<float>(kVinylBrakeBuf);
                }
            } else if (m_vinylBrakeFactor < 1.0f) {
                // Brake just released: snap read position to live.
                // dataL/dataR already hold live audio from Pass 1 — no buffer read needed.
                m_vinylBrakeFactor  = 1.0f;
                m_vinylBrakeReadPos = static_cast<float>((m_vinylBrakeWritePos - 1) & kVinylBrakeMask);
            } else if (wantBackspin) {
                // Linear-interpolated backward read: high pitch → low pitch → silence.
                for (int i = 0; i < block.n; ++i) {
                    if (m_backspinSpeed > 0.0f) {
                        const int   rp0  = static_cast<int>(m_backspinReadPos) & kVinylBrakeMask;
                        const int   rp1  = (rp0 + 1) & kVinylBrakeMask;
                        const float frac = m_backspinReadPos - std::floor(m_backspinReadPos);
                        const float tailGain = stopTailGain(m_backspinSpeed, 0.10f);
                        if (block.L) {
                            const float sample = m_vinylBrakeBufL[rp0] + frac * (m_vinylBrakeBufL[rp1] - m_vinylBrakeBufL[rp0]);
                            block.L[i] = sample * tailGain;
                        }
                        if (block.R) {
                            const float sample = m_vinylBrakeBufR[rp0] + frac * (m_vinylBrakeBufR[rp1] - m_vinylBrakeBufR[rp0]);
                            block.R[i] = sample * tailGain;
                        }
                        m_backspinReadPos -= m_backspinSpeed;
                        if (m_backspinReadPos < 0.0f)
                            m_backspinReadPos += static_cast<float>(kVinylBrakeBuf);
                        m_backspinSpeed = std::max(0.0f, m_backspinSpeed - m_backspinSpeedRampDown);
                    } else {
                        if (block.L) block.L[i] = 0.0f;
                        if (block.R) block.R[i] = 0.0f;
                    }
                }
            }
        }

        // ── Echo Out (stop effect): records live audio when idle; when active, cuts
        //    live audio and plays the echo tail from the pre-recorded buffer → silence
        {
            const bool wantEchoOut = m_echoOutWanted.load(std::memory_order_relaxed);

            if (!wantEchoOut && m_echoOutAudioActive) {
                // Toggle turned off: reset for next use
                m_echoOutAudioActive = false;
                m_echoOutLpStateL    = 0.0f;
                m_echoOutLpStateR    = 0.0f;
            } else if (wantEchoOut && !m_echoOutAudioActive) {
                m_echoOutAudioActive = true;
            }

            if (!m_echoOutAudioActive) {
                const StereoBlock block(bufferToFill);
                for (int i = 0; i < block.n; ++i) {
                    m_echoOutBufL[m_echoOutWritePos & kEchoOutMask] = block.rL ? block.rL[i] : 0.f;
                    m_echoOutBufR[m_echoOutWritePos & kEchoOutMask] = block.rR ? block.rR[i] : 0.f;
                    ++m_echoOutWritePos;
                }
            } else {
                // Active: cut live audio, play echo tail from pre-recorded buffer → silence
                const StereoBlock block(bufferToFill);
                for (int i = 0; i < block.n; ++i) {
                    const int rp = (m_echoOutWritePos - m_echoOutDelaySamples + kEchoOutBuf) & kEchoOutMask;
                    const float delL = m_echoOutBufL[rp];
                    const float delR = m_echoOutBufR[rp];
                    m_echoOutLpStateL += m_echoOutLpCoef * (delL - m_echoOutLpStateL);
                    m_echoOutLpStateR += m_echoOutLpCoef * (delR - m_echoOutLpStateR);
                    // Feed silence + feedback so the echo repeats and decays
                    m_echoOutBufL[m_echoOutWritePos & kEchoOutMask] = m_echoOutLpStateL * kEchoOutFeedback;
                    m_echoOutBufR[m_echoOutWritePos & kEchoOutMask] = m_echoOutLpStateR * kEchoOutFeedback;
                    ++m_echoOutWritePos;
                    // Output is ONLY the echo tail (live audio is cut → stop effect)
                    if (block.L) block.L[i] = m_echoOutLpStateL;
                    if (block.R) block.R[i] = m_echoOutLpStateR;
                }
            }
        }

        // ── Roll Out (stop effect): loops a captured 250 ms segment and fades to silence
        {
            const bool wantRollOut = m_rollOutWanted.load(std::memory_order_relaxed);

            if (wantRollOut && m_rollOutNeedSync) {
                m_rollOutLoopLen   = std::min(static_cast<int>(m_sampleRate * 0.25),
                                              kVinylBrakeBuf / 4);
                m_rollOutLoopStart = (m_vinylBrakeWritePos - m_rollOutLoopLen + kVinylBrakeBuf * 2)
                                      & kVinylBrakeMask;
                m_rollOutOffset    = 0.0f;
                m_rollOutGain      = 1.0f;
                m_rollOutNeedSync  = false;
            }
            if (!wantRollOut)
                m_rollOutNeedSync = true;

            if (wantRollOut && !m_rollOutNeedSync) {
                const StereoBlock block(bufferToFill);

                for (int i = 0; i < block.n; ++i) {
                    const int rp = (m_rollOutLoopStart + static_cast<int>(m_rollOutOffset))
                                    & kVinylBrakeMask;
                    if (m_rollOutGain > 0.0f) {
                        if (block.L) block.L[i] = m_vinylBrakeBufL[rp] * m_rollOutGain;
                        if (block.R) block.R[i] = m_vinylBrakeBufR[rp] * m_rollOutGain;
                        m_rollOutGain = std::max(0.0f, m_rollOutGain - m_rollOutRampDown);
                    } else {
                        if (block.L) block.L[i] = 0.0f;
                        if (block.R) block.R[i] = 0.0f;
                    }
                    m_rollOutOffset += 1.0f;
                    if (static_cast<int>(m_rollOutOffset) >= m_rollOutLoopLen)
                        m_rollOutOffset -= static_cast<float>(m_rollOutLoopLen);
                }
            }
        }

        // Deck inserts process in the canonical pre-fader chain. Time-based tail
        // effects produce a separate wet return so closing/resetting the deck does
        // not cut their delay/reverb state.
        m_postFaderTailReturn.clear(0, 0, bufferToFill.numSamples);
        m_postFaderTailReturn.clear(1, 0, bufferToFill.numSamples);
        if (!FxProcessor::isColorFxType(m_colorFx.getEffectType())) {
            for (auto& fx : m_fxChain) {
                if (FxProcessor::placementForType(fx.getEffectType())
                    == FxPlacement::PostFaderTail) {
                    fx.processWetReturn(*bufferToFill.buffer, m_tailScratch,
                                        bufferToFill.startSample,
                                        bufferToFill.numSamples);
                    m_postFaderTailReturn.addFrom(0, 0, m_tailScratch, 0, 0,
                                                  bufferToFill.numSamples);
                    m_postFaderTailReturn.addFrom(1, 0, m_tailScratch, 1, 0,
                                                  bufferToFill.numSamples);
                } else {
                    fx.process(*bufferToFill.buffer,
                               bufferToFill.startSample,
                               bufferToFill.numSamples);
                }
            }
        }
        if (FxProcessor::placementForType(m_padFx.getEffectType())
            == FxPlacement::PostFaderTail) {
            m_padFx.processWetReturn(*bufferToFill.buffer, m_tailScratch,
                                     bufferToFill.startSample,
                                     bufferToFill.numSamples);
            m_postFaderTailReturn.addFrom(0, 0, m_tailScratch, 0, 0,
                                          bufferToFill.numSamples);
            m_postFaderTailReturn.addFrom(1, 0, m_tailScratch, 1, 0,
                                          bufferToFill.numSamples);
        } else {
            m_padFx.process(*bufferToFill.buffer,
                            bufferToFill.startSample,
                            bufferToFill.numSamples);
        }

        // Channel pre-fader meter: post Trim/EQ/Filter/insert FX, pre channel fader.
        {
            auto* buf = bufferToFill.buffer;
            const int s = bufferToFill.startSample;
            const int n = bufferToFill.numSamples;

            float peakPreL = 0.0f;
            float peakPreR = 0.0f;
            if (buf->getNumChannels() > 0)
                peakPreL = buf->getMagnitude(0, s, n);
            if (buf->getNumChannels() > 1)
                peakPreR = buf->getMagnitude(1, s, n);

            m_preFaderPeakL.store(peakPreL, std::memory_order_relaxed);
            m_preFaderPeakR.store(peakPreR, std::memory_order_relaxed);

            // Capture for PFL routing: headphones always hear this pre-fader signal
            // so cue works even when the channel fader or crossfader is closed.
            if (m_preFaderScratch.getNumSamples() >= n) {
                m_preFaderScratch.copyFrom(0, 0, *buf, 0, s, n);
                m_preFaderScratch.copyFrom(1, 0, *buf, std::min(1, buf->getNumChannels() - 1), s, n);
            }
        }

        // Apply the channel fader with per-sample smoothing. Crossfader gain is master-owned.
        // The crossfader fires applyVolumes() on every pixel of slider movement, so
        // The channel fader can change every block. Smoothing eliminates the
        // step-change pops that occur at block boundaries.
        m_faderSmooth.setTargetValue(parameters.fader);
        if (m_faderSmooth.isSmoothing() || std::abs(m_faderSmooth.getTargetValue() - 1.0f) > 0.001f) {
            const size_t nc = slicedBlock.getNumChannels();
            const int    ns = static_cast<int>(slicedBlock.getNumSamples());
            for (int i = 0; i < ns; ++i) {
                const float g = m_faderSmooth.getNextValue();
                for (size_t ch = 0; ch < nc; ++ch)
                    slicedBlock.getChannelPointer(ch)[i] *= g;
            }
        }

        applyClickFreeTransition(bufferToFill);

        // ── Post-fader per-deck VU (channels 0+1 after Beat FX) ─────────────
        {
            auto* buf = bufferToFill.buffer;
            const int s = bufferToFill.startSample;
            const int n = bufferToFill.numSamples;
            if (buf->getNumChannels() > 0)
                m_peakL.store(buf->getMagnitude(0, s, n), std::memory_order_relaxed);
            if (buf->getNumChannels() > 1)
                m_peakR.store(buf->getMagnitude(1, s, n), std::memory_order_relaxed);
        }
        // Master volume, limiter, and output routing are handled by AudioEngine.
    }

void DeckChannelProcessor::setTrim(float value)
{
    m_parameters.update([value](Parameters& p) { p.trim = std::clamp(value, 0.0f, 4.0f); });
}

void DeckChannelProcessor::setFader(float value)
{
    m_parameters.update([value](Parameters& p) { p.fader = std::clamp(value, 0.0f, 1.0f); });
}

void DeckChannelProcessor::setPolarityInverted(bool inverted)
{
    m_parameters.update([inverted](Parameters& p) { p.polarityInverted = inverted; });
}

void DeckChannelProcessor::setStopEffectWanted(std::atomic<bool>& flag, bool active)
{
    const bool previous = flag.exchange(active, std::memory_order_relaxed);
    if (previous != active)
        armClickFreeTransition();
}

void DeckChannelProcessor::applyClickFreeTransition(const juce::AudioSourceChannelInfo& bufferToFill)
{
    auto* buf = bufferToFill.buffer;
    const int start = bufferToFill.startSample;
    const int n = bufferToFill.numSamples;
    const int numCh = std::min(buf->getNumChannels(), 2);
    if (numCh <= 0 || n <= 0)
        return;

    const bool bridge = m_pendingClickFreeBridge.exchange(false, std::memory_order_acq_rel)
                        && m_lastOutputValid;
    if (bridge) {
        const int fadeLen = std::min(n, clickFreeBridgeSamples());
        for (int ch = 0; ch < numCh; ++ch) {
            float* w = buf->getWritePointer(ch, start);
            const float from = m_lastOutputSample[ch];
            for (int i = 0; i < fadeLen; ++i) {
                const float t = static_cast<float>(i + 1) / static_cast<float>(fadeLen);
                w[i] = from + (w[i] - from) * t;
            }
        }
    }

    for (int ch = 0; ch < numCh; ++ch)
        m_lastOutputSample[ch] = buf->getSample(ch, start + n - 1);
    if (numCh == 1)
        m_lastOutputSample[1] = m_lastOutputSample[0];
    m_lastOutputValid = true;
}

int DeckChannelProcessor::clickFreeBridgeSamples() const
{
    const int samples = m_sampleRate > 0.0
        ? static_cast<int>(std::round(m_sampleRate * 0.0025))
        : 128;
    return std::clamp(samples, 64, 192);
}

float DeckChannelProcessor::stopTailGain(float value, float fadeStart)
{
    const float t = std::clamp(value / fadeStart, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void DeckChannelProcessor::setEq(float l, float m, float h) {
        m_parameters.update([l, m, h](Parameters& p) {
            p.eqLow = std::clamp(l, -1.0f, 1.0f);
            p.eqMid = std::clamp(m, -1.0f, 1.0f);
            p.eqHigh = std::clamp(h, -1.0f, 1.0f);
        });
        publishFilterSnapshot();
    }

void DeckChannelProcessor::setFilterVal(float f) {
        m_parameters.update([f](Parameters& p) {
            p.filter = std::clamp(f, -1.0f, 1.0f);
        });
        publishFilterSnapshot();
    }

float DeckChannelProcessor::getDecibelsFromKnob(float kb) const {
        // Mirrors mixerEqGainFromKnob(): +6 dB boost and -26 dB cut.
        const float gain = static_cast<float>(mixerEqGainFromKnob(kb));
        return 20.0f * std::log10(gain);
    }

void DeckChannelProcessor::publishFilterSnapshot() noexcept
{
    if(g_inMixerCallback)m_coeffBuildRt.fetch_add(1,std::memory_order_relaxed);
    const auto generation=m_parameterGeneration.fetch_add(1,std::memory_order_acq_rel)+1;
    const Parameters parameters = m_parameters.controlSnapshot();
    const MixerFilterTargets targets {
        parameters.eqLow, parameters.eqMid, parameters.eqHigh, parameters.filter
    };
    const auto snapshot=buildMixerCoefficientSnapshot(targets,m_filterSampleRate.load(std::memory_order_acquire),generation,m_deviceGeneration.load(std::memory_order_acquire));
    if(!snapshot.valid()){m_invalidSets.fetch_add(1,std::memory_order_relaxed);return;}
    for(auto& slot:m_coefficientSlots){
        auto expected=SnapshotState::Empty;
        if(slot.state.compare_exchange_strong(expected,SnapshotState::Writing,std::memory_order_acq_rel)){
            slot.snapshot=snapshot;slot.state.store(SnapshotState::Ready,std::memory_order_release);return;
        }
    }
    for(auto& slot:m_coefficientSlots){
        auto expected=SnapshotState::Ready;
        if(slot.state.compare_exchange_strong(expected,SnapshotState::Writing,std::memory_order_acq_rel)){
            slot.snapshot=snapshot;slot.state.store(SnapshotState::Ready,std::memory_order_release);m_staleSnapshots.fetch_add(1,std::memory_order_relaxed);return;
        }
    }
    m_staleSnapshots.fetch_add(1,std::memory_order_relaxed);
}

void DeckChannelProcessor::activateFilterSnapshot() noexcept
{
    for(auto& slot:m_coefficientSlots){
        auto expected=SnapshotState::Ready;
        if(!slot.state.compare_exchange_strong(expected,SnapshotState::Writing,std::memory_order_acq_rel))continue;
        const auto snapshot=slot.snapshot;
        if(snapshot.deviceGeneration!=m_deviceGeneration.load(std::memory_order_acquire)||snapshot.parameterGeneration<m_parameterGeneration.load(std::memory_order_acquire)){
            slot.state.store(SnapshotState::Empty,std::memory_order_release);m_staleSnapshots.fetch_add(1,std::memory_order_relaxed);continue;
        }
        const int next=1-m_activeFilterBank;m_filterBanks[next].setSnapshot(snapshot);m_filterBanks[next].clearState();
        m_activeFilterBank=next;m_filterFadeRemaining=kFilterFadeSamples;m_snapshotSwitches.fetch_add(1,std::memory_order_relaxed);slot.state.store(SnapshotState::Empty,std::memory_order_release);break;
    }
}

void DeckChannelProcessor::processPreparedFilters(const juce::AudioSourceChannelInfo& info) noexcept
{
    const int channels=std::min(2,info.buffer->getNumChannels());
    for(int ch=0;ch<channels;++ch){float*w=info.buffer->getWritePointer(ch,info.startSample);for(int i=0;i<info.numSamples;++i){const float input=w[i];const float current=m_filterBanks[m_activeFilterBank].process(ch,input);if(i<m_filterFadeRemaining){const int old=1-m_activeFilterBank;const float previous=m_filterBanks[old].process(ch,input);const float t=static_cast<float>(kFilterFadeSamples-m_filterFadeRemaining+i+1)/kFilterFadeSamples;w[i]=previous+(current-previous)*t;}else w[i]=current;}}
    m_filterFadeRemaining=std::max(0,m_filterFadeRemaining-info.numSamples);
}

DeckChannelProcessor::RealtimeStats DeckChannelProcessor::realtimeStats() const noexcept{return{m_coeffBuildRt.load(),m_prepareRt.load(),m_growthRt.load(),m_lockRt.load(),m_constructRt.load(),m_snapshotSwitches.load(),m_staleSnapshots.load(),m_invalidSets.load()};}

FxProcessor* DeckChannelProcessor::fxChainSlot(int slot) {
        if (slot < 1 || slot > kFxChainSlots) return nullptr;
        return &m_fxChain[static_cast<size_t>(slot - 1)];
    }
