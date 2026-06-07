#include "MixerDspSource.h"

#include <algorithm>
#include <cmath>

MixerDspSource::MixerDspSource(juce::AudioSource* inSource) : source(inSource) {}

void MixerDspSource::setFxEffectType(EffectType type) { m_colorFx.setEffectType(type); }
void MixerDspSource::setFxAmount(float amount) { m_colorFx.setAmount(amount); }
void MixerDspSource::setFxSCKnob(float knob) { m_colorFx.setSCKnobValue(knob); }
void MixerDspSource::setFxSCParam(float param) { m_colorFx.setSCParamValue(param); }
void MixerDspSource::setFxExternalDelayTime(float seconds) { m_colorFx.setExternalDelayTime(seconds); }
void MixerDspSource::setFxPrimaryParam(float v) { m_colorFx.setPrimaryParam(v); }

void MixerDspSource::setFxSlotEffectType(int slot, EffectType type) {
        if (auto* fx = fxChainSlot(slot)) fx->setEffectType(type);
    }

void MixerDspSource::setFxSlotAmount(int slot, float amount) {
        if (auto* fx = fxChainSlot(slot)) fx->setAmount(amount);
    }

void MixerDspSource::setFxSlotExternalDelayTime(int slot, float seconds) {
        if (auto* fx = fxChainSlot(slot)) fx->setExternalDelayTime(seconds);
    }

void MixerDspSource::setFxSlotPrimaryParam(int slot, float v) {
        if (auto* fx = fxChainSlot(slot)) fx->setPrimaryParam(v);
    }

void MixerDspSource::setBeatSyncPosition(double beatPosition, double beatDurationSec) {
        m_colorFx.setBeatSyncPosition(beatPosition, beatDurationSec);
        for (auto& fx : m_fxChain)
            fx.setBeatSyncPosition(beatPosition, beatDurationSec);
        m_padFx.setBeatSyncPosition(beatPosition, beatDurationSec);
    }

void MixerDspSource::setPadFxEffectType(EffectType type) { m_padFx.setEffectType(type); }
void MixerDspSource::setPadFxAmount(float amount) { m_padFx.setAmount(amount); }

void MixerDspSource::clearPadFx() {
        m_padFx.setEffectType(EffectType::None);
        m_padFx.setAmount(0.0f);
        armClickFreeTransition();
    }

void MixerDspSource::setVinylBrakeActive(bool active) { setStopEffectWanted(m_vinylBrakeWanted, active); }
void MixerDspSource::setEchoOutActive(bool active) { setStopEffectWanted(m_echoOutWanted, active); }
void MixerDspSource::setBackspinActive(bool active) { setStopEffectWanted(m_backspinWanted, active); }
void MixerDspSource::setRollOutActive(bool active) { setStopEffectWanted(m_rollOutWanted, active); }
void MixerDspSource::setScratchTimbre(float amount) {
    scratchTimbre.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
}
void MixerDspSource::armClickFreeTransition() {
    m_pendingClickFreeBridge.store(true, std::memory_order_release);
}

const juce::AudioBuffer<float>& MixerDspSource::getPflBuffer() const { return m_preFaderScratch; }

void MixerDspSource::prepareToPlay(int samplesPerBlockExpected, double sampleRate) {
        if (source) source->prepareToPlay(samplesPerBlockExpected, sampleRate);

        m_preFaderScratch.setSize(2, std::max(64, samplesPerBlockExpected), false, true, true);

        m_colorFx.prepare(sampleRate, samplesPerBlockExpected, 2);
        for (auto& fx : m_fxChain)
            fx.prepare(sampleRate, samplesPerBlockExpected, 2);
        m_padFx.prepare(sampleRate, samplesPerBlockExpected, 2);
        
        juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(samplesPerBlockExpected), 2 };
        lowEq.prepare(spec);
        midEq.prepare(spec);
        highEq.prepare(spec);
        colorFilter.prepare(spec);

        m_sampleRate = sampleRate;
        for (int i = 0; i < 8; ++i) m_scratchWarmLpState[i] = 0.0f;
        m_lastOutputSample[0] = 0.0f;
        m_lastOutputSample[1] = 0.0f;
        m_lastOutputValid = false;
        m_pendingClickFreeBridge.store(false, std::memory_order_release);

        // Initialise per-sample gain smoothers so the first block has no ramp glitch.
        const float sr = static_cast<float>(sampleRate);
        m_trimSmooth .reset(sr, 0.010f);   // 10 ms — trim rarely changes rapidly
        m_trimSmooth .setCurrentAndTargetValue(trimVal .load(std::memory_order_relaxed));
        m_faderSmooth.reset(sr, 0.020f);   // 20 ms — crossfader can move very fast
        m_faderSmooth.setCurrentAndTargetValue(faderVal.load(std::memory_order_relaxed));

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

        updateFilters();
    }

void MixerDspSource::releaseResources() {
        if (source) source->releaseResources();
    }

void MixerDspSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) {
        if (source) {
            if (bufferToFill.numSamples > 0) {
                source->getNextAudioBlock(bufferToFill);
            } else {
                // Keep this path side-effect free; some JACK states can report
                // zero-sized callbacks transiently. We only log in that case.
            }
        }

        if (m_sampleRate <= 0.0 || bufferToFill.buffer->getNumChannels() == 0 || bufferToFill.numSamples == 0) return;

        // Apply deferred EQ / filter coefficient updates on the audio thread.
        // This prevents the data race of writing IIR coefficients from the UI
        // thread while the audio thread reads them inside lowEq.process().
        if (m_filtersDirty.exchange(false, std::memory_order_acquire))
            updateFilters();

        // Rate-proportional 4-pole anti-alias LP + gentle soft-clip during scratch.
        // scratchTimbre carries absRate (0–1); cutoff at rate × sr × 0.25 places
        // the first linear-interp alias image at 4× cutoff → ~48 dB rejection.
        const float scratchRate = scratchTimbre.load(std::memory_order_relaxed);
        const bool  scratchLpActive = (scratchRate > 0.00015f && scratchRate < 0.99f);
        if (scratchLpActive) {
            const int numChannels = std::min(bufferToFill.buffer->getNumChannels(), 2);

            // On activation, seed all pole states from the first input sample so
            // the filter output starts at the correct level with no convergence noise.
            if (!m_scratchLpWasActive) {
                for (int ch = 0; ch < numChannels; ++ch) {
                    const float seed = bufferToFill.buffer->getSample(ch, bufferToFill.startSample);
                    m_scratchWarmLpState[ch]     = seed;
                    m_scratchWarmLpState[ch + 2] = seed;
                    m_scratchWarmLpState[ch + 4] = seed;
                    m_scratchWarmLpState[ch + 6] = seed;
                }
            }

            // Keep slow micro-scratches bright and audible — a fixed minimum cutoff
            // prevents the "underwater digital" tone at low platter speeds.
            const float rateHz = scratchRate * static_cast<float>(m_sampleRate) * 0.32f;
            const float cutoffHz = std::max(2600.0f, rateHz);
            const float pole = std::exp(-2.0f * juce::MathConstants<float>::pi * cutoffHz
                                        / static_cast<float>(m_sampleRate));
            const float alpha = 1.0f - std::clamp(pole, 0.0f, 0.9999f);
            // Very gentle saturation only at crawl speeds; preserve linearity for nuance.
            const float satK = std::max(0.0f, (0.22f - scratchRate) / 0.22f) * 0.14f;
            const float gainBoost = 1.0f + std::max(0.0f, (0.50f - std::min(scratchRate, 0.50f)) * 1.35f);

            const int ns = bufferToFill.numSamples;
            for (int ch = 0; ch < numChannels; ++ch) {
                float s1 = m_scratchWarmLpState[ch];
                float s2 = m_scratchWarmLpState[ch + 2];
                float s3 = m_scratchWarmLpState[ch + 4];
                float s4 = m_scratchWarmLpState[ch + 6];
                float* w = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                // satK is constant within the block; hoist the branch outside the sample loop.
                if (satK > 0.001f) {
                    for (int i = 0; i < ns; ++i) {
                        s1 += alpha * (w[i] - s1);
                        s2 += alpha * (s1   - s2);
                        s3 += alpha * (s2   - s3);
                        s4 += alpha * (s3   - s4);
                        w[i] = (s4 / (1.0f + std::abs(s4) * satK)) * gainBoost;
                    }
                } else {
                    for (int i = 0; i < ns; ++i) {
                        s1 += alpha * (w[i] - s1);
                        s2 += alpha * (s1   - s2);
                        s3 += alpha * (s2   - s3);
                        s4 += alpha * (s3   - s4);
                        w[i] = s4 * gainBoost;
                    }
                }
                m_scratchWarmLpState[ch]     = s1;
                m_scratchWarmLpState[ch + 2] = s2;
                m_scratchWarmLpState[ch + 4] = s3;
                m_scratchWarmLpState[ch + 6] = s4;
            }
        }
        m_scratchLpWasActive = scratchLpActive;

        juce::dsp::AudioBlock<float> block(*bufferToFill.buffer);
        auto fullBlock   = block.getSubBlock(bufferToFill.startSample, bufferToFill.numSamples);
        // EQ/filter chain is prepared for exactly 2 channels; clamp here so the
        // ProcessorDuplicator never accesses an uninitialised filter instance when
        // the device buffer is wider (e.g. user selects output channels 3+4).
        auto slicedBlock = fullBlock.getSubsetChannelBlock(
                               0, std::min(fullBlock.getNumChannels(), static_cast<size_t>(2)));
        juce::dsp::ProcessContextReplacing<float> context(slicedBlock);

        // Apply trim pre-EQ/pre-fader with per-sample smoothing.
        // Reading the atomic each block and setting a target lets the SmoothedValue
        // ramp across the block — no step changes, no clicks.
        m_trimSmooth.setTargetValue(trimVal.load(std::memory_order_relaxed));
        if (m_trimSmooth.isSmoothing() || std::abs(m_trimSmooth.getTargetValue() - 1.0f) > 0.001f) {
            const size_t nc = slicedBlock.getNumChannels();
            const int    ns = static_cast<int>(slicedBlock.getNumSamples());
            for (int i = 0; i < ns; ++i) {
                const float g = m_trimSmooth.getNextValue();
                for (size_t ch = 0; ch < nc; ++ch)
                    slicedBlock.getChannelPointer(ch)[i] *= g;
            }
        }

        // ── Color FX (pre-EQ: timbre-shaping effects that act on raw source color) ─
        if (FxProcessor::isColorFxType(m_colorFx.getEffectType()))
            m_colorFx.process(*bufferToFill.buffer,
                              bufferToFill.startSample,
                              bufferToFill.numSamples);

        lowEq.process(context);
        midEq.process(context);
        highEq.process(context);
        colorFilter.process(context);

        // ── Circular buffer: always records; vinyl brake + backspin read from it ──
        {
            const bool wantBrake    = m_vinylBrakeWanted.load(std::memory_order_relaxed);
            const bool wantBackspin = m_backspinWanted.load(std::memory_order_relaxed);

            const int numCh = std::min(bufferToFill.buffer->getNumChannels(), 2);
            const int bStart = bufferToFill.startSample;
            const int bN     = bufferToFill.numSamples;
            float* dataL = numCh > 0 ? bufferToFill.buffer->getWritePointer(0, bStart) : nullptr;
            float* dataR = numCh > 1 ? bufferToFill.buffer->getWritePointer(1, bStart) : nullptr;

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
                for (int i = 0; i < bN; ++i, ++wp) {
                    const int idx = wp & kVinylBrakeMask;
                    m_vinylBrakeBufL[idx] = dataL ? dataL[i] : 0.0f;
                    m_vinylBrakeBufR[idx] = dataR ? dataR[i] : 0.0f;
                }
                m_vinylBrakeWritePos = wp;
            }

            // Pass 2: substitute output — wantBrake/wantBackspin are block-invariant so the
            // branch is hoisted here rather than re-evaluated per sample.
            // Live audio passes through unchanged when neither effect is active.
            if (wantBrake) {
                // Pitch ramps toward zero; read position advances slower than write.
                for (int i = 0; i < bN; ++i) {
                    m_vinylBrakeFactor = std::max(0.0f, m_vinylBrakeFactor - m_vinylBrakeRampDown);
                    const int rp = static_cast<int>(m_vinylBrakeReadPos) & kVinylBrakeMask;
                    const float tailGain = stopTailGain(m_vinylBrakeFactor, 0.035f);
                    if (dataL) dataL[i] = m_vinylBrakeBufL[rp] * tailGain;
                    if (dataR) dataR[i] = m_vinylBrakeBufR[rp] * tailGain;
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
                for (int i = 0; i < bN; ++i) {
                    if (m_backspinSpeed > 0.0f) {
                        const int   rp0  = static_cast<int>(m_backspinReadPos) & kVinylBrakeMask;
                        const int   rp1  = (rp0 + 1) & kVinylBrakeMask;
                        const float frac = m_backspinReadPos - std::floor(m_backspinReadPos);
                        const float tailGain = stopTailGain(m_backspinSpeed, 0.10f);
                        if (dataL) {
                            const float sample = m_vinylBrakeBufL[rp0] + frac * (m_vinylBrakeBufL[rp1] - m_vinylBrakeBufL[rp0]);
                            dataL[i] = sample * tailGain;
                        }
                        if (dataR) {
                            const float sample = m_vinylBrakeBufR[rp0] + frac * (m_vinylBrakeBufR[rp1] - m_vinylBrakeBufR[rp0]);
                            dataR[i] = sample * tailGain;
                        }
                        m_backspinReadPos -= m_backspinSpeed;
                        if (m_backspinReadPos < 0.0f)
                            m_backspinReadPos += static_cast<float>(kVinylBrakeBuf);
                        m_backspinSpeed = std::max(0.0f, m_backspinSpeed - m_backspinSpeedRampDown);
                    } else {
                        if (dataL) dataL[i] = 0.0f;
                        if (dataR) dataR[i] = 0.0f;
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

            const int numCh = std::min(bufferToFill.buffer->getNumChannels(), 2);
            const int bStart = bufferToFill.startSample;
            const int bN     = bufferToFill.numSamples;

            if (!m_echoOutAudioActive) {
                // Idle: continuously record live audio into the delay buffer (no feedback)
                const float* srcL = numCh > 0 ? bufferToFill.buffer->getReadPointer(0, bStart) : nullptr;
                const float* srcR = numCh > 1 ? bufferToFill.buffer->getReadPointer(1, bStart) : nullptr;
                for (int i = 0; i < bN; ++i) {
                    m_echoOutBufL[m_echoOutWritePos & kEchoOutMask] = srcL ? srcL[i] : 0.f;
                    m_echoOutBufR[m_echoOutWritePos & kEchoOutMask] = srcR ? srcR[i] : 0.f;
                    ++m_echoOutWritePos;
                }
            } else {
                // Active: cut live audio, play echo tail from pre-recorded buffer → silence
                float* dataL = numCh > 0 ? bufferToFill.buffer->getWritePointer(0, bStart) : nullptr;
                float* dataR = numCh > 1 ? bufferToFill.buffer->getWritePointer(1, bStart) : nullptr;
                for (int i = 0; i < bN; ++i) {
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
                    if (dataL) dataL[i] = m_echoOutLpStateL;
                    if (dataR) dataR[i] = m_echoOutLpStateR;
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
                const int numCh = std::min(bufferToFill.buffer->getNumChannels(), 2);
                const int bStart = bufferToFill.startSample;
                const int bN     = bufferToFill.numSamples;
                float* dataL = numCh > 0 ? bufferToFill.buffer->getWritePointer(0, bStart) : nullptr;
                float* dataR = numCh > 1 ? bufferToFill.buffer->getWritePointer(1, bStart) : nullptr;

                for (int i = 0; i < bN; ++i) {
                    const int rp = (m_rollOutLoopStart + static_cast<int>(m_rollOutOffset))
                                    & kVinylBrakeMask;
                    if (m_rollOutGain > 0.0f) {
                        if (dataL) dataL[i] = m_vinylBrakeBufL[rp] * m_rollOutGain;
                        if (dataR) dataR[i] = m_vinylBrakeBufR[rp] * m_rollOutGain;
                        m_rollOutGain = std::max(0.0f, m_rollOutGain - m_rollOutRampDown);
                    } else {
                        if (dataL) dataL[i] = 0.0f;
                        if (dataR) dataR[i] = 0.0f;
                    }
                    m_rollOutOffset += 1.0f;
                    if (static_cast<int>(m_rollOutOffset) >= m_rollOutLoopLen)
                        m_rollOutOffset -= static_cast<float>(m_rollOutLoopLen);
                }
            }
        }

        // Channel pre-fader meter: post Trim/EQ/Filter/FX, pre channel fader/crossfader.
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

        // Apply channel volume (crossfader + channel fader) with per-sample smoothing.
        // The crossfader fires applyVolumes() on every pixel of slider movement, so
        // faderVal can change every block.  Smoothing over 20 ms eliminates the
        // step-change pops that occur at block boundaries.
        m_faderSmooth.setTargetValue(faderVal.load(std::memory_order_relaxed));
        if (m_faderSmooth.isSmoothing() || std::abs(m_faderSmooth.getTargetValue() - 1.0f) > 0.001f) {
            const size_t nc = slicedBlock.getNumChannels();
            const int    ns = static_cast<int>(slicedBlock.getNumSamples());
            for (int i = 0; i < ns; ++i) {
                const float g = m_faderSmooth.getNextValue();
                for (size_t ch = 0; ch < nc; ++ch)
                    slicedBlock.getChannelPointer(ch)[i] *= g;
            }
        }

        // ── Beat FX chain + PAD FX (post-fader: tails continue after fader closes) ──
        if (!FxProcessor::isColorFxType(m_colorFx.getEffectType())) {
            for (auto& fx : m_fxChain)
                fx.process(*bufferToFill.buffer,
                           bufferToFill.startSample,
                           bufferToFill.numSamples);
        }
        m_padFx.process(*bufferToFill.buffer,
                        bufferToFill.startSample,
                        bufferToFill.numSamples);

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
        // Master volume, limiter, and output routing are handled by DjMasterBus.
    }

void MixerDspSource::setTrim(float val) { trimVal = val; }
void MixerDspSource::setFader(float val) { faderVal = val; }

void MixerDspSource::setStopEffectWanted(std::atomic<bool>& flag, bool active)
{
    const bool previous = flag.exchange(active, std::memory_order_relaxed);
    if (previous != active)
        armClickFreeTransition();
}

void MixerDspSource::applyClickFreeTransition(const juce::AudioSourceChannelInfo& bufferToFill)
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

int MixerDspSource::clickFreeBridgeSamples() const
{
    const int samples = m_sampleRate > 0.0
        ? static_cast<int>(std::round(m_sampleRate * 0.0025))
        : 128;
    return std::clamp(samples, 64, 192);
}

float MixerDspSource::stopTailGain(float value, float fadeStart)
{
    const float t = std::clamp(value / fadeStart, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void MixerDspSource::setEq(float l, float m, float h) {
        lowVol = l;
        midVol = m;
        highVol = h;
        // Signal the audio thread to recompute coefficients.  Writing the
        // atomics first + release on the flag ensures the audio thread sees
        // all three values before it reads m_filtersDirty = true.
        m_filtersDirty.store(true, std::memory_order_release);
    }

void MixerDspSource::setFilterVal(float f) {
        filterVal = f;
        m_filtersDirty.store(true, std::memory_order_release);
    }

float MixerDspSource::getDecibelsFromKnob(float kb) const {
        if (kb < 0.0f) {
            return kb * 32.0f; // -1 -> -32 dB (approx -inf / kill)
        } else {
            return kb * 6.0f;  // +1 -> +6 dB
        }
    }

void MixerDspSource::updateFilters() {
        if (m_sampleRate <= 0) return;
        
        // Update EQs using standard DJ shelving/peak frequencies
        *lowEq.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(m_sampleRate, 250.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(lowVol)));
        *midEq.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(m_sampleRate, 1000.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(midVol)));
        *highEq.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(m_sampleRate, 2500.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(highVol)));

        // Color Filter (LPF/HPF combo)
        if (std::abs(filterVal) < 0.05f) {
            // Flat response / completely bypassed
            // Since we can't 'bypass' perfectly, we just set a flat peak filter
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(m_sampleRate, 1000.0f, 0.707f, 1.0f);
        } else if (filterVal < 0.0f) {
            // Low Pass: sweep down from 20000 to ~80 Hz
            // t goes from 1.0 down to 0
            float t = 1.0f + filterVal;
            float freq = 80.0f * std::pow(20000.0f / 80.0f, t);
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(m_sampleRate, std::max(20.0f, freq), 1.2f);
        } else {
            // High Pass: sweep up from 20 to ~10000 Hz
            float freq = 20.0f * std::pow(10000.0f / 20.0f, filterVal);
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(m_sampleRate, std::max(20.0f, freq), 1.2f);
        }
    }

FxProcessor* MixerDspSource::fxChainSlot(int slot) {
        if (slot < 1 || slot > kFxChainSlots) return nullptr;
        return &m_fxChain[static_cast<size_t>(slot - 1)];
    }
