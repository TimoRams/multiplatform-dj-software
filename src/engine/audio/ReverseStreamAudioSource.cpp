#include "ReverseStreamAudioSource.h"

#include <algorithm>

ReverseStreamAudioSource::ReverseStreamAudioSource(juce::PositionableAudioSource* forwardSource,
                                                   juce::PositionableAudioSource* directSource)
    : m_forwardSource(forwardSource),
      m_directSource(directSource != nullptr ? directSource : forwardSource) {}

void ReverseStreamAudioSource::setLoopRangeSamples(juce::int64 loopInSample,
                                                   juce::int64 loopOutSample,
                                                   double sampleRate)
{
    const juce::SpinLock::ScopedLockType lock(m_stateLock);
    auto* source = randomAccessSource();
    if (!source)
        return;

    m_sampleRate = sampleRate > 1.0 ? sampleRate : 44100.0;
    m_windowSamples = std::clamp(static_cast<int>(std::lround(m_sampleRate * 0.002)), 16, 256);

    const juce::int64 total = std::max<juce::int64>(0, source->getTotalLength());
    if (total <= 2) {
        m_loopEnabled = false;
        return;
    }

    juce::int64 in = std::clamp(loopInSample, static_cast<juce::int64>(0), total - 2);
    juce::int64 out = std::clamp(loopOutSample, static_cast<juce::int64>(1), total - 1);

    const juce::int64 minLen = std::max<juce::int64>(8, m_windowSamples * 2);
    if (out <= in + minLen)
        out = std::min(total - 1, in + minLen);
    if (out <= in + 2) {
        m_loopEnabled = false;
        return;
    }

    const int searchRadius = std::clamp(static_cast<int>(std::lround(m_sampleRate * 0.0015)), 8, 512);
    const juce::int64 snappedIn = findNearestZeroCrossingUnsafe(in, searchRadius, total);
    const juce::int64 snappedOut = findNearestZeroCrossingUnsafe(out, searchRadius, total);

    in = std::clamp(snappedIn, static_cast<juce::int64>(0), total - 2);
    out = std::clamp(snappedOut, in + 1, total - 1);
    if (out <= in + minLen)
        out = std::min(total - 1, in + minLen);
    if (out <= in + 2) {
        m_loopEnabled = false;
        return;
    }

    m_loopInSample = in;
    m_loopOutSample = out;
    m_loopEnabled = true;
    m_pendingFadeInSamples = 0;

    if (m_logicalPos >= m_loopOutSample)
        m_logicalPos = m_loopInSample;
}

void ReverseStreamAudioSource::clearLoopRangeSamples()
{
    const juce::SpinLock::ScopedLockType lock(m_stateLock);
    m_loopEnabled = false;
    m_pendingFadeInSamples = 0;
}

void ReverseStreamAudioSource::setNextReadPosition(juce::int64 newPosition)
{
    const juce::SpinLock::ScopedLockType lock(m_stateLock);
    if (m_forwardSource)
        m_forwardSource->setNextReadPosition(newPosition);
    if (m_directSource && m_directSource != m_forwardSource)
        m_directSource->setNextReadPosition(newPosition);
    m_logicalPos = newPosition;
}

juce::int64 ReverseStreamAudioSource::getNextReadPosition() const
{
    return m_logicalPos;
}

juce::int64 ReverseStreamAudioSource::getTotalLength() const
{
    auto* source = randomAccessSource();
    return source ? source->getTotalLength() : 0;
}

bool ReverseStreamAudioSource::isLooping() const
{
    return m_loopEnabled;
}

void ReverseStreamAudioSource::setLooping(bool shouldLoop)
{
    if (m_forwardSource)
        m_forwardSource->setLooping(shouldLoop);
    if (m_directSource && m_directSource != m_forwardSource)
        m_directSource->setLooping(shouldLoop);
}

void ReverseStreamAudioSource::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    const juce::SpinLock::ScopedLockType lock(m_stateLock);
    if (m_forwardSource)
        m_forwardSource->prepareToPlay(samplesPerBlockExpected, sampleRate);
    if (m_directSource && m_directSource != m_forwardSource)
        m_directSource->prepareToPlay(samplesPerBlockExpected, sampleRate);
    m_sampleRate = sampleRate > 1.0 ? sampleRate : 44100.0;
    m_windowSamples = std::clamp(static_cast<int>(std::lround(m_sampleRate * 0.002)), 16, 256);
    m_pendingFadeInSamples = 0;
    m_pendingDirectionFadeInSamples = 0;
}

void ReverseStreamAudioSource::releaseResources()
{
    const juce::SpinLock::ScopedLockType lock(m_stateLock);
    if (m_forwardSource)
        m_forwardSource->releaseResources();
    if (m_directSource && m_directSource != m_forwardSource)
        m_directSource->releaseResources();
}

void ReverseStreamAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    const juce::SpinLock::ScopedLockType lock(m_stateLock);

    if (m_loopEnabled && !m_reverse.load(std::memory_order_relaxed)) {
        getLoopedForwardAudioBlock(bufferToFill);
        applyDirectionFadeInToRange(bufferToFill.buffer,
                                    bufferToFill.startSample,
                                    bufferToFill.numSamples,
                                    bufferToFill.buffer ? bufferToFill.buffer->getNumChannels() : 0);
        return;
    }

    if (!m_reverse.load(std::memory_order_relaxed)) {
        auto* source = forwardPlaybackSource();
        if (!source) {
            bufferToFill.clearActiveBufferRegion();
            return;
        }
        source->setNextReadPosition(m_logicalPos);
        source->getNextAudioBlock(bufferToFill);
        m_logicalPos = source->getNextReadPosition();
        applyDirectionFadeInToRange(bufferToFill.buffer,
                                    bufferToFill.startSample,
                                    bufferToFill.numSamples,
                                    bufferToFill.buffer ? bufferToFill.buffer->getNumChannels() : 0);
        return;
    }

    auto* source = randomAccessSource();
    if (!source) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    const int numSamples = bufferToFill.numSamples;
    juce::int64 currentPos = m_logicalPos;

    juce::int64 readStart = currentPos - numSamples;
    int samplesToRead = numSamples;
    int zerosToPad = 0;

    if (readStart < 0) {
        samplesToRead = static_cast<int>(currentPos);
        zerosToPad = numSamples - samplesToRead;
        readStart = 0;
    }

    if (samplesToRead > 0) {
        source->setNextReadPosition(readStart);

        juce::AudioSourceChannelInfo readInfo(bufferToFill);
        readInfo.startSample = bufferToFill.startSample;
        readInfo.numSamples = samplesToRead;
        source->getNextAudioBlock(readInfo);

        for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
            float* ptr = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
            std::reverse(ptr, ptr + samplesToRead);
        }

        if (zerosToPad > 0) {
            applyFadeOutToTail(bufferToFill.buffer,
                               bufferToFill.startSample,
                               samplesToRead,
                               bufferToFill.buffer->getNumChannels());
        }
    }

    if (zerosToPad > 0) {
        for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
            juce::FloatVectorOperations::clear(
                bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample + samplesToRead),
                zerosToPad);
        }
    }

    m_logicalPos = currentPos - samplesToRead;
    if (m_logicalPos < 0)
        m_logicalPos = 0;

    applyDirectionFadeInToRange(bufferToFill.buffer,
                                bufferToFill.startSample,
                                bufferToFill.numSamples,
                                bufferToFill.buffer ? bufferToFill.buffer->getNumChannels() : 0);
}

void ReverseStreamAudioSource::setReverse(bool rev)
{
    const juce::SpinLock::ScopedLockType lock(m_stateLock);
    const bool prev = m_reverse.load(std::memory_order_relaxed);
    if (prev == rev)
        return;

    m_reverse.store(rev, std::memory_order_relaxed);
    m_pendingDirectionFadeInSamples = std::max(16, m_windowSamples);
}

juce::PositionableAudioSource* ReverseStreamAudioSource::forwardPlaybackSource() const
{
    return m_forwardSource != nullptr ? m_forwardSource : m_directSource;
}

juce::PositionableAudioSource* ReverseStreamAudioSource::randomAccessSource() const
{
    return m_directSource != nullptr ? m_directSource : m_forwardSource;
}

void ReverseStreamAudioSource::applyFadeInToRange(juce::AudioBuffer<float>* buffer,
                                                  int startSample,
                                                  int count,
                                                  int numChannels)
{
    if (!buffer || count <= 0 || m_windowSamples <= 0 || m_pendingFadeInSamples <= 0)
        return;

    const int applyCount = std::min(count, m_pendingFadeInSamples);
    const int startPhase = m_windowSamples - m_pendingFadeInSamples;
    for (int i = 0; i < applyCount; ++i) {
        const float g = std::clamp(static_cast<float>(startPhase + i + 1)
                                 / static_cast<float>(m_windowSamples),
                                   0.0f, 1.0f);
        for (int ch = 0; ch < numChannels; ++ch) {
            float* w = buffer->getWritePointer(ch, startSample + i);
            *w *= g;
        }
    }
    m_pendingFadeInSamples -= applyCount;
}

void ReverseStreamAudioSource::applyFadeOutToTail(juce::AudioBuffer<float>* buffer,
                                                    int chunkStart,
                                                    int chunkLen,
                                                    int numChannels)
{
    if (!buffer || chunkLen <= 0 || m_windowSamples <= 0)
        return;

    const int fadeLen = std::min(m_windowSamples, chunkLen);
    const int fadeStart = chunkStart + chunkLen - fadeLen;
    for (int i = 0; i < fadeLen; ++i) {
        const float g = static_cast<float>(fadeLen - i)
                      / static_cast<float>(fadeLen + 1);
        for (int ch = 0; ch < numChannels; ++ch) {
            float* w = buffer->getWritePointer(ch, fadeStart + i);
            *w *= g;
        }
    }
}

void ReverseStreamAudioSource::applyDirectionFadeInToRange(juce::AudioBuffer<float>* buffer,
                                                           int startSample,
                                                           int count,
                                                           int numChannels)
{
    if (!buffer || count <= 0 || m_windowSamples <= 0 || m_pendingDirectionFadeInSamples <= 0)
        return;

    const int applyCount = std::min(count, m_pendingDirectionFadeInSamples);
    const int startPhase = m_windowSamples - m_pendingDirectionFadeInSamples;
    for (int i = 0; i < applyCount; ++i) {
        const float g = std::clamp(static_cast<float>(startPhase + i + 1)
                                 / static_cast<float>(m_windowSamples),
                                   0.0f, 1.0f);
        for (int ch = 0; ch < numChannels; ++ch) {
            float* w = buffer->getWritePointer(ch, startSample + i);
            *w *= g;
        }
    }
    m_pendingDirectionFadeInSamples -= applyCount;
}

void ReverseStreamAudioSource::getLoopedForwardAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    auto* out = bufferToFill.buffer;
    auto* source = randomAccessSource();
    if (!out || bufferToFill.numSamples <= 0 || !source) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    const int numChannels = out->getNumChannels();
    int remaining = bufferToFill.numSamples;
    int destOffset = 0;

    while (remaining > 0) {
        if (m_logicalPos >= m_loopOutSample) {
            m_logicalPos = m_loopInSample;
            continue;
        }

        if (m_logicalPos < m_loopInSample) {
            const int toEntry = static_cast<int>(
                std::min<juce::int64>(remaining, m_loopInSample - m_logicalPos));
            juce::AudioSourceChannelInfo readInfo(bufferToFill);
            readInfo.startSample = bufferToFill.startSample + destOffset;
            readInfo.numSamples = toEntry;
            source->setNextReadPosition(m_logicalPos);
            source->getNextAudioBlock(readInfo);
            m_logicalPos += toEntry;
            destOffset += toEntry;
            remaining -= toEntry;
            continue;
        }

        const juce::int64 samplesToBoundary = m_loopOutSample - m_logicalPos;
        if (samplesToBoundary <= 0) {
            m_logicalPos = m_loopInSample;
            continue;
        }

        const int chunk = std::min<int>(remaining, static_cast<int>(samplesToBoundary));

        juce::AudioSourceChannelInfo readInfo(bufferToFill);
        readInfo.startSample = bufferToFill.startSample + destOffset;
        readInfo.numSamples = chunk;

        source->setNextReadPosition(m_logicalPos);
        source->getNextAudioBlock(readInfo);

        applyFadeInToRange(out, readInfo.startSample, chunk, numChannels);

        const bool hitsBoundary = (chunk == samplesToBoundary);
        if (hitsBoundary) {
            applyFadeOutToTail(out, readInfo.startSample, chunk, numChannels);
            m_logicalPos = m_loopInSample;
            m_pendingFadeInSamples = m_windowSamples;
        } else {
            m_logicalPos += chunk;
        }

        destOffset += chunk;
        remaining -= chunk;
    }
}

juce::int64 ReverseStreamAudioSource::findNearestZeroCrossingUnsafe(juce::int64 approx,
                                                                    int radius,
                                                                    juce::int64 totalLength)
{
    auto* source = randomAccessSource();
    if (!source || totalLength <= 2)
        return approx;

    const juce::int64 start = std::clamp(approx - static_cast<juce::int64>(radius),
                                         static_cast<juce::int64>(0),
                                         totalLength - 2);
    const juce::int64 end = std::clamp(approx + static_cast<juce::int64>(radius),
                                       static_cast<juce::int64>(1),
                                       totalLength - 1);
    const int count = static_cast<int>(std::max<juce::int64>(0, end - start + 1));
    if (count < 2)
        return std::clamp(approx, static_cast<juce::int64>(0), totalLength - 1);

    juce::AudioBuffer<float> probe(2, count);
    juce::AudioSourceChannelInfo info;
    info.buffer = &probe;
    info.startSample = 0;
    info.numSamples = count;
    probe.clear();

    const juce::int64 oldPos = source->getNextReadPosition();
    source->setNextReadPosition(start);
    source->getNextAudioBlock(info);
    source->setNextReadPosition(oldPos);

    const int ch1 = probe.getNumChannels() > 1 ? 1 : 0;
    const float* p0 = probe.getReadPointer(0);
    const float* p1 = probe.getReadPointer(ch1);

    juce::int64 bestSample = std::clamp(approx, start, end);
    juce::int64 bestDist = std::numeric_limits<juce::int64>::max();

    for (int i = 1; i < count; ++i) {
        const float prev = 0.5f * (p0[i - 1] + p1[i - 1]);
        const float curr = 0.5f * (p0[i] + p1[i]);
        const bool crosses = (prev <= 0.0f && curr >= 0.0f)
                          || (prev >= 0.0f && curr <= 0.0f);
        if (!crosses)
            continue;

        const juce::int64 sampleIdx = start + i;
        const juce::int64 dist = std::llabs(sampleIdx - approx);
        if (dist < bestDist) {
            bestDist = dist;
            bestSample = sampleIdx;
            if (dist == 0)
                break;
        }
    }

    if (bestDist == std::numeric_limits<juce::int64>::max()) {
        float bestAbs = std::numeric_limits<float>::max();
        for (int i = 0; i < count; ++i) {
            const float s = 0.5f * (p0[i] + p1[i]);
            const float a = std::abs(s);
            if (a < bestAbs) {
                bestAbs = a;
                bestSample = start + i;
            }
        }
    }

    return std::clamp(bestSample, static_cast<juce::int64>(0), totalLength - 1);
}
