#include "audio/internal/ScratchResampler.h"

#include "HermiteKernel.h"

#include <algorithm>
#include <cmath>

namespace engine::audio {

namespace {
constexpr int kWindowPadSamples = 4;
constexpr int kWindowBuffersPerSide = 3;
constexpr int kCapacityBuffers = 8;
} // namespace

void ScratchResampler::prepare(int numChannels, int maxBlockSize, double outputSampleRate)
{
    m_channels = std::max(1, numChannels);
    m_deviceBufferSize = std::max(64, maxBlockSize);
    m_blockSize = m_deviceBufferSize;
    m_outputSampleRate = std::max(1.0, outputSampleRate);

    // Hold ~0.5s of audio so slow scratching stays inside the RAM window and does
    // not decode/read on the audio thread on every small move (the main cause of
    // crackle during slow, precise scratching).
    const int timeCapacity = static_cast<int>(std::lround(m_outputSampleRate * 0.5));
    const int capacity = std::clamp(std::max(m_deviceBufferSize * kCapacityBuffers, timeCapacity),
                                    768, 262144);
    m_sourceBuffer.setSize(m_channels, capacity, false, true, true);
    m_sourceBuffer.clear();
    m_sourceSize = 0;
    m_readPos = 0.0;
    m_bufferOriginSample = 0.0;
    m_lastRate = 0.0;
    m_smoothedRate = 0.0;
}

void ScratchResampler::reset(double readPositionSamples) noexcept
{
    m_readPos = readPositionSamples;
    m_bufferOriginSample = readPositionSamples;
    m_sourceSize = 0;
    m_lastRate = 0.0;
    m_smoothedRate = 0.0;
    m_trackVel = 0.0;
}

void ScratchResampler::setReadPositionSamples(double readPositionSamples) noexcept
{
    if (!std::isfinite(readPositionSamples))
        readPositionSamples = 0.0;
    m_readPos = wrapPosition(readPositionSamples);
}

void ScratchResampler::nudgeReadPositionSamples(double deltaSamples) noexcept
{
    if (std::abs(deltaSamples) < 1e-12)
        return;
    setReadPositionSamples(m_readPos + deltaSamples);
}

void ScratchResampler::snapSmoothedRate(double rate) noexcept
{
    m_smoothedRate = rate;
    m_lastRate = rate;
}

void ScratchResampler::primeTrackerVelocity(double ratePerOutputSample) noexcept
{
    m_trackVel = ratePerOutputSample * m_outputSampleRate;
}

void ScratchResampler::setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle) noexcept
{
    m_cache = cache;
    m_cacheHandle = handle;
    m_sourceSize = 0;
    m_bufferOriginSample = m_readPos;
    m_starvationGain = 0.0f;
}

ScratchCacheStats ScratchResampler::cacheStats() const noexcept
{
    return {m_pageHits.load(), m_pageMisses.load(), m_starvationBlocks.load(),
            m_recoveryEvents.load(), m_droppedRequests.load(), m_generationMismatches.load(),
            m_diskReadsFromAudioThread.load()};
}

double ScratchResampler::wrapPosition(double pos) const noexcept
{
    if (m_loopActive && m_loopOutSample > m_loopInSample + 1.0) {
        const double len = m_loopOutSample - m_loopInSample;
        double o = pos - m_loopInSample;
        o = std::fmod(o, len);
        if (o < 0.0)
            o += len;
        return m_loopInSample + o;
    }

    if (m_trackLengthSamples <= 1.0)
        return std::max(0.0, pos);

    return std::clamp(pos, 0.0, m_trackLengthSamples - 1.0);
}

void ScratchResampler::windowMargins(double rate,
                                     int outputBlockSize,
                                     int& lookBehind,
                                     int& lookAhead) const noexcept
{
    const int blockSamples = std::max(m_deviceBufferSize, outputBlockSize);
    // Time-based floor (~100ms each side) so even at near-zero scratch rate the
    // window spans enough audio to avoid constant edge reloads on the audio thread.
    const int minMargin = static_cast<int>(std::lround(m_outputSampleRate * 0.10));
    const int baseMargin = std::max(blockSamples * kWindowBuffersPerSide, minMargin)
        + kHermiteRadius + kWindowPadSamples;
    const int speedMargin = static_cast<int>(
        std::ceil(std::abs(rate) * static_cast<double>(blockSamples)))
        + kHermiteRadius + kWindowPadSamples;

    lookBehind = std::max(baseMargin, speedMargin);
    lookAhead = lookBehind;

    const int halfCapacity = std::max(kMinWindowSamples, m_sourceBuffer.getNumSamples() / 2 - 8);
    lookBehind = std::min(lookBehind, halfCapacity);
    lookAhead = std::min(lookAhead, halfCapacity);
}

bool ScratchResampler::needsWindowReload(double minAbsPos, double maxAbsPos) const noexcept
{
    if (m_sourceSize < kMinWindowSamples)
        return true;

    const double relMin = minAbsPos - m_bufferOriginSample;
    const double relMax = maxAbsPos - m_bufferOriginSample;
    const int edgeGuard = kHermiteRadius + std::max(32, m_deviceBufferSize / 3);

    return relMin < static_cast<double>(edgeGuard)
        || relMax > static_cast<double>(m_sourceSize - edgeGuard - 1);
}

bool ScratchResampler::positionInWindow(double position) const noexcept
{
    return m_sourceSize >= kMinWindowSamples
        && position >= m_bufferOriginSample + kHermiteRadius
        && position < m_bufferOriginSample + m_sourceSize - kHermiteRadius - 1;
}

void ScratchResampler::prefetchAround(double readPositionSamples) noexcept
{
    if (!m_cache || !m_cacheHandle.isValid() || m_cacheHandle.pageCount() <= 0)
        return;
    const auto sample = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(readPositionSamples)),
        0, std::max<std::int64_t>(0, m_cacheHandle.lengthInSamples() - 1));
    const auto current = AudioPage::pageIndexForSample(sample);
    const auto first = std::max<std::int64_t>(0, current - 6);
    const auto last = std::min<std::int64_t>(m_cacheHandle.pageCount() - 1, current + 6);
    (void)m_cache->requestPage(m_cacheHandle, current,
                               AudioCachePriority::RealtimeCritical);
    (void)m_cache->requestRange(m_cacheHandle, first, last,
                                AudioCachePriority::ScratchNearPlayhead);
}

bool ScratchResampler::refillWindowFromCache(double rate, int outputBlockSize) noexcept
{
    if (!m_cache || !m_cacheHandle.isValid()) return false;

    int lookBehind = 0;
    int lookAhead = 0;
    windowMargins(rate, outputBlockSize, lookBehind, lookAhead);
    if (rate > 0.05) {
        lookBehind = std::max(kMinWindowSamples, lookBehind / 2);
        lookAhead = std::min(m_sourceBuffer.getNumSamples() - lookBehind, lookAhead * 2);
    } else if (rate < -0.05) {
        lookAhead = std::max(kMinWindowSamples, lookAhead / 2);
        lookBehind = std::min(m_sourceBuffer.getNumSamples() - lookAhead, lookBehind * 2);
    }
    const auto trackLength = m_cacheHandle.lengthInSamples();
    const auto start = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(m_readPos)) - lookBehind);
    const auto wanted = std::min<std::int64_t>(m_sourceBuffer.getNumSamples(), lookBehind + lookAhead + outputBlockSize);
    const auto count = std::min<std::int64_t>(wanted, std::max<std::int64_t>(0, trackLength - start));
    if (count < kMinWindowSamples) return false;
    const auto firstPage = AudioPage::pageIndexForSample(start);
    const auto lastPage = AudioPage::pageIndexForSample(start + count - 1);
    bool complete = true;
    for (auto pageIndex = firstPage; pageIndex <= lastPage; ++pageIndex) {
        auto page = m_cache->tryGetPage(m_cacheHandle, pageIndex);
        if (!page) {
            complete = false;
            m_pageMisses.fetch_add(1, std::memory_order_relaxed);
            if (!m_cache->requestPage(m_cacheHandle, pageIndex,
                    pageIndex == AudioPage::pageIndexForSample(static_cast<std::int64_t>(m_readPos))
                        ? AudioCachePriority::RealtimeCritical : AudioCachePriority::ScratchNearPlayhead))
                m_droppedRequests.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (page->trackId != m_cacheHandle.id() || page->generation != m_cacheHandle.generation()) {
            complete = false;
            m_generationMismatches.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        m_pageHits.fetch_add(1, std::memory_order_relaxed);
        const auto copyStart = std::max<std::int64_t>(start, page->firstSample);
        const auto copyEnd = std::min<std::int64_t>(start + count,
            page->firstSample + page->validSampleCount);
        const int copyCount = static_cast<int>(std::max<std::int64_t>(0, copyEnd - copyStart));
        const int sourceOffset = static_cast<int>(copyStart - page->firstSample);
        const int targetOffset = static_cast<int>(copyStart - start);
        for (int channel = 0; channel < m_channels; ++channel) {
            const int sourceChannel = std::min(channel, static_cast<int>(page->channelCount) - 1);
            if (const float* source = page->channelData(static_cast<unsigned>(sourceChannel)))
                std::copy_n(source + sourceOffset, copyCount,
                            m_sourceBuffer.getWritePointer(channel) + targetOffset);
        }
    }
    if (!complete) return false;
    m_bufferOriginSample = static_cast<double>(start);
    m_sourceSize = static_cast<int>(count);
    return true;
}

bool ScratchResampler::ensureWindow(double rate, int outputBlockSize) noexcept
{
    const int blockSamples = std::max(m_deviceBufferSize, outputBlockSize);
    const double blockSpan = std::max(std::abs(rate), std::abs(m_smoothedRate))
                           * static_cast<double>(blockSamples);
    const double pad = static_cast<double>(kHermiteRadius + kWindowPadSamples);
    const double minPos = m_readPos - blockSpan - pad;
    const double maxPos = m_readPos + blockSpan + pad;
    const double clampedMin = std::max(0.0, minPos);
    const double clampedMax = (m_trackLengthSamples > 1.0)
        ? std::min(m_trackLengthSamples - 1.0, maxPos)
        : maxPos;

    if (!needsWindowReload(clampedMin, clampedMax)) return true;
    return refillWindowFromCache(rate, outputBlockSize);
}

float ScratchResampler::readHermite(int channel, double position) const noexcept
{
    const double relativePos = position - m_bufferOriginSample;
    const int i = static_cast<int>(std::floor(relativePos));
    const float frac = static_cast<float>(relativePos - static_cast<double>(i));
    const int ch = std::min(channel, m_sourceBuffer.getNumChannels() - 1);

    const auto at = [&](int idx) -> float {
        if (m_sourceSize <= 0)
            return 0.0f;

        if (idx < 0)
            idx = -idx;
        else if (idx >= m_sourceSize)
            idx = 2 * (m_sourceSize - 1) - idx;

        idx = std::clamp(idx, 0, m_sourceSize - 1);
        return m_sourceBuffer.getSample(ch, idx);
    };

    return engine::audio::cubicHermite(at(i - 1), at(i), at(i + 1), at(i + 2), frac);
}

void ScratchResampler::writeScratchOutput(float* out0, float* out1, int index, bool ready) noexcept
{
    const float step = 1.0f / static_cast<float>(kStarvationFadeSamples);
    if (ready) {
        if (m_starvationGain <= 0.0f) m_recoveryEvents.fetch_add(1, std::memory_order_relaxed);
        m_starvationGain = std::min(1.0f, m_starvationGain + step);
        m_lastOutputL = readHermite(0, m_readPos);
        m_lastOutputR = readHermite(1, m_readPos);
    } else {
        m_starvationGain = std::max(0.0f, m_starvationGain - step);
    }
    if (out0) out0[index] = m_lastOutputL * m_starvationGain;
    if (out1) out1[index] = m_lastOutputR * m_starvationGain;
}

void ScratchResampler::processBlock(double rate,
                                    const juce::AudioSourceChannelInfo& output) noexcept
{
    if (!output.buffer || output.numSamples <= 0) {
        if (output.buffer)
            output.clearActiveBufferRegion();
        return;
    }

    const int start = output.startSample;
    const int numSamples = output.numSamples;
    if (start < 0 || start + numSamples > output.buffer->getNumSamples()) {
        output.clearActiveBufferRegion();
        return;
    }

    if (std::abs(rate) < 1e-7) {
        m_smoothedRate = 0.0;
        m_lastRate = 0.0;
    } else {
        m_lastRate = rate;
    }

    const int outChannels = std::min(output.buffer->getNumChannels(), m_channels);

    float* out0 = outChannels > 0 ? output.buffer->getWritePointer(0, start) : nullptr;
    float* out1 = outChannels > 1 ? output.buffer->getWritePointer(1, start) : nullptr;

    const bool windowReady = ensureWindow(rate, numSamples);
    if (!windowReady) m_starvationBlocks.fetch_add(1, std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i) {
        const double delta = rate - m_smoothedRate;
        const double absRate = std::abs(rate);
        const double slew = absRate < 0.12
            ? std::clamp(0.0008 + absRate * 0.004, 0.0008, 0.006)
            : std::clamp(0.004 + absRate * 0.008, 0.004, 0.030);
        m_smoothedRate += std::clamp(delta, -slew, slew);

        if (std::abs(m_smoothedRate) < 1e-7) {
            writeScratchOutput(out0, out1, i, false);
            if (std::abs(m_smoothedRate) < 1e-7)
                continue;
        }

        m_readPos = wrapPosition(m_readPos);
        writeScratchOutput(out0, out1, i, windowReady && positionInWindow(m_readPos));

        m_readPos += m_smoothedRate;
    }
    m_readPos = wrapPosition(m_readPos);
}

double ScratchResampler::processScratchTracking(double targetPosSamples,
                                                double commandedRate,
                                                double maxAbsRate,
                                                const juce::AudioSourceChannelInfo& output) noexcept
{
    if (!output.buffer || output.numSamples <= 0) {
        if (output.buffer)
            output.clearActiveBufferRegion();
        return 0.0;
    }

    const int start = output.startSample;
    const int numSamples = output.numSamples;
    if (start < 0 || start + numSamples > output.buffer->getNumSamples()) {
        output.clearActiveBufferRegion();
        return 0.0;
    }

    // No audio before t=0 — keep the read head at the start during pre-roll while
    // the visible (negative) playhead is published separately by the UI thread.
    const double target = std::max(0.0, targetPosSamples);

    const double outSr = std::max(1.0, m_outputSampleRate);
    const double dt = 1.0 / outSr;
    const double absMaxRate = std::abs(maxAbsRate);
    const double referenceRate = std::clamp(
        std::isfinite(commandedRate) ? commandedRate : 0.0,
        -absMaxRate,
        absMaxRate);
    const double referenceVelocity = referenceRate * outSr;

    // Critically-damped position + velocity tracker. A position-only tracker
    // assumes the hand stops at every MIDI tick, creating a repeated accelerate /
    // brake sound when USB delivers discrete or batched jog events. The measured
    // hand velocity supplies the moving-reference term while the position error
    // remains authoritative, so no long-term drift is possible.
    constexpr double kTrackHz = 52.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    const double omega = kTwoPi * kTrackHz;
    const double maxVel = absMaxRate * outSr;

    // Window must span the path the read head will travel this block.
    const double estRate = std::clamp(
        std::max(std::abs((target - m_readPos) / static_cast<double>(std::max(1, numSamples))),
                 std::max(std::abs(m_trackVel) * dt, std::abs(referenceRate))) * 1.5,
        0.0, absMaxRate);
    const bool windowReady = ensureWindow(estRate, numSamples);
    if (!windowReady) m_starvationBlocks.fetch_add(1, std::memory_order_relaxed);

    const int outChannels = std::min(output.buffer->getNumChannels(), m_channels);
    float* out0 = outChannels > 0 ? output.buffer->getWritePointer(0, start) : nullptr;
    float* out1 = outChannels > 1 ? output.buffer->getWritePointer(1, start) : nullptr;

    // Feed-forward may lead the last received target by only a few milliseconds.
    // This bridges USB/audio callback cadence without allowing a stale velocity
    // to run the read head away after the platter stops.
    // Short, slow movements should stop almost exactly under the hand. Faster
    // throws get a longer bridge across USB packet gaps, while micro-scratches
    // use only ~2.5 ms of look-ahead and therefore avoid a digital-sounding
    // overshoot/correction cycle.
    const double speedBlend = std::clamp(std::abs(referenceRate) / 1.5, 0.0, 1.0);
    const double maximumPredictionSeconds = 0.0025 + 0.0035 * speedBlend;
    const double predictionSamples = std::abs(referenceVelocity)
        * std::min(maximumPredictionSeconds,
                   static_cast<double>(numSamples) / outSr);
    const double corridorLow = target - predictionSamples;
    const double corridorHigh = target + predictionSamples;

    for (int i = 0; i < numSamples; ++i) {
        const double err = target - m_readPos;
        const double accel = omega * omega * err
            + 2.0 * omega * (referenceVelocity - m_trackVel);
        m_trackVel = std::clamp(m_trackVel + accel * dt, -maxVel, maxVel);

        const double rate = std::clamp(m_trackVel * dt, -absMaxRate, absMaxRate);

        m_readPos = wrapPosition(m_readPos);
        writeScratchOutput(out0, out1, i, windowReady && positionInWindow(m_readPos));
        m_readPos += rate;
        if (!m_loopActive) {
            if (m_readPos > corridorHigh && m_trackVel > 0.0) {
                m_readPos = corridorHigh;
                m_trackVel = 0.0;
            } else if (m_readPos < corridorLow && m_trackVel < 0.0) {
                m_readPos = corridorLow;
                m_trackVel = 0.0;
            }
        }
    }

    m_smoothedRate = m_trackVel * dt;
    m_lastRate = m_smoothedRate;
    m_readPos = wrapPosition(m_readPos);
    return m_smoothedRate;
}

} // namespace engine::audio
