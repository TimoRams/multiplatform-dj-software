#include "AudioPageCache.h"
#include "AudioCacheWorker.h"

#include <QFileInfo>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct PageRequest {
    const void* entry = nullptr;
    std::uint64_t trackId = 0;
    std::uint64_t generation = 0;
    std::int64_t pageIndex = 0;
    std::uint64_t queuedAtMicros = 0;
};

std::uint64_t steadyMicros() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void updateMaximum(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept
{
    auto previous = target.load(std::memory_order_relaxed);
    while (previous < value
           && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
}

template <size_t Capacity>
class BoundedQueue {
    struct Slot { std::atomic<size_t> sequence{0}; PageRequest value; };
public:
    BoundedQueue() { for (size_t i = 0; i < Capacity; ++i) m_slots[i].sequence.store(i); }
    bool tryPush(PageRequest value) noexcept
    {
        size_t pos = m_enqueue.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = m_slots[pos % Capacity];
            const size_t seq = slot.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (m_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.value = value;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) return false;
            else pos = m_enqueue.load(std::memory_order_relaxed);
        }
    }
    bool tryPop(PageRequest& value) noexcept
    {
        size_t pos = m_dequeue.load(std::memory_order_relaxed);
        Slot& slot = m_slots[pos % Capacity];
        if (slot.sequence.load(std::memory_order_acquire) != pos + 1) return false;
        m_dequeue.store(pos + 1, std::memory_order_relaxed);
        value = slot.value;
        slot.sequence.store(pos + Capacity, std::memory_order_release);
        return true;
    }
    [[nodiscard]] size_t approximateSize() const noexcept
    {
        const auto queued = m_enqueue.load(std::memory_order_relaxed);
        const auto consumed = m_dequeue.load(std::memory_order_relaxed);
        return std::min(Capacity, queued >= consumed ? queued - consumed : size_t{0});
    }
private:
    std::array<Slot, Capacity> m_slots;
    alignas(64) std::atomic<size_t> m_enqueue{0};
    alignas(64) std::atomic<size_t> m_dequeue{0};
};

std::string mapKey(const AudioCacheKey& key)
{
    return key.canonicalPath.toStdString() + '|' + std::to_string(key.fileSize)
        + '|' + std::to_string(key.lastModifiedMs);
}
}

struct AudioPageCache::Impl {
    struct PageSlot {
        std::atomic<AudioPage*> page{nullptr};
        std::atomic<std::uint32_t> readers{0};
        std::atomic<std::uint8_t> state{0}; // 0 missing, 1 queued, 2 resident, 3 decoding
        std::atomic<bool> recentlyUsed{false};
        // These links are owned exclusively by the decoder worker.  PageSlot
        // addresses are stable for the lifetime of the cache entry.
        PageSlot* evictionPrevious = nullptr;
        PageSlot* evictionNext = nullptr;
        bool onEvictionClock = false;
    };
    struct Entry {
        AudioCacheKey key;
        std::uint64_t id = 0;
        std::atomic<std::uint64_t> generation{1};
        std::atomic<std::uint32_t> users{0};
        std::atomic<bool> active{true};
        double sampleRate = 0.0;
        std::int64_t length = 0;
        int channels = 0;
        // AudioFormatReader owns the backing file handle. Explicit device eject
        // must be able to close it after the last cache user leaves, while a
        // decoder request may still be finishing on the worker thread.
        std::mutex readerMutex;
        std::unique_ptr<juce::AudioFormatReader> reader;
        std::unique_ptr<PageSlot[]> pageSlots;
        std::int64_t pageCount = 0;
    };

    juce::AudioFormatManager formats;
    std::mutex controlMutex;
    std::unordered_map<std::string, Entry*> activeEntries;
    std::vector<std::unique_ptr<Entry>> entries; // stable addresses until cache shutdown
    struct RetiredPage {
        PageSlot* slot = nullptr;
        AudioPage* page = nullptr;
        std::uint64_t retiredAtMicros = 0;
    };
    std::vector<RetiredPage> retiredPages; // worker-owned deferred deletes
    PageSlot* evictionClockHand = nullptr;
    size_t evictionClockSize = 0;
    std::array<BoundedQueue<AudioPageCache::kRequestQueueCapacity>,
               static_cast<size_t>(AudioCachePriority::Count)> queues;
    std::condition_variable condition;
    std::mutex conditionMutex;
    std::condition_variable pageReadyCondition;
    std::mutex pageReadyMutex;
    std::atomic<std::uint64_t> nextId{1};
    std::atomic<std::uint64_t> hits{0}, misses{0}, queued{0}, dropped{0};
    std::atomic<std::uint64_t> decoded{0}, failures{0}, evicted{0}, resident{0}, open{0};
    std::atomic<std::uint64_t> peakPending{0}, workerRequests{0}, workerLatency{0}, worstWorkerLatency{0};
    std::atomic<std::uint64_t> decodeMicros{0}, worstDecodeMicros{0};
    std::atomic<std::uint64_t> evictionScans{0}, evictionCandidates{0};
    std::atomic<std::uint64_t> evictionMicros{0}, worstEvictionMicros{0};
    std::atomic<std::uint64_t> readerWaitMicros{0}, worstReaderWaitMicros{0};
    std::atomic<bool> accepting{true};
};

AudioPageReadGuard::AudioPageReadGuard(AudioPageReadGuard&& other) noexcept
    : m_page(other.m_page), m_readers(other.m_readers)
{
    other.m_page = nullptr; other.m_readers = nullptr;
}
AudioPageReadGuard& AudioPageReadGuard::operator=(AudioPageReadGuard&& other) noexcept
{
    if (this != &other) { reset(); m_page = other.m_page; m_readers = other.m_readers;
        other.m_page = nullptr; other.m_readers = nullptr; }
    return *this;
}
AudioPageReadGuard::~AudioPageReadGuard() { reset(); }
void AudioPageReadGuard::reset() noexcept
{
    if (m_readers) m_readers->fetch_sub(1, std::memory_order_release);
    m_page = nullptr; m_readers = nullptr;
}

AudioPageCache::AudioPageCache(std::uint64_t budgetBytes)
    : m_impl(std::make_unique<Impl>()), m_budgetBytes(std::max<std::uint64_t>(budgetBytes, 1))
{
    m_impl->formats.registerBasicFormats();
    m_worker = std::make_unique<AudioCacheWorker>(*this);
}
AudioPageCache::~AudioPageCache() { shutdownAndJoin(); }

AudioCacheHandle AudioPageCache::openTrack(const TrackCacheOpenRequest& request)
{
    return openTrack(request, {});
}

AudioCacheHandle AudioPageCache::openTrack(
    const TrackCacheOpenRequest& request,
    std::unique_ptr<juce::AudioFormatReader> preparedReader)
{
    AudioCacheHandle handle;
    if (!m_impl->accepting.load(std::memory_order_acquire)) return handle;
    const QFileInfo info(request.filePath);
    if (!info.exists() || !info.isFile()) return handle;
    AudioCacheKey key{info.canonicalFilePath(), static_cast<std::uint64_t>(info.size()),
                      info.lastModified().toMSecsSinceEpoch()};
    if (key.canonicalPath.isEmpty()) key.canonicalPath = info.absoluteFilePath();
    const auto lookup = mapKey(key);
    std::lock_guard lock(m_impl->controlMutex);
    if (auto it = m_impl->activeEntries.find(lookup); it != m_impl->activeEntries.end()) {
        auto* entry = it->second;
        entry->users.fetch_add(1, std::memory_order_relaxed);
        handle.m_token = entry; handle.m_trackId = entry->id;
        handle.m_generation = entry->generation.load(); handle.m_sampleRate = entry->sampleRate;
        handle.m_lengthInSamples = entry->length; handle.m_channelCount = entry->channels;
        return handle;
    }
    auto entry = std::make_unique<Impl::Entry>();
    entry->key = key;
    entry->reader = preparedReader
        ? std::move(preparedReader)
        : std::unique_ptr<juce::AudioFormatReader>(
            m_impl->formats.createReaderFor(juce::File(key.canonicalPath.toStdString())));
    if (!entry->reader || entry->reader->sampleRate <= 0.0 || entry->reader->numChannels == 0
        || entry->reader->numChannels > 8 || entry->reader->lengthInSamples <= 0) return handle;
    entry->id = m_impl->nextId.fetch_add(1);
    entry->sampleRate = entry->reader->sampleRate;
    entry->length = entry->reader->lengthInSamples;
    entry->channels = static_cast<int>(entry->reader->numChannels);
    entry->pageCount = (entry->length + AudioPage::kSamplesPerChannel - 1) / AudioPage::kSamplesPerChannel;
    entry->pageSlots = std::make_unique<Impl::PageSlot[]>(static_cast<size_t>(entry->pageCount));
    entry->users.store(1);
    auto* raw = entry.get();
    m_impl->entries.push_back(std::move(entry));
    m_impl->activeEntries.emplace(lookup, raw);
    m_impl->open.fetch_add(1);
    handle.m_token = raw; handle.m_trackId = raw->id; handle.m_generation = 1;
    handle.m_sampleRate = raw->sampleRate; handle.m_lengthInSamples = raw->length;
    handle.m_channelCount = raw->channels;
    return handle;
}

void AudioPageCache::releaseTrack(const AudioCacheHandle& handle)
{
    if (!handle.isValid()) return;
    auto* entry = const_cast<Impl::Entry*>(static_cast<const Impl::Entry*>(handle.m_token));
    std::lock_guard lock(m_impl->controlMutex);
    if (entry->id != handle.id() || entry->generation.load() != handle.generation()) return;
    const auto old = entry->users.fetch_sub(1);
    if (old > 1) return;
    entry->active.store(false, std::memory_order_release);
    entry->generation.fetch_add(1, std::memory_order_acq_rel);
    m_impl->activeEntries.erase(mapKey(entry->key));
    m_impl->open.fetch_sub(1);
    {
        // The cache keeps Entry addresses stable, but an inactive entry must
        // not keep removable media mounted. The decoder takes the same lock
        // around its only AudioFormatReader access and rechecks generation.
        std::lock_guard readerLock(entry->readerMutex);
        entry->reader.reset();
    }
    notifyWorker();
}

AudioPageReadGuard AudioPageCache::tryGetPage(const AudioCacheHandle& handle,
                                               std::int64_t pageIndex) const noexcept
{
    if (!handle.isValid() || pageIndex < 0) { m_impl->misses.fetch_add(1); return {}; }
    auto* entry = static_cast<const Impl::Entry*>(handle.m_token);
    if (entry->id != handle.id() || entry->generation.load(std::memory_order_acquire) != handle.generation()
        || !entry->active.load(std::memory_order_acquire) || pageIndex >= entry->pageCount) {
        m_impl->misses.fetch_add(1); return {};
    }
    auto& slot = entry->pageSlots[static_cast<size_t>(pageIndex)];
    slot.readers.fetch_add(1, std::memory_order_acquire);
    auto* page = slot.page.load(std::memory_order_acquire);
    if (!page || page->generation != handle.generation()) {
        slot.readers.fetch_sub(1, std::memory_order_release);
        m_impl->misses.fetch_add(1, std::memory_order_relaxed); return {};
    }
    slot.recentlyUsed.store(true, std::memory_order_relaxed);
    m_impl->hits.fetch_add(1, std::memory_order_relaxed);
    return AudioPageReadGuard(page, &slot.readers);
}

bool AudioPageCache::requestPage(const AudioCacheHandle& handle, std::int64_t pageIndex,
                                  AudioCachePriority priority) noexcept
{
    if (!m_impl->accepting.load(std::memory_order_acquire) || !handle.isValid()
        || pageIndex < 0 || priority >= AudioCachePriority::Count) return false;
    auto* entry = const_cast<Impl::Entry*>(static_cast<const Impl::Entry*>(handle.m_token));
    if (entry->id != handle.id() || entry->generation.load(std::memory_order_acquire) != handle.generation()
        || !entry->active.load(std::memory_order_acquire) || pageIndex >= entry->pageCount) return false;
    auto& slot = entry->pageSlots[static_cast<size_t>(pageIndex)];
    std::uint8_t expected = 0;
    if (!slot.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return expected == 1 || expected == 2 || expected == 3;
    PageRequest request{entry, handle.id(), handle.generation(), pageIndex, steadyMicros()};
    if (!m_impl->queues[static_cast<size_t>(priority)].tryPush(request)) {
        slot.state.store(0, std::memory_order_release); m_impl->dropped.fetch_add(1); return false;
    }
    m_impl->queued.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t pending = 0;
    for (const auto& queue : m_impl->queues)
        pending += queue.approximateSize();
    updateMaximum(m_impl->peakPending, pending);
    return true; // worker polls; no condition-variable syscall on the RT producer
}

bool AudioPageCache::requestRange(const AudioCacheHandle& handle, std::int64_t first,
                                   std::int64_t last, AudioCachePriority priority) noexcept
{
    if (first < 0 || last < first) return false;
    bool all = true;
    for (auto page = first; page <= last; ++page) all = requestPage(handle, page, priority) && all;
    // Bulk requests originate outside the real-time callback. Wake the decoder
    // immediately so track-load prewarming does not wait for its polling tick.
    notifyWorker();
    return all;
}

bool AudioPageCache::waitForPageRange(
    const AudioCacheHandle& handle,
    std::int64_t firstPage,
    std::int64_t lastPage,
    std::chrono::milliseconds timeout,
    const std::function<bool()>& shouldCancel) const
{
    if (!handle.isValid() || firstPage < 0 || lastPage < firstPage
        || timeout < std::chrono::milliseconds::zero()) {
        return false;
    }
    auto* entry = static_cast<const Impl::Entry*>(handle.m_token);
    if (entry->id != handle.id()
        || entry->generation.load(std::memory_order_acquire)
            != handle.generation()
        || !entry->active.load(std::memory_order_acquire)
        || lastPage >= entry->pageCount) {
        return false;
    }
    const auto ready = [&]() {
        if (shouldCancel && shouldCancel())
            return true;
        if (!entry->active.load(std::memory_order_acquire)
            || entry->generation.load(std::memory_order_acquire)
                != handle.generation()) {
            return true;
        }
        for (auto page = firstPage; page <= lastPage; ++page) {
            const auto& slot = entry->pageSlots[static_cast<std::size_t>(page)];
            const auto* value = slot.page.load(std::memory_order_acquire);
            if (!value || value->generation != handle.generation())
                return false;
        }
        return true;
    };
    std::unique_lock lock(m_impl->pageReadyMutex);
    (void)m_impl->pageReadyCondition.wait_for(lock, timeout, ready);
    if (shouldCancel && shouldCancel())
        return false;
    if (!entry->active.load(std::memory_order_acquire)
        || entry->generation.load(std::memory_order_acquire)
            != handle.generation()) {
        return false;
    }
    for (auto page = firstPage; page <= lastPage; ++page) {
        const auto& slot = entry->pageSlots[static_cast<std::size_t>(page)];
        const auto* value = slot.page.load(std::memory_order_acquire);
        if (!value || value->generation != handle.generation())
            return false;
    }
    return true;
}

AudioCacheStats AudioPageCache::stats() const noexcept
{
    std::uint64_t pending = 0;
    for (const auto& queue : m_impl->queues)
        pending += queue.approximateSize();
    return {m_impl->hits.load(), m_impl->misses.load(), m_impl->queued.load(), m_impl->dropped.load(),
            m_impl->decoded.load(), m_impl->failures.load(), m_impl->evicted.load(),
            m_impl->resident.load(), m_impl->open.load(), pending, m_impl->peakPending.load(),
            m_impl->workerRequests.load(),
            m_impl->workerLatency.load(), m_impl->worstWorkerLatency.load(),
            m_impl->decodeMicros.load(), m_impl->worstDecodeMicros.load(),
            m_impl->evictionScans.load(), m_impl->evictionCandidates.load(),
            m_impl->evictionMicros.load(), m_impl->worstEvictionMicros.load(),
            m_impl->readerWaitMicros.load(), m_impl->worstReaderWaitMicros.load()};
}

void AudioPageCache::notifyWorker() noexcept { m_impl->condition.notify_one(); }

void AudioPageCache::workerRun(const std::atomic<bool>& shutdown)
{
    auto addToEvictionClock = [this](Impl::PageSlot& slot) {
        if (slot.onEvictionClock) return;
        if (!m_impl->evictionClockHand) {
            slot.evictionPrevious = &slot;
            slot.evictionNext = &slot;
            m_impl->evictionClockHand = &slot;
        } else {
            auto* const head = m_impl->evictionClockHand;
            auto* const tail = head->evictionPrevious;
            slot.evictionPrevious = tail;
            slot.evictionNext = head;
            tail->evictionNext = &slot;
            head->evictionPrevious = &slot;
        }
        slot.onEvictionClock = true;
        ++m_impl->evictionClockSize;
    };
    auto removeFromEvictionClock = [this](Impl::PageSlot& slot) {
        if (!slot.onEvictionClock) return;
        if (slot.evictionNext == &slot) {
            m_impl->evictionClockHand = nullptr;
        } else {
            slot.evictionPrevious->evictionNext = slot.evictionNext;
            slot.evictionNext->evictionPrevious = slot.evictionPrevious;
            if (m_impl->evictionClockHand == &slot)
                m_impl->evictionClockHand = slot.evictionNext;
        }
        slot.evictionPrevious = nullptr;
        slot.evictionNext = nullptr;
        slot.onEvictionClock = false;
        --m_impl->evictionClockSize;
    };
    auto reapRetiredPages = [this] {
        const auto now = steadyMicros();
        auto& retired = m_impl->retiredPages;
        for (size_t index = 0; index < retired.size();) {
            auto& candidate = retired[index];
            if (candidate.slot->readers.load(std::memory_order_acquire) != 0) {
                ++index;
                continue;
            }
            const auto waited = now - candidate.retiredAtMicros;
            m_impl->readerWaitMicros.fetch_add(waited, std::memory_order_relaxed);
            updateMaximum(m_impl->worstReaderWaitMicros, waited);
            delete candidate.page;
            candidate = retired.back();
            retired.pop_back();
        }
    };
    auto evictOne = [this, &removeFromEvictionClock] {
        const auto scanStartedAt = steadyMicros();
        m_impl->evictionScans.fetch_add(1, std::memory_order_relaxed);
        const size_t candidates = m_impl->evictionClockSize;
        // One turn clears second-chance bits; a second turn finds a victim.
        // This keeps a full cache immediately decodable after warm-up while
        // retaining O(1) amortized work across subsequent evictions.
        for (size_t attempt = 0; attempt < candidates * 2; ++attempt) {
            auto* const candidate = m_impl->evictionClockHand;
            m_impl->evictionClockHand = candidate->evictionNext;
            m_impl->evictionCandidates.fetch_add(1, std::memory_order_relaxed);
            if (candidate->readers.load(std::memory_order_acquire) != 0)
                continue;
            if (candidate->recentlyUsed.exchange(false, std::memory_order_acq_rel))
                continue;
            auto* const retired = candidate->page.exchange(nullptr, std::memory_order_acq_rel);
            if (!retired) {
                candidate->state.store(0, std::memory_order_release);
                removeFromEvictionClock(*candidate);
                continue;
            }
            candidate->state.store(0, std::memory_order_release);
            removeFromEvictionClock(*candidate);
            m_impl->resident.fetch_sub(retired->byteSize(), std::memory_order_relaxed);
            m_impl->retiredPages.push_back({candidate, retired, steadyMicros()});
            m_impl->evicted.fetch_add(1, std::memory_order_relaxed);
            const auto elapsed = steadyMicros() - scanStartedAt;
            m_impl->evictionMicros.fetch_add(elapsed, std::memory_order_relaxed);
            updateMaximum(m_impl->worstEvictionMicros, elapsed);
            return true;
        }
        const auto elapsed = steadyMicros() - scanStartedAt;
        m_impl->evictionMicros.fetch_add(elapsed, std::memory_order_relaxed);
        updateMaximum(m_impl->worstEvictionMicros, elapsed);
        return false;
    };

    unsigned fairness = 0;
    while (!shutdown.load(std::memory_order_acquire)) {
        reapRetiredPages();
        PageRequest request;
        bool found = false;
        const size_t start = (++fairness % 32 == 0) ? 2 : 0; // periodic lower-priority service
        for (size_t offset = 0; offset < static_cast<size_t>(AudioCachePriority::Count); ++offset) {
            const size_t queue = (start + offset) % static_cast<size_t>(AudioCachePriority::Count);
            if (m_impl->queues[queue].tryPop(request)) { found = true; break; }
        }
        if (!found) {
            std::unique_lock lock(m_impl->conditionMutex);
            m_impl->condition.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }
        const auto queueLatency = steadyMicros() - request.queuedAtMicros;
        m_impl->workerRequests.fetch_add(1, std::memory_order_relaxed);
        m_impl->workerLatency.fetch_add(queueLatency, std::memory_order_relaxed);
        updateMaximum(m_impl->worstWorkerLatency, queueLatency);
        auto* entry = const_cast<Impl::Entry*>(static_cast<const Impl::Entry*>(request.entry));
        auto& slot = entry->pageSlots[static_cast<size_t>(request.pageIndex)];
        if (!entry->active.load() || entry->id != request.trackId
            || entry->generation.load() != request.generation) { slot.state.store(0); continue; }
        slot.state.store(3, std::memory_order_release);
        auto page = std::make_unique<AudioPage>();
        page->trackId = entry->id; page->generation = request.generation;
        page->pageIndex = request.pageIndex;
        page->firstSample = AudioPage::firstSampleForPage(request.pageIndex);
        page->validSampleCount = static_cast<std::uint32_t>(std::min<std::int64_t>(
            AudioPage::kSamplesPerChannel, entry->length - page->firstSample));
        page->channelCount = static_cast<std::uint16_t>(entry->channels);
        page->planarPcm.resize(static_cast<size_t>(entry->channels * AudioPage::kSamplesPerChannel));
        juce::AudioBuffer<float> buffer(entry->channels, static_cast<int>(AudioPage::kSamplesPerChannel));
        buffer.clear();
        const auto decodeStartedAt = steadyMicros();
        bool ok = false;
        {
            std::lock_guard readerLock(entry->readerMutex);
            if (entry->active.load(std::memory_order_acquire)
                && entry->generation.load(std::memory_order_acquire) == request.generation
                && entry->reader) {
                ok = entry->reader->read(
                    &buffer, 0, static_cast<int>(page->validSampleCount),
                    page->firstSample, true, true);
            }
        }
        const auto decodeElapsed = steadyMicros() - decodeStartedAt;
        m_impl->decodeMicros.fetch_add(decodeElapsed, std::memory_order_relaxed);
        updateMaximum(m_impl->worstDecodeMicros, decodeElapsed);
        if (!ok) {
            slot.state.store(0);
            m_impl->failures.fetch_add(1);
            // Pair the state transition with the mutex used by blocking
            // loader-side waiters so a completion notification cannot be lost
            // between their predicate check and wait operation.
            const std::lock_guard readyLock(m_impl->pageReadyMutex);
            m_impl->pageReadyCondition.notify_all();
            continue;
        }
        for (int channel = 0; channel < entry->channels; ++channel)
            std::copy_n(buffer.getReadPointer(channel), AudioPage::kSamplesPerChannel,
                        page->planarPcm.data() + static_cast<size_t>(channel) * AudioPage::kSamplesPerChannel);
        const auto bytes = page->byteSize();
        while (m_impl->resident.load(std::memory_order_relaxed) + bytes > m_budgetBytes
               && evictOne()) {}
        if (m_impl->resident.load() + bytes > m_budgetBytes || !entry->active.load()
            || entry->generation.load() != request.generation) { slot.state.store(0); continue; }
        slot.recentlyUsed.store(true, std::memory_order_relaxed);
        slot.page.store(page.release(), std::memory_order_release);
        slot.state.store(2, std::memory_order_release);
        addToEvictionClock(slot);
        m_impl->resident.fetch_add(bytes); m_impl->decoded.fetch_add(1);
        const std::lock_guard readyLock(m_impl->pageReadyMutex);
        m_impl->pageReadyCondition.notify_all();
    }

    for (auto& retired : m_impl->retiredPages) {
        while (retired.slot->readers.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
        delete retired.page;
    }
    m_impl->retiredPages.clear();

    for (auto& entry : m_impl->entries)
        for (std::int64_t i = 0; i < entry->pageCount; ++i) {
            auto& slot = entry->pageSlots[static_cast<size_t>(i)];
            auto* retired = slot.page.exchange(nullptr);
            while (slot.readers.load() != 0) std::this_thread::yield();
            delete retired;
        }
}

void AudioPageCache::shutdownAndJoin() noexcept
{
    if (!m_impl || !m_impl->accepting.exchange(false)) return;
    if (m_worker) m_worker->shutdownAndJoin();
    m_worker.reset();
    m_impl->resident.store(0);
}
