#include "audio/internal/ScratchResampler.h"

#include <algorithm>
#include <cmath>

namespace engine::audio {

std::array<float,
           ScratchResampler::kSincCutoffBands
               * (ScratchResampler::kSincPhaseCount + 1)
               * ScratchResampler::kSincTaps>
    ScratchResampler::s_sincTable {};
std::once_flag ScratchResampler::s_sincTableOnce;

namespace {
constexpr int kWindowPadSamples = 4;
constexpr int kWindowBuffersPerSide = 3;
constexpr int kCapacityBuffers = 12;
constexpr double kPreparedSourceRateCeiling = 192000.0;
constexpr int kMaximumSourceBufferSamples = 524288;
constexpr double kMaximumMotionAccelerationRatesPerSecond = 1'500.0;
constexpr double kMaximumMotionJerkRatesPerSecond2 = 1'000'000.0;
} // namespace

void ScratchResampler::prepare(int numChannels, int maxBlockSize, double outputSampleRate)
{
    m_channels = std::max(1, numChannels);
    m_deviceBufferSize = std::max(64, maxBlockSize);
    m_blockSize = m_deviceBufferSize;
    m_outputSampleRate = std::max(1.0, outputSampleRate);
    if (!m_cacheHandle.isValid())
        m_trackSampleRate = m_outputSampleRate;

    // Reserve for a 192 kHz source up front. Loading a normal track on deck 2
    // therefore does not allocate a new scratch window while deck 1 is playing.
    // Capacity is expressed in source samples: sizing this from the device rate
    // only held 125 ms of a 192 kHz track on a 48 kHz device.
    const int capacity = requiredSourceBufferCapacity(m_trackSampleRate);
    m_sourceBuffer.setSize(m_channels, capacity, false, true, true);
    m_sourceBuffer.clear();
    m_sourceSize = 0;
    m_readPos = 0.0;
    m_bufferOriginSample = 0.0;
    m_lastRate = 0.0;
    m_smoothedRate = 0.0;
    m_previousRateStep = 0.0;
    m_trackVel = 0.0;
    m_referencePos = 0.0;
    m_referenceVelocity = 0.0;
    m_referenceValid = false;
    m_leadSpeedEnvelope = 0.0;
    m_starvationGain = 0.0f;
    m_lastOutputL = 0.0f;
    m_lastOutputR = 0.0f;
    prepareSincTable();
}

int ScratchResampler::requiredSourceBufferCapacity(double sourceSampleRate) const noexcept
{
    const double sizingSourceRate = std::max(
        {m_outputSampleRate, sourceSampleRate, kPreparedSourceRateCeiling});
    const double sourcePerOutput = sizingSourceRate / std::max(1.0, m_outputSampleRate);
    const int timeCapacity = static_cast<int>(std::lround(sizingSourceRate * 0.5));
    const int rateCapacity = static_cast<int>(std::ceil(
        m_deviceBufferSize * kCapacityBuffers * sourcePerOutput));
    return std::clamp(std::max(rateCapacity, timeCapacity),
                      768, kMaximumSourceBufferSamples);
}

void ScratchResampler::prepareSincTable()
{
    std::call_once(s_sincTableOnce, [] {
        constexpr double pi = 3.14159265358979323846;
        constexpr std::size_t phaseTapCount =
            (kSincPhaseCount + 1) * kSincTaps;
        std::array<double, phaseTapCount> windowTable {};
        for (int phase = 0; phase <= kSincPhaseCount; ++phase) {
            const double fraction = static_cast<double>(phase)
                / static_cast<double>(kSincPhaseCount);
            for (int tap = 0; tap < kSincTaps; ++tap) {
                const double offset = static_cast<double>(tap - (kSincRadius - 1));
                const double distance = offset - fraction;
                // Four-term Blackman-Harris suppresses the metallic fold-back
                // that a short Hann kernel leaves behind on fast scratches.
                // Sixty-four taps plus stereo coefficient sharing retains the
                // usable high-speed passband while keeping the stronger
                // stop-band cheaper than evaluating a separate kernel for each
                // channel.
                const double normalizedDistance = distance / kSincRadius;
                const double window = std::abs(normalizedDistance) >= 1.0
                    ? 0.0
                    : 0.35875
                        + 0.48829 * std::cos(pi * normalizedDistance)
                        + 0.14128 * std::cos(2.0 * pi * normalizedDistance)
                        + 0.01168 * std::cos(3.0 * pi * normalizedDistance);
                windowTable[static_cast<std::size_t>(phase) * kSincTaps
                            + static_cast<std::size_t>(tap)] = window;
            }
        }

        const auto tableIndex = [](int band, int phase, int tap) {
            return (static_cast<std::size_t>(band) * (kSincPhaseCount + 1)
                    + static_cast<std::size_t>(phase)) * kSincTaps
                + static_cast<std::size_t>(tap);
        };

        for (int band = 0; band < kSincCutoffBands; ++band) {
            const double bandFraction = static_cast<double>(band)
                / static_cast<double>(kSincCutoffBands - 1);
            const double inverseSpeed = 1.0 - bandFraction
                * (1.0 - 1.0 / kMaximumFilterRate);
            const double speed = 1.0 / inverseSpeed;
            // A source-domain low-pass must happen before decimation. Filtering
            // the already-resampled output cannot remove folded frequencies.
            const double cutoff = 0.90 / speed;
            for (int phase = 0; phase <= kSincPhaseCount; ++phase) {
                const double fraction = static_cast<double>(phase)
                    / static_cast<double>(kSincPhaseCount);
                double sum = 0.0;
                for (int tap = 0; tap < kSincTaps; ++tap) {
                    const double offset = static_cast<double>(tap - (kSincRadius - 1));
                    const double distance = offset - fraction;
                    const double sincPosition = cutoff * distance;
                    const double sinc = std::abs(sincPosition) < 1.0e-12
                        ? 1.0
                        : std::sin(pi * sincPosition) / (pi * sincPosition);
                    const double window = windowTable[
                        static_cast<std::size_t>(phase) * kSincTaps
                        + static_cast<std::size_t>(tap)];
                    const double coefficient = cutoff * sinc * window;
                    s_sincTable[tableIndex(band, phase, tap)]
                        = static_cast<float>(coefficient);
                    sum += coefficient;
                }
                const float normalization = static_cast<float>(
                    std::abs(sum) > 1.0e-12 ? 1.0 / sum : 1.0);
                for (int tap = 0; tap < kSincTaps; ++tap)
                    s_sincTable[tableIndex(band, phase, tap)] *= normalization;
            }
        }
    });
}

void ScratchResampler::reset(double readPositionSamples) noexcept
{
    m_readPos = readPositionSamples;
    m_bufferOriginSample = readPositionSamples;
    m_sourceSize = 0;
    m_lastRate = 0.0;
    m_smoothedRate = 0.0;
    m_previousRateStep = 0.0;
    m_trackVel = 0.0;
    m_referenceVelocity = 0.0;
    m_referenceValid = false;
    m_leadSpeedEnvelope = 0.0;
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

void ScratchResampler::setTrackCacheSource(AudioPageCache* cache, AudioCacheHandle handle)
{
    m_cache = cache;
    m_cacheHandle = handle;
    m_trackSampleRate = handle.isValid() && handle.sampleRate() > 0.0
        ? handle.sampleRate() : m_outputSampleRate;
    const int requiredCapacity = requiredSourceBufferCapacity(m_trackSampleRate);
    if (m_sourceBuffer.getNumChannels() != m_channels
        || m_sourceBuffer.getNumSamples() < requiredCapacity) {
        // Track installation runs behind DeckAudioPipeline's callback gate.
        // Ordinary <=192 kHz files fit the reservation made in prepare(); this
        // path is only a bounded fallback for a larger source or device block.
        m_sourceBuffer.setSize(m_channels, requiredCapacity, false, true, true);
        m_sourceBuffer.clear();
    }
    m_sourceSize = 0;
    m_bufferOriginSample = m_readPos;
    m_starvationGain = 0.0f;
}

ScratchMotionStats ScratchResampler::motionStats() const noexcept
{
    return {m_statMinRate.load(std::memory_order_relaxed),
            m_statMaxRate.load(std::memory_order_relaxed),
            m_statMaxRateStep.load(std::memory_order_relaxed),
            m_statMaxRateStepDelta.load(std::memory_order_relaxed),
            m_statTrackingError.load(std::memory_order_relaxed),
            m_statLead.load(std::memory_order_relaxed),
            m_statLeadLimitedBlocks.load(std::memory_order_relaxed)};
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
    const int minMargin = static_cast<int>(std::lround(m_trackSampleRate * 0.10));
    const int baseMargin = std::max(blockSamples * kWindowBuffersPerSide, minMargin)
        + kSincRadius + kWindowPadSamples;
    const int speedMargin = static_cast<int>(
        std::ceil(std::abs(rate) * static_cast<double>(blockSamples)))
        + kSincRadius + kWindowPadSamples;

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
    const int edgeGuard = kSincRadius + std::max(32, m_deviceBufferSize / 3);
    const bool ownsTrackStart = m_bufferOriginSample <= 0.0;
    const bool ownsTrackEnd = m_trackLengthSamples > 1.0
        && m_bufferOriginSample + static_cast<double>(m_sourceSize)
            >= m_trackLengthSamples;

    return (relMin < static_cast<double>(edgeGuard) && !ownsTrackStart)
        || (relMax > static_cast<double>(m_sourceSize - edgeGuard - 1)
            && !ownsTrackEnd);
}

bool ScratchResampler::positionInWindow(double position) const noexcept
{
    if (m_sourceSize < kMinWindowSamples)
        return false;

    // At a real track boundary the sinc reader can use its reflected extension;
    // a local cache-window boundary cannot, because audio outside that window
    // exists and must be loaded instead of mirrored.
    const bool ownsTrackStart = m_bufferOriginSample <= 0.0;
    const bool ownsTrackEnd = m_trackLengthSamples > 1.0
        && m_bufferOriginSample + static_cast<double>(m_sourceSize)
            >= m_trackLengthSamples;
    const double firstReadable = m_bufferOriginSample
        + (ownsTrackStart ? 0.0 : static_cast<double>(kSincRadius));
    const double endReadable = m_bufferOriginSample
        + static_cast<double>(m_sourceSize)
        - (ownsTrackEnd ? 0.0 : static_cast<double>(kSincRadius + 1));
    return position >= firstReadable && position < endReadable;
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
    // Predict only the directions in which the head can travel during this
    // block. The former symmetric +/-abs(rate) range contradicted the
    // directionally biased refill below: a reverse 8x block could load most of
    // its local window ahead of the head, then run out of samples behind it.
    // Keeping both endpoints also covers a real sign change without assuming
    // that the entire block moves in only the new direction.
    const double minRate = std::min({0.0, m_smoothedRate, rate});
    const double maxRate = std::max({0.0, m_smoothedRate, rate});
    const double pad = static_cast<double>(kSincRadius + kWindowPadSamples);
    const double minPos = m_readPos
        + minRate * static_cast<double>(blockSamples) - pad;
    const double maxPos = m_readPos
        + maxRate * static_cast<double>(blockSamples) + pad;
    const double clampedMin = std::max(0.0, minPos);
    const double clampedMax = (m_trackLengthSamples > 1.0)
        ? std::min(m_trackLengthSamples - 1.0, maxPos)
        : maxPos;

    if (!needsWindowReload(clampedMin, clampedMax))
        return true;

    // Bias a same-direction move toward its destination. A reversal needs both
    // sides of the playhead, so keep the refill symmetric for that block.
    double refillRate = 0.0;
    if (minRate >= 0.0)
        refillRate = maxRate;
    else if (maxRate <= 0.0)
        refillRate = minRate;

    if (!refillWindowFromCache(refillRate, outputBlockSize))
        return false;
    // A caller may legally provide a block larger than the prepared device
    // size. Never claim the whole path is resident when the fixed RT buffer is
    // too small; the starvation fade is safer than reading a partial window.
    return positionInWindow(clampedMin) && positionInWindow(clampedMax);
}

void ScratchResampler::readBandlimitedStereo(double position, double rate,
                                             float& left, float& right) const noexcept
{
    left = 0.0f;
    right = 0.0f;
    if (m_sourceSize <= 0 || m_sourceBuffer.getNumChannels() <= 0)
        return;

    const double relativePos = position - m_bufferOriginSample;
    const int centre = static_cast<int>(std::floor(relativePos));
    const double fraction = relativePos - static_cast<double>(centre);
    const float* sourceLeft = m_sourceBuffer.getReadPointer(0);
    const float* sourceRight = m_sourceBuffer.getReadPointer(
        std::min(1, m_sourceBuffer.getNumChannels() - 1));
    const auto reflectedIndex = [this](int idx) noexcept {
        if (idx < 0)
            idx = -idx;
        else if (idx >= m_sourceSize)
            idx = 2 * (m_sourceSize - 1) - idx;

        return std::clamp(idx, 0, m_sourceSize - 1);
    };

    const double phasePosition = fraction * kSincPhaseCount;
    const int phase0 = std::clamp(static_cast<int>(phasePosition),
                                  0, kSincPhaseCount - 1);
    const int phase1 = phase0 + 1;
    const float phaseMix = static_cast<float>(phasePosition - phase0);

    const double speed = std::clamp(
        std::max(1.0, std::abs(rate)), 1.0, kMaximumFilterRate);
    const double bandPosition = (1.0 - 1.0 / speed)
        * static_cast<double>(kSincCutoffBands - 1)
        / (1.0 - 1.0 / kMaximumFilterRate);
    const int band0 = std::clamp(static_cast<int>(bandPosition),
                                 0, kSincCutoffBands - 1);
    const int band1 = std::min(band0 + 1, kSincCutoffBands - 1);
    const float bandMix = static_cast<float>(bandPosition - band0);
    const auto tableBase = [](int band, int phase) {
        return (static_cast<std::size_t>(band) * (kSincPhaseCount + 1)
                + static_cast<std::size_t>(phase)) * kSincTaps;
    };
    const auto base00 = tableBase(band0, phase0);
    const auto base01 = tableBase(band0, phase1);
    const auto base10 = tableBase(band1, phase0);
    const auto base11 = tableBase(band1, phase1);
    const bool interpolateCutoffBand = band0 != band1 && bandMix > 0.0f;

    for (int tap = 0; tap < kSincTaps; ++tap) {
        const auto offset = static_cast<std::size_t>(tap);
        const float band0Coefficient = std::lerp(
            s_sincTable[base00 + offset],
            s_sincTable[base01 + offset], phaseMix);
        const float coefficient = interpolateCutoffBand
            ? std::lerp(
                band0Coefficient,
                std::lerp(s_sincTable[base10 + offset],
                          s_sincTable[base11 + offset], phaseMix),
                bandMix)
            : band0Coefficient;
        const int sourceIndex = reflectedIndex(
            centre + tap - (kSincRadius - 1));
        left += sourceLeft[sourceIndex] * coefficient;
        right += sourceRight[sourceIndex] * coefficient;
    }
}

void ScratchResampler::writeScratchOutput(float* out0, float* out1, int index,
                                          bool ready, double rate) noexcept
{
    const float step = 1.0f / static_cast<float>(kStarvationFadeSamples);
    if (ready) {
        if (m_starvationGain <= 0.0f) m_recoveryEvents.fetch_add(1, std::memory_order_relaxed);
        m_starvationGain = std::min(1.0f, m_starvationGain + step);
        readBandlimitedStereo(m_readPos, rate, m_lastOutputL, m_lastOutputR);
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

    const double targetRate = std::isfinite(rate) ? rate : 0.0;
    m_lastRate = targetRate;

    const int outChannels = std::min(output.buffer->getNumChannels(), m_channels);

    float* out0 = outChannels > 0 ? output.buffer->getWritePointer(0, start) : nullptr;
    float* out1 = outChannels > 1 ? output.buffer->getWritePointer(1, start) : nullptr;

    const bool windowReady = ensureWindow(targetRate, numSamples);
    if (!windowReady) m_starvationBlocks.fetch_add(1, std::memory_order_relaxed);

    // The release controller publishes one exponentially decaying target per
    // callback. Treat those values as endpoints of a full-block trajectory,
    // then apply the same time-based acceleration and jerk bounds as tracking.
    // This prevents both callback-frequency pitch stairs and a short-buffer
    // 2x->8x impulse when the physical release estimate is ahead of the rate the
    // final tracking block had time to render.
    const double blockStartRate = m_smoothedRate;
    const double normalizedRateScale = std::max(1.0, m_trackSampleRate)
        / std::max(1.0, m_outputSampleRate);
    const double dt = 1.0 / std::max(1.0, m_outputSampleRate);
    const double maxRateStep = kMaximumMotionAccelerationRatesPerSecond
        * normalizedRateScale * dt;
    const double maxRateStepDelta = kMaximumMotionJerkRatesPerSecond2
        * normalizedRateScale * dt * dt;
    double statMinRate = m_smoothedRate;
    double statMaxRate = m_smoothedRate;
    double statMaxRateStep = 0.0;
    double statMaxRateStepDelta = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        const double previousRate = m_smoothedRate;
        const double blockFraction = static_cast<double>(i + 1)
            / static_cast<double>(numSamples);
        const double desiredRate = std::lerp(
            blockStartRate, targetRate, blockFraction);
        const double accelerationLimitedStep = std::clamp(
            desiredRate - previousRate, -maxRateStep, maxRateStep);
        const double rateStep = std::clamp(
            accelerationLimitedStep,
            m_previousRateStep - maxRateStepDelta,
            m_previousRateStep + maxRateStepDelta);
        m_smoothedRate += rateStep;
        const double actualRateStep = m_smoothedRate - previousRate;
        statMinRate = std::min(statMinRate, m_smoothedRate);
        statMaxRate = std::max(statMaxRate, m_smoothedRate);
        statMaxRateStep = std::max(statMaxRateStep, std::abs(actualRateStep));
        statMaxRateStepDelta = std::max(
            statMaxRateStepDelta, std::abs(actualRateStep - m_previousRateStep));
        m_previousRateStep = actualRateStep;

        if (std::abs(m_smoothedRate) < 1e-7) {
            writeScratchOutput(out0, out1, i, false, m_smoothedRate);
            if (std::abs(m_smoothedRate) < 1e-7)
                continue;
        }

        m_readPos = wrapPosition(m_readPos);
        writeScratchOutput(out0, out1, i,
                           windowReady && positionInWindow(m_readPos),
                           m_smoothedRate);

        m_readPos += m_smoothedRate;
    }
    m_statMinRate.store(statMinRate, std::memory_order_relaxed);
    m_statMaxRate.store(statMaxRate, std::memory_order_relaxed);
    m_statMaxRateStep.store(statMaxRateStep, std::memory_order_relaxed);
    m_statMaxRateStepDelta.store(statMaxRateStepDelta, std::memory_order_relaxed);
    m_readPos = wrapPosition(m_readPos);
}

double ScratchResampler::processScratchTracking(double targetPosSamples,
                                                double commandedRate,
                                                double maxAbsRate,
                                                const juce::AudioSourceChannelInfo& output,
                                                double inputLeadSeconds) noexcept
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
    // Motion bounds are specified in normalized platter rates. Convert them to
    // source samples/output sample independently of the caller's safety limit.
    const double normalizedRateScale = std::max(1.0, m_trackSampleRate)
        / outSr;
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
    //
    // The bandwidth is deliberately a constant. Making it follow the error size
    // was tried and measured worse: the staleness of the last controller event
    // varies by up to a couple of milliseconds of travel from block to block,
    // which is the same error magnitude a real hand reversal produces, so an
    // error-driven bandwidth cannot tell them apart and simply lets the event
    // quantisation through as rate modulation. Per-sample rate continuity at 1x
    // degraded roughly six-fold for no meaningful gain in reversal tracking.
    // The loop starts correcting inside each controller frame and settles over
    // the following few milliseconds, while the continuous reference below
    // integrates quantised jog events instead of reproducing their edges as
    // audible pitch steps.
    constexpr double kTrackHz = 56.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    const double omega = kTwoPi * kTrackHz;

    // Window must span the path the read head will travel this block.
    const double positionCorrectionRate = (target - m_readPos)
        / static_cast<double>(std::max(1, numSamples));
    double estRate = m_trackVel * dt;
    if (std::abs(referenceRate) > std::abs(estRate))
        estRate = referenceRate;
    if (std::abs(positionCorrectionRate) > std::abs(estRate))
        estRate = positionCorrectionRate;
    estRate = std::clamp(estRate * 1.5, -absMaxRate, absMaxRate);
    const bool windowReady = ensureWindow(estRate, numSamples);
    if (!windowReady) m_starvationBlocks.fetch_add(1, std::memory_order_relaxed);

    const int outChannels = std::min(output.buffer->getNumChannels(), m_channels);
    float* out0 = outChannels > 0 ? output.buffer->getWritePointer(0, start) : nullptr;
    float* out1 = outChannels > 1 ? output.buffer->getWritePointer(1, start) : nullptr;

    // `target` is where the hand was when the last controller event arrived, and
    // the audio produced here is played out over the whole block. Holding that
    // snapshot still for every sample asks the read head to stop while the
    // platter is in fact still turning: it runs into the lead limit part-way
    // through the block, freezes, then lurches when the next target arrives.
    // That cycle is what makes slow drags sound digital and fast drags chop.
    //
    // Advance the target with the measured hand velocity instead. Sample i is
    // heard dt later than sample 0, so the hand has moved that much further by
    // then, and a fixed input lead absorbs the age the event already had.
    //
    // Shortening this horizon when the reported velocity is changing fast — so
    // that a reversal predicts less far past the turn — was tried and measured
    // worse: the overshoot barely moved while per-sample rate continuity through
    // an alternating scratch degraded by a factor of twenty, because the horizon
    // also sets the runaway bound below and collapsing both together makes the
    // guard fire during ordinary movement.
    // The lead is supplied by the caller because it depends on which input is
    // driving: a jog ring's target is a couple of milliseconds old, an on-screen
    // drag's is most of a UI frame old. Assuming the former for the latter
    // leaves the head permanently trailing, and every arriving event then lands
    // as a position correction — which is heard as a shake on slow drags.
    const double kInputLeadSeconds =
        std::clamp(std::isfinite(inputLeadSeconds) ? inputLeadSeconds : 0.002,
                   0.0, 0.030);
    const double blockSeconds = static_cast<double>(numSamples) / outSr;
    const double maxLeadSeconds = kInputLeadSeconds + blockSeconds;
    // Runaway guard only. The bound follows whichever of the commanded and the
    // actual tracker velocity is larger, so it stays clear of the head both
    // while it is catching up and while it is coasting out a movement whose
    // command has already decayed — in neither case may it snap the position.
    // A stale command is handled by the tracker itself: the controller decays
    // the reference velocity to zero within a few milliseconds and the position
    // term then glides the head onto the target with no jump.
    // The moving target is itself hard-clamped below, so the head can only
    // overshoot it transiently — the guard therefore needs real headroom above
    // the extrapolation horizon, or it fires during ordinary steady motion and
    // collapses onto the slack term at every direction change, snapping the
    // position exactly where the scratch is most sensitive.
    constexpr double kRunawayHorizonFactor = 4.0;
    constexpr double kRunawaySlackOutputSamples = 256.0;

    // Sizing the bound from the instantaneous speed makes it collapse to the
    // slack term exactly at a direction change, because both the commanded and
    // the tracked velocity pass through zero there — while the position error
    // carried in from the preceding fast motion does not vanish with them. The
    // guard then fires mid-reversal, snaps the read head and clamps the tracker
    // velocity, which is heard as a click at the turn: measured up to a 0.75x
    // rate step on a 6x reversal. So the bound follows an envelope that rises
    // instantly and decays over a quarter second. A genuine runaway is still
    // caught, just not within the few milliseconds a hand spends at zero.
    const double instantSpeed = std::max(std::abs(referenceVelocity), std::abs(m_trackVel));
    m_leadSpeedEnvelope = std::max(instantSpeed,
                                   m_leadSpeedEnvelope * std::exp(-blockSeconds / 0.25));
    // Where the hand is estimated to be right now, given the event we have and
    // how old it already is.
    const double handNow = target + referenceVelocity * kInputLeadSeconds;

    // Join the persistent reference state to the newest predicted hand state
    // with a minimum-jerk quintic Hermite curve. The former linear correction
    // changed velocity at every callback edge; a cubic fixed that velocity edge
    // but still let acceleration change there on extreme reversals. Zero endpoint
    // acceleration makes adjacent curves C2, while the absolute position and
    // velocity endpoints still prevent sticker drift and preserve hand speed.
    if (!m_referenceValid) {
        m_referencePos = handNow;
        m_referenceVelocity = referenceVelocity;
        m_referenceValid = true;
    }
    const double referenceStartPos = m_referencePos;
    const double referenceStartVelocity = m_referenceVelocity;
    const double referenceEndPos = target
        + referenceVelocity * (kInputLeadSeconds + blockSeconds);
    const double referenceEndVelocity = referenceVelocity;
    const double positionDelta = referenceEndPos - referenceStartPos;
    const double velocityStartBlock = referenceStartVelocity * blockSeconds;
    const double velocityEndBlock = referenceEndVelocity * blockSeconds;
    const double curve1 = velocityStartBlock;
    const double curve3 = 10.0 * positionDelta
        - 6.0 * velocityStartBlock - 4.0 * velocityEndBlock;
    const double curve4 = -15.0 * positionDelta
        + 8.0 * velocityStartBlock + 7.0 * velocityEndBlock;
    const double curve5 = 6.0 * positionDelta
        - 3.0 * velocityStartBlock - 3.0 * velocityEndBlock;
    const double invBlockSeconds = 1.0 / std::max(dt, blockSeconds);

    double statMinRate = 0.0;
    double statMaxRate = 0.0;
    double statMaxRateStep = 0.0;
    double statMaxRateStepDelta = 0.0;
    double previousRate = m_smoothedRate;
    bool leadLimited = false;

    for (int i = 0; i < numSamples; ++i) {
        const double curveU = static_cast<double>(i)
            / static_cast<double>(numSamples);
        const double curveU2 = curveU * curveU;
        const double curveU3 = curveU2 * curveU;
        const double curveU4 = curveU3 * curveU;
        const double curveU5 = curveU4 * curveU;
        const double movingTarget = referenceStartPos
            + curve1 * curveU
            + curve3 * curveU3
            + curve4 * curveU4
            + curve5 * curveU5;
        const double movingReferenceVelocity = (
            curve1
            + 3.0 * curve3 * curveU2
            + 4.0 * curve4 * curveU3
            + 5.0 * curve5 * curveU4) * invBlockSeconds;
        const double err = movingTarget - m_readPos;
        const double accel = omega * omega * err
            + 2.0 * omega * (movingReferenceVelocity - m_trackVel);
        const double currentRate = m_trackVel * dt;
        // Keep a timestamp spike or a large absolute-position correction from
        // demanding an acceleration no physical jog can produce. 1500x/s still
        // reaches the complete 8x range in about 5.3 milliseconds; faster turns
        // are rounded instead of becoming a metallic pitch impulse.
        const double accelerationLimitedRateStep = std::clamp(
            accel * dt * dt,
            -kMaximumMotionAccelerationRatesPerSecond * normalizedRateScale * dt,
            kMaximumMotionAccelerationRatesPerSecond * normalizedRateScale * dt);
        // Acceleration itself must not appear as a new block-edge step. This
        // jerk bound reaches the full physical acceleration budget in roughly
        // 1.5 ms at 48 kHz, but removes the single-sample edge that produces a
        // click-like pitch impulse on an abrupt reversal.
        const double maxRateStepDelta = kMaximumMotionJerkRatesPerSecond2
            * normalizedRateScale * dt * dt;
        const double desiredRateStep = std::clamp(
            accelerationLimitedRateStep,
            m_previousRateStep - maxRateStepDelta,
            m_previousRateStep + maxRateStepDelta);
        const double desiredRate = currentRate + desiredRateStep;

        // A hard +/-max clamp turns the last acceleration sample into zero in
        // one step. On a very fast reversal the position servo can briefly ask
        // for more than the public 8x range while catching up, and that clamp
        // edge is heard as a sharp digital chirp. Approach either safety bound
        // exponentially instead. The allowed step meets the unconstrained
        // servo continuously, then tends to zero at the boundary; inward motion
        // remains completely unaffected. The time constant is shorter than one
        // controller frame, so it is a safety knee rather than input smoothing.
        constexpr double kSpeedLimitApproachSeconds = 0.0015;
        const double limitAlpha = 1.0
            - std::exp(-dt / kSpeedLimitApproachSeconds);
        const double lowerRate = currentRate
            + (-absMaxRate - currentRate) * limitAlpha;
        const double upperRate = currentRate
            + (absMaxRate - currentRate) * limitAlpha;
        const double rate = std::clamp(desiredRate, lowerRate, upperRate);
        m_trackVel = rate * outSr;

        if (i == 0) {
            statMinRate = statMaxRate = rate;
        } else {
            statMinRate = std::min(statMinRate, rate);
            statMaxRate = std::max(statMaxRate, rate);
        }
        const double actualRateStep = rate - previousRate;
        statMaxRateStep = std::max(statMaxRateStep, std::abs(actualRateStep));
        statMaxRateStepDelta = std::max(
            statMaxRateStepDelta, std::abs(actualRateStep - m_previousRateStep));
        m_previousRateStep = actualRateStep;
        previousRate = rate;

        m_readPos = wrapPosition(m_readPos);
        writeScratchOutput(out0, out1, i,
                           windowReady && positionInWindow(m_readPos), rate);
        m_readPos += rate;

        if (!m_loopActive) {
            // The bound scales with whichever velocity is currently larger, the
            // commanded one or the head's own. Closing a gap and coasting a
            // decayed command both stay inside it, so the guard only engages on
            // a genuinely runaway position and never snaps ordinary movement.
            const double maxLeadSamples =
                std::max({std::abs(referenceVelocity), std::abs(m_trackVel),
                          m_leadSpeedEnvelope})
                    * maxLeadSeconds * kRunawayHorizonFactor
                + kRunawaySlackOutputSamples * normalizedRateScale;
            const double lead = m_readPos - target;
            if (lead > maxLeadSamples) {
                m_readPos = target + maxLeadSamples;
                m_trackVel = std::min(m_trackVel, referenceVelocity);
                leadLimited = true;
            } else if (lead < -maxLeadSamples) {
                m_readPos = target - maxLeadSamples;
                m_trackVel = std::max(m_trackVel, referenceVelocity);
                leadLimited = true;
            }
        }
    }

    m_referencePos = referenceEndPos;
    m_referenceVelocity = referenceEndVelocity;

    m_statMinRate.store(statMinRate, std::memory_order_relaxed);
    m_statMaxRate.store(statMaxRate, std::memory_order_relaxed);
    m_statMaxRateStep.store(statMaxRateStep, std::memory_order_relaxed);
    m_statMaxRateStepDelta.store(statMaxRateStepDelta, std::memory_order_relaxed);
    m_statTrackingError.store(target - m_readPos, std::memory_order_relaxed);
    m_statLead.store(m_readPos - target, std::memory_order_relaxed);
    if (leadLimited)
        m_statLeadLimitedBlocks.fetch_add(1, std::memory_order_relaxed);

    m_smoothedRate = m_trackVel * dt;
    m_lastRate = m_smoothedRate;
    m_readPos = wrapPosition(m_readPos);
    return m_smoothedRate;
}

} // namespace engine::audio
