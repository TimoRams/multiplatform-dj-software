#include "CachedPlaybackAudioSource.h"

#include <algorithm>
#include <cmath>

CachedPlaybackAudioSource::CachedPlaybackAudioSource(AudioPageCache& cache,
                                                     AudioCacheHandle handle) noexcept
    : m_cache(cache), m_handle(handle)
{
}

void CachedPlaybackAudioSource::setNextReadPosition(juce::int64 position)
{
    m_position.store(std::clamp<juce::int64>(position, 0, getTotalLength()), std::memory_order_release);
}
juce::int64 CachedPlaybackAudioSource::getNextReadPosition() const { return m_position.load(std::memory_order_acquire); }
juce::int64 CachedPlaybackAudioSource::getTotalLength() const { return std::max<juce::int64>(0, m_handle.lengthInSamples()); }
bool CachedPlaybackAudioSource::isLooping() const { return m_loopEnabled.load(std::memory_order_acquire); }
void CachedPlaybackAudioSource::setLooping(bool enabled) { m_loopEnabled.store(enabled, std::memory_order_release); }

void CachedPlaybackAudioSource::setLoopRangeSamples(juce::int64 in, juce::int64 out, double) noexcept
{
    if (in < 0 || out <= in) { clearLoopRangeSamples(); return; }
    m_loopIn.store(in, std::memory_order_relaxed);
    m_loopOut.store(out, std::memory_order_relaxed);
    m_loopEnabled.store(true, std::memory_order_release);
}
void CachedPlaybackAudioSource::clearLoopRangeSamples() noexcept { m_loopEnabled.store(false, std::memory_order_release); }

float CachedPlaybackAudioSource::nextGain(bool ready) noexcept
{
    const float step = 1.0f / kFadeSamples;
    if (ready) {
        if (m_starvationGain <= 0.0f) m_recovery.fetch_add(1, std::memory_order_relaxed);
        m_starvationGain = std::min(1.0f, m_starvationGain + step);
    } else m_starvationGain = std::max(0.0f, m_starvationGain - step);
    return m_starvationGain;
}

void CachedPlaybackAudioSource::requestReadAhead(juce::int64 position, int blockSize, bool reverse) noexcept
{
    const auto current = AudioPage::pageIndexForSample(std::max<juce::int64>(0, position));
    if (current < 0) return;
    const int speedPages = std::clamp(2 + blockSize / static_cast<int>(AudioPage::kSamplesPerChannel), 2, 8);
    const auto first = std::max<juce::int64>(0, current - (reverse ? speedPages : 1));
    const auto last = std::min<juce::int64>(m_handle.pageCount() - 1, current + (reverse ? 1 : speedPages));
    for (auto page = first; page <= last; ++page) {
        const auto priority = page == current ? AudioCachePriority::RealtimeCritical
                                               : AudioCachePriority::PlaybackReadAhead;
        if (!m_cache.requestPage(m_handle, page, priority)) m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void CachedPlaybackAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (!info.buffer || info.numSamples <= 0 || !m_handle.isValid()) {
        info.clearActiveBufferRegion(); return;
    }
    const int channels = std::min(2, info.buffer->getNumChannels());
    float* outL = channels > 0 ? info.buffer->getWritePointer(0, info.startSample) : nullptr;
    float* outR = channels > 1 ? info.buffer->getWritePointer(1, info.startSample) : nullptr;
    auto position = m_position.load(std::memory_order_relaxed);
    const bool reverse = m_reverse.load(std::memory_order_acquire);
    const bool loop = m_loopEnabled.load(std::memory_order_acquire);
    const auto loopIn = m_loopIn.load(std::memory_order_relaxed);
    const auto loopOut = m_loopOut.load(std::memory_order_relaxed);
    requestReadAhead(position, info.numSamples, reverse);
    bool missed = false;
    std::int64_t pinnedIndex = -1;
    AudioPageReadGuard page;
    for (int i = 0; i < info.numSamples; ++i) {
        if (loop && loopOut > loopIn) {
            if (!reverse && position >= loopOut) position = loopIn;
            if (reverse && position < loopIn) position = loopOut - 1;
        }
        const bool inRange = position >= 0 && position < getTotalLength();
        const auto pageIndex = inRange ? AudioPage::pageIndexForSample(position) : -1;
        if (pageIndex != pinnedIndex) {
            page = {};
            pinnedIndex = pageIndex;
            if (pageIndex >= 0) page = m_cache.tryGetPage(m_handle, pageIndex);
        }
        bool ready = static_cast<bool>(page);
        if (ready && (page->trackId != m_handle.id() || page->generation != m_handle.generation())) {
            ready = false; m_generationMismatch.fetch_add(1, std::memory_order_relaxed);
        }
        if (ready) {
            const auto offset = static_cast<int>(position - page->firstSample);
            ready = offset >= 0 && offset < static_cast<int>(page->validSampleCount);
            if (ready) {
                m_lastL = page->channelData(0)[offset];
                const unsigned right = page->channelCount > 1 ? 1u : 0u;
                m_lastR = page->channelData(right)[offset];
                m_hits.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!ready) {
            missed = true; m_misses.fetch_add(1, std::memory_order_relaxed);
            if (pageIndex >= 0 && !m_cache.requestPage(m_handle, pageIndex, AudioCachePriority::RealtimeCritical))
                m_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        const float gain = nextGain(ready);
        if (outL) outL[i] = m_lastL * gain;
        if (outR) outR[i] = m_lastR * gain;
        position += reverse ? -1 : 1; // timeline advances even while starved
        if (!loop) position = std::clamp<juce::int64>(position, 0, getTotalLength());
    }
    if (missed) m_starvation.fetch_add(1, std::memory_order_relaxed);
    m_position.store(position, std::memory_order_release);
}

PlaybackCacheStats CachedPlaybackAudioSource::cacheStats() const noexcept
{
    return {m_hits.load(), m_misses.load(), m_starvation.load(), m_recovery.load(),
            m_dropped.load(), m_generationMismatch.load(), 0, 0};
}
