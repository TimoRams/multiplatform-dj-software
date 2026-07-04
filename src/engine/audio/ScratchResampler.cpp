#include "ScratchResampler.hpp"

#include "HermiteKernel.hpp"

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

bool ScratchResampler::tryPrimeWindowFromDisk(int outputBlockSize) noexcept
{
    if (!m_reader || m_sourceSize >= kMinWindowSamples)
        return m_sourceSize >= kMinWindowSamples;
    return reloadWindowFromDisk(0.0, outputBlockSize);
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

bool ScratchResampler::reloadWindowFromDisk(double rate, int outputBlockSize) noexcept
{
    if (!m_reader)
        return false;

    int lookBehind = 0;
    int lookAhead = 0;
    windowMargins(rate, outputBlockSize, lookBehind, lookAhead);

    const double blockSpan = std::max(std::abs(rate), std::abs(m_smoothedRate))
                           * static_cast<double>(std::max(m_deviceBufferSize, outputBlockSize));
    const int marginBehind = lookBehind + static_cast<int>(std::ceil(blockSpan));
    const int marginAhead = lookAhead + static_cast<int>(std::ceil(blockSpan));

    const juce::int64 trackEnd = std::max<juce::int64>(
        0, static_cast<juce::int64>(std::llround(m_trackLengthSamples)) - 1);

    const juce::int64 startSample = std::max<juce::int64>(
        0, static_cast<juce::int64>(std::floor(m_readPos)) - marginBehind);
    const juce::int64 endSample = std::min(
        trackEnd,
        static_cast<juce::int64>(std::ceil(m_readPos)) + marginAhead);

    const int numSamples = static_cast<int>(endSample - startSample + 1);
    const int capacity = m_sourceBuffer.getNumSamples();
    if (numSamples < kMinWindowSamples || numSamples > capacity)
        return false;

    if (!m_reader->read(&m_sourceBuffer, 0, numSamples, startSample, true, true)) {
        m_sourceSize = 0;
        return false;
    }

    m_bufferOriginSample = static_cast<double>(startSample);
    m_sourceSize = numSamples;
    return true;
}

void ScratchResampler::reloadWindowFromStream(juce::AudioSource& input,
                                              double rate,
                                              int outputBlockSize) noexcept
{
    if (!std::isfinite(m_readPos))
        m_readPos = 0.0;

    m_readPos = wrapPosition(m_readPos);

    int lookBehind = 0;
    int lookAhead = 0;
    windowMargins(rate, outputBlockSize, lookBehind, lookAhead);

    const int anchorSample = std::max(0, static_cast<int>(std::floor(m_readPos)) - lookBehind);

    if (auto* positionable = dynamic_cast<juce::PositionableAudioSource*>(&input)) {
        positionable->setNextReadPosition(static_cast<juce::int64>(anchorSample));
    }

    m_bufferOriginSample = static_cast<double>(anchorSample);
    m_sourceSize = 0;

    const int capacity = m_sourceBuffer.getNumSamples();
    const int targetFill = std::min(capacity - 1, lookBehind + lookAhead + m_deviceBufferSize);
    while (m_sourceSize < targetFill && m_sourceSize < capacity - m_deviceBufferSize) {
        const int pullCount = std::min(m_deviceBufferSize, capacity - m_sourceSize);
        if (pullCount <= 0)
            break;

        juce::AudioSourceChannelInfo pullInfo(&m_sourceBuffer, m_sourceSize, pullCount);
        input.getNextAudioBlock(pullInfo);
        m_sourceSize += pullCount;
    }
}

void ScratchResampler::ensureWindow(juce::AudioSource& input, double rate, int outputBlockSize) noexcept
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

    if (!needsWindowReload(clampedMin, clampedMax))
        return;

    if (m_reader != nullptr) {
        if (!reloadWindowFromDisk(rate, outputBlockSize))
            reloadWindowFromStream(input, rate, outputBlockSize);
        return;
    }

    reloadWindowFromStream(input, rate, outputBlockSize);
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

void ScratchResampler::processBlock(juce::AudioSource& input,
                                    double rate,
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

    ensureWindow(input, rate, numSamples);

    for (int i = 0; i < numSamples; ++i) {
        const double delta = rate - m_smoothedRate;
        const double absRate = std::abs(rate);
        const double slew = absRate < 0.12
            ? std::clamp(0.0008 + absRate * 0.004, 0.0008, 0.006)
            : std::clamp(0.004 + absRate * 0.008, 0.004, 0.030);
        m_smoothedRate += std::clamp(delta, -slew, slew);

        if (m_sourceSize < kMinWindowSamples || std::abs(m_smoothedRate) < 1e-7) {
            if (out0) out0[i] = 0.0f;
            if (out1) out1[i] = 0.0f;
            if (std::abs(m_smoothedRate) < 1e-7)
                continue;
        }

        m_readPos = wrapPosition(m_readPos);

        if (out0) out0[i] = readHermite(0, m_readPos);
        if (out1) out1[i] = readHermite(1, m_readPos);

        m_readPos += m_smoothedRate;
    }
}

double ScratchResampler::processScratchTracking(juce::AudioSource& input,
                                                double targetPosSamples,
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

    // Critically-damped (zeta = 1) second-order position tracker. The bandwidth
    // sets responsiveness: high enough to feel precise, low enough to reject the
    // step-jitter of discrete UI events. Explicit Euler is stable since w*dt << 1.
    constexpr double kTrackHz = 52.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    const double omega = kTwoPi * kTrackHz;
    const double maxVel = absMaxRate * outSr;

    // Window must span the path the read head will travel this block.
    const double estRate = std::clamp(
        std::max(std::abs((target - m_readPos) / static_cast<double>(std::max(1, numSamples))),
                 std::abs(m_trackVel) * dt) * 1.5,
        0.0, absMaxRate);
    ensureWindow(input, estRate, numSamples);

    const int outChannels = std::min(output.buffer->getNumChannels(), m_channels);
    float* out0 = outChannels > 0 ? output.buffer->getWritePointer(0, start) : nullptr;
    float* out1 = outChannels > 1 ? output.buffer->getWritePointer(1, start) : nullptr;

    const bool haveWindow = m_sourceSize >= kMinWindowSamples;
    if (!haveWindow)
        output.clearActiveBufferRegion();

    for (int i = 0; i < numSamples; ++i) {
        const double err = target - m_readPos;
        const double accel = omega * omega * err - 2.0 * omega * m_trackVel;
        m_trackVel = std::clamp(m_trackVel + accel * dt, -maxVel, maxVel);

        const double rate = std::clamp(m_trackVel * dt, -absMaxRate, absMaxRate);

        m_readPos = wrapPosition(m_readPos);
        if (haveWindow) {
            if (out0) out0[i] = readHermite(0, m_readPos);
            if (out1) out1[i] = readHermite(1, m_readPos);
        }
        m_readPos += rate;
    }

    m_smoothedRate = m_trackVel * dt;
    m_lastRate = m_smoothedRate;
    return m_smoothedRate;
}

} // namespace engine::audio
