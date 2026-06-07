#include "TimeStretchAudioSource.h"

#include <algorithm>
#include <cmath>

TimeStretchAudioSource::TimeStretchAudioSource(juce::AudioSource* inSource) : source(inSource) {}

void TimeStretchAudioSource::setTempoRatio(double ratio) {
        const double clamped = std::clamp(ratio, 0.01, 8.0);
        const double current = m_targetTempoRatio.load(std::memory_order_relaxed);
        if (std::abs(current - clamped) < kTempoUpdateEpsilon)
            return;

        m_targetTempoRatio.store(clamped, std::memory_order_release);
    }

void TimeStretchAudioSource::setPitchLockEnabled(bool enabled) {
        if (m_pitchLockEnabled.load(std::memory_order_relaxed) == enabled)
            return;

        m_pitchLockEnabled.store(enabled, std::memory_order_relaxed);
        m_resetPipelineRequested.store(true, std::memory_order_release);
    }

void TimeStretchAudioSource::setScratchBypass(bool enabled) {
    m_scratchBypass.store(enabled, std::memory_order_relaxed);
}

void TimeStretchAudioSource::enterScratchBypass() noexcept {
    m_scratchBypass.store(true, std::memory_order_relaxed);
    m_switchFadeRemaining.store(0, std::memory_order_relaxed);
    m_reportedLatencySamples.store(0, std::memory_order_relaxed);
    m_prefillTargetSamples = 0;
    if (fifo)
        fifo->reset();
}

void TimeStretchAudioSource::endScratchBypass() noexcept
{
    m_scratchBypass.store(false, std::memory_order_relaxed);
    m_appliedTempoRatio = m_targetTempoRatio.load(std::memory_order_relaxed);
    updateStretcherRatios();

    // Re-entering the keylock/RubberBand path after varispeed scratch needs a
    // gentle fade — an instant switch causes post-scratch clicks.
    if (m_pitchLockEnabled.load(std::memory_order_relaxed))
        m_resetPipelineRequested.store(true, std::memory_order_release);
}

void TimeStretchAudioSource::prepareToPlay(int samplesPerBlockExpected, double sr) {
        sampleRate = sr;
        if (source) source->prepareToPlay(samplesPerBlockExpected, sr);
        stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
            sr, 2,
            RubberBand::RubberBandStretcher::OptionProcessRealTime |
            RubberBand::RubberBandStretcher::OptionWindowShort |
            RubberBand::RubberBandStretcher::OptionPitchHighSpeed);

        m_maxProcessSize = std::clamp(samplesPerBlockExpected > 0 ? samplesPerBlockExpected * 2 : 512,
                                      kMinPullSize,
                                      4096);
        stretcher->setMaxProcessSize(static_cast<size_t>(m_maxProcessSize));

        const int baseBlockSize = samplesPerBlockExpected > 0
            ? samplesPerBlockExpected
            : kDefaultPrefillCapSamples;
        m_prefillHardCapSamples = std::clamp(baseBlockSize * kPrefillMaxBlocks,
                                             kMinPullSize,
                                             kMaxPrefillSamples);

        m_appliedTempoRatio = m_targetTempoRatio.load(std::memory_order_relaxed);
        updateStretcherRatios();

        const int scratchCapacity = std::max(kMaxPullSize, m_maxProcessSize);
        scratchBuffer.setSize(2, scratchCapacity);
        outputBuffer.setSize(2, kFifoCapacity);
        trimBuffer.setSize(2, kMaxPullSize);
        prewarmZeroBuffer.setSize(2, std::max(kMaxPullSize, m_maxProcessSize));
        prewarmZeroBuffer.clear();
        fifo = std::make_unique<juce::AbstractFifo>(kFifoCapacity);

        // Pre-warm upfront: startup delay is absorbed here, never trimmed
        // on-the-fly during audio callbacks.
        prewarmStretcher();
    }

void TimeStretchAudioSource::releaseResources() {
        if (source) source->releaseResources();
        stretcher.reset();
        fifo.reset();
        m_prefillTargetSamples = 0;
        m_reportedLatencySamples.store(0, std::memory_order_relaxed);
        m_startPadRemaining.store(0, std::memory_order_relaxed);
        m_startDelayTrimRemaining.store(0, std::memory_order_relaxed);
        m_switchFadeRemaining.store(0, std::memory_order_relaxed);
    }

void TimeStretchAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) {
        if (!source) [[unlikely]] {
            info.clearActiveBufferRegion();
            return;
        }

        applyPendingRatioChange();

        if (m_scratchBypass.load(std::memory_order_relaxed)
                || !m_pitchLockEnabled.load(std::memory_order_relaxed)) {
            if (m_resetPipelineRequested.exchange(false, std::memory_order_acquire))
                resetRealtimePipeline(false);
            m_reportedLatencySamples.store(0, std::memory_order_relaxed);
            source->getNextAudioBlock(info);
            applySwitchFade(info);
            return;
        }

        if (!stretcher || !fifo) [[unlikely]] {
            info.clearActiveBufferRegion();
            return;
        }

        if (m_resetPipelineRequested.exchange(false, std::memory_order_acquire))
            resetRealtimePipeline(true);

        const int numCh = std::min(info.buffer->getNumChannels(), 2);
        const int framesNeeded = info.numSamples;
        const int destStart = info.startSample;
        int maxPullLoops = kPullLoopLimit;

        const int desiredPrefill = computeDesiredPrefillSamples();
        updatePrefillTarget(desiredPrefill);

        while (fifo->getNumReady() < framesNeeded + m_prefillTargetSamples && maxPullLoops > 0) {
            const int shortfall = std::max(0, (framesNeeded + m_prefillTargetSamples) - fifo->getNumReady());
            int pullSize = stretcher->getSamplesRequired();
            if (pullSize <= 0)
                pullSize = std::max(kMinPullSize, shortfall);
            const int scratchCapacity = scratchBuffer.getNumSamples();
            if (scratchCapacity < kMinPullSize) [[unlikely]] {
                info.clearActiveBufferRegion();
                return;
            }
            const int maxPull = std::min({kMaxPullSize, m_maxProcessSize, scratchCapacity});
            pullSize = std::clamp(pullSize, kMinPullSize, maxPull);

            juce::AudioSourceChannelInfo pullInfo;
            pullInfo.buffer = &scratchBuffer;
            pullInfo.startSample = 0;
            pullInfo.numSamples = pullSize;
            scratchBuffer.clear(0, pullSize);

            const int startPadRemaining = m_startPadRemaining.load(std::memory_order_relaxed);
            if (startPadRemaining > 0) {
                const int padNow = std::min(startPadRemaining, pullSize);
                m_startPadRemaining.store(startPadRemaining - padNow, std::memory_order_relaxed);

                if (padNow < pullSize) {
                    juce::AudioSourceChannelInfo tailInfo;
                    tailInfo.buffer = &scratchBuffer;
                    tailInfo.startSample = padNow;
                    tailInfo.numSamples = pullSize - padNow;
                    source->getNextAudioBlock(tailInfo);
                }
            } else {
                source->getNextAudioBlock(pullInfo);
            }

            const float* inputs[2] = { scratchBuffer.getReadPointer(0), scratchBuffer.getReadPointer(1) };
            if (scratchBuffer.getNumChannels() == 1)
                inputs[1] = inputs[0];

            stretcher->process(inputs, pullSize, false);

            int avail = stretcher->available();
            int toTrim = m_startDelayTrimRemaining.load(std::memory_order_relaxed);
            if (avail > 0 && toTrim > 0) {
                const int trimNow = std::min(avail, toTrim);
                trimStretcherOutput(trimNow);
                m_startDelayTrimRemaining.store(toTrim - trimNow, std::memory_order_relaxed);
                avail = stretcher->available();
            }

            if (avail > 0) {
                const int fifoCapacity = fifo->getTotalSize();
                const int outputCapacity = outputBuffer.getNumSamples();
                const int writeCapacity = std::min(fifoCapacity, outputCapacity);
                avail = std::min(avail, writeCapacity);
                if (avail <= 0) {
                    --maxPullLoops;
                    continue;
                }

                int start1, size1, start2, size2;
                fifo->prepareToWrite(avail, start1, size1, start2, size2);

                if (size1 > 0) {
                    float* outputs1[2] = { outputBuffer.getWritePointer(0, start1), outputBuffer.getWritePointer(1, start1) };
                    stretcher->retrieve(outputs1, size1);
                }
                if (size2 > 0) {
                    float* outputs2[2] = { outputBuffer.getWritePointer(0, start2), outputBuffer.getWritePointer(1, start2) };
                    stretcher->retrieve(outputs2, size2);
                }

                fifo->finishedWrite(size1 + size2);
            }

            --maxPullLoops;
        }

        updateReportedLatency(m_prefillTargetSamples);

        const int ready = fifo->getNumReady();
        const int toRead = std::min(ready, framesNeeded);
        if (toRead <= 0) {
            info.clearActiveBufferRegion();
            return;
        }

        int start1, size1, start2, size2;
        fifo->prepareToRead(toRead, start1, size1, start2, size2);

        if (size1 > 0) {
            for (int ch = 0; ch < numCh; ++ch)
                info.buffer->copyFrom(ch, destStart, outputBuffer, ch, start1, size1);
        }
        if (size2 > 0) {
            for (int ch = 0; ch < numCh; ++ch)
                info.buffer->copyFrom(ch, destStart + size1, outputBuffer, ch, start2, size2);
        }

        fifo->finishedRead(size1 + size2);

        if (toRead < framesNeeded) {
            const int remainderStart = destStart + toRead;
            const int remainderLen = framesNeeded - toRead;
            for (int ch = 0; ch < numCh; ++ch)
                info.buffer->clear(ch, remainderStart, remainderLen);
        }

        applySwitchFade(info);
    }

int TimeStretchAudioSource::getLatencySamples() const {
        int delay = m_reportedLatencySamples.load(std::memory_order_relaxed);
        delay += std::max(0, m_startDelayTrimRemaining.load(std::memory_order_relaxed));
        return delay;
    }

void TimeStretchAudioSource::updateReportedLatency(int targetSamples) {
        targetSamples = std::max(0, targetSamples);
        const int current = m_reportedLatencySamples.load(std::memory_order_relaxed);
        if (current == targetSamples)
            return;

        const int step = current < targetSamples
            ? std::max(8, (targetSamples - current) / 4)
            : std::max(16, (current - targetSamples) / 6);

        const int next = current < targetSamples
            ? std::min(targetSamples, current + step)
            : std::max(targetSamples, current - step);

        m_reportedLatencySamples.store(next, std::memory_order_relaxed);
    }

int TimeStretchAudioSource::computeDesiredPrefillSamples() const {
        if (!m_pitchLockEnabled.load(std::memory_order_relaxed))
            return 0;

        const double tempoDelta = std::abs(m_appliedTempoRatio - 1.0);
        if (tempoDelta < kPrefillDeadbandTempoDelta)
            return 0;

        const int dynamic = static_cast<int>(std::lround(tempoDelta * sampleRate * kPrefillDynamicFactor));
        const int extremeBoost = tempoDelta > kPrefillExtremeThreshold
            ? static_cast<int>(std::lround((tempoDelta - kPrefillExtremeThreshold) * sampleRate * kPrefillExtremeFactor))
            : 0;
        const int hardCap = std::max(kMinPullSize, m_prefillHardCapSamples);
        return std::clamp(dynamic + extremeBoost, 0, hardCap);
    }

void TimeStretchAudioSource::updatePrefillTarget(int desiredPrefill) {
        if (m_prefillTargetSamples == desiredPrefill)
            return;

        if (desiredPrefill > m_prefillTargetSamples) {
            const int step = std::max(16, (desiredPrefill - m_prefillTargetSamples) / 6);
            m_prefillTargetSamples = std::min(desiredPrefill, m_prefillTargetSamples + step);
        } else {
            const int step = std::max(32, (m_prefillTargetSamples - desiredPrefill) / 3);
            m_prefillTargetSamples = std::max(desiredPrefill, m_prefillTargetSamples - step);
        }
    }

void TimeStretchAudioSource::trimStretcherOutput(int samplesToTrim) {
        if (!stretcher || samplesToTrim <= 0)
            return;

        while (samplesToTrim > 0) {
            const int avail = stretcher->available();
            if (avail <= 0)
                break;

            const int trimCapacity = trimBuffer.getNumSamples();
            if (trimCapacity <= 0)
                break;

            const int chunk = std::min({samplesToTrim, avail, trimCapacity});

            float* trimOut[2] = {
                trimBuffer.getWritePointer(0),
                trimBuffer.getWritePointer(1)
            };
            stretcher->retrieve(trimOut, chunk);
            samplesToTrim -= chunk;
        }
    }

void TimeStretchAudioSource::resetRealtimePipeline(bool prewarm) {
        if (fifo)
            fifo->reset();
        m_prefillTargetSamples = 0;
        m_reportedLatencySamples.store(0, std::memory_order_relaxed);
        m_startPadRemaining.store(0, std::memory_order_relaxed);
        m_startDelayTrimRemaining.store(0, std::memory_order_relaxed);
        m_switchFadeRemaining.store(kSwitchFadeSamples, std::memory_order_relaxed);
        if (stretcher) {
            stretcher->reset();
            updateStretcherRatios();
            if (prewarm)
                prewarmStretcher();
        }
    }

void TimeStretchAudioSource::prewarmStretcher() {
        if (!stretcher) return;

        const int pad = static_cast<int>(stretcher->getPreferredStartPad());
        if (pad > 0) {
            int remainingPad = pad;
            while (remainingPad > 0) {
                const int chunk = std::min(remainingPad, prewarmZeroBuffer.getNumSamples());
                if (chunk <= 0)
                    break;
                const float* ptrs[2] = {
                    prewarmZeroBuffer.getReadPointer(0),
                    prewarmZeroBuffer.getReadPointer(1)
                };
                stretcher->process(ptrs, chunk, false);
                remainingPad -= chunk;
            }
        }

        // Keep feeding zeros and draining until the full start delay is consumed.
        int remaining = static_cast<int>(stretcher->getStartDelay());
        for (int guard = 128; guard > 0 && remaining > 0; --guard) {
            int avail = stretcher->available();
            if (avail <= 0) {
                const float* ptrs[2] = {
                    prewarmZeroBuffer.getReadPointer(0),
                    prewarmZeroBuffer.getReadPointer(1)
                };
                stretcher->process(ptrs, kMinPullSize, false);
                avail = stretcher->available();
            }
            if (avail > 0) {
                const int chunk = std::min({remaining, avail, trimBuffer.getNumSamples()});
                if (chunk <= 0)
                    break;
                float* ptrs[2] = {
                    trimBuffer.getWritePointer(0),
                    trimBuffer.getWritePointer(1)
                };
                stretcher->retrieve(ptrs, chunk);
                remaining -= chunk;
            }
        }

        m_startPadRemaining.store(0, std::memory_order_relaxed);
        m_startDelayTrimRemaining.store(0, std::memory_order_relaxed);
    }

void TimeStretchAudioSource::updateStretcherRatios() {
        if (!stretcher)
            return;

        const double safeTempoRatio = std::clamp(std::abs(m_appliedTempoRatio), 0.01, 8.0);
        const bool pitchLock = m_pitchLockEnabled.load(std::memory_order_relaxed);

        // Tempo is controlled by ResamplingAudioSource. Rubber Band stays at
        // 1:1 time and optionally compensates pitch when keylock is enabled.
        stretcher->setTimeRatio(1.0);
        stretcher->setPitchScale(pitchLock ? (1.0 / safeTempoRatio) : 1.0);
    }

void TimeStretchAudioSource::applyPendingRatioChange() {
        const double target = m_targetTempoRatio.load(std::memory_order_acquire);
        const double delta = target - m_appliedTempoRatio;
        if (std::abs(delta) < kTempoUpdateEpsilon)
            return;

        m_appliedTempoRatio += std::clamp(delta,
                                          -kMaxTempoRatioStepPerBlock,
                                          kMaxTempoRatioStepPerBlock);
        updateStretcherRatios();
    }

void TimeStretchAudioSource::applySwitchFade(const juce::AudioSourceChannelInfo& info) {
        int remaining = m_switchFadeRemaining.load(std::memory_order_relaxed);
        if (remaining <= 0 || !info.buffer || info.numSamples <= 0)
            return;

        const int fadeNow = std::min(remaining, info.numSamples);
        const int fadeStart = kSwitchFadeSamples - remaining;
        const int channels = info.buffer->getNumChannels();

        for (int i = 0; i < fadeNow; ++i) {
            const float gain = static_cast<float>(fadeStart + i + 1)
                / static_cast<float>(kSwitchFadeSamples);
            for (int ch = 0; ch < channels; ++ch)
                info.buffer->setSample(ch,
                                       info.startSample + i,
                                       info.buffer->getSample(ch, info.startSample + i) * gain);
        }

        m_switchFadeRemaining.store(remaining - fadeNow, std::memory_order_relaxed);
    }
