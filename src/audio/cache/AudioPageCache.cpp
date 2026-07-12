#include "AudioPageCache.h"
#include "AudioCacheWorker.h"

#include <QFileInfo>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
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
};

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
        std::atomic<std::uint64_t> accessEpoch{0};
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
        std::unique_ptr<juce::AudioFormatReader> reader;
        std::unique_ptr<PageSlot[]> pageSlots;
        std::int64_t pageCount = 0;
    };

    juce::AudioFormatManager formats;
    std::mutex controlMutex;
    std::unordered_map<std::string, Entry*> activeEntries;
    std::vector<std::unique_ptr<Entry>> entries; // stable addresses until cache shutdown
    std::array<BoundedQueue<AudioPageCache::kRequestQueueCapacity>,
               static_cast<size_t>(AudioCachePriority::Count)> queues;
    std::condition_variable condition;
    std::mutex conditionMutex;
    std::atomic<std::uint64_t> nextId{1}, epoch{1};
    std::atomic<std::uint64_t> hits{0}, misses{0}, queued{0}, dropped{0};
    std::atomic<std::uint64_t> decoded{0}, failures{0}, evicted{0}, resident{0}, open{0};
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
    entry->reader.reset(m_impl->formats.createReaderFor(juce::File(key.canonicalPath.toStdString())));
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
    slot.accessEpoch.store(m_impl->epoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
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
    PageRequest request{entry, handle.id(), handle.generation(), pageIndex};
    if (!m_impl->queues[static_cast<size_t>(priority)].tryPush(request)) {
        slot.state.store(0, std::memory_order_release); m_impl->dropped.fetch_add(1); return false;
    }
    m_impl->queued.fetch_add(1, std::memory_order_relaxed);
    return true; // worker polls; no condition-variable syscall on the RT producer
}

bool AudioPageCache::requestRange(const AudioCacheHandle& handle, std::int64_t first,
                                   std::int64_t last, AudioCachePriority priority) noexcept
{
    if (first < 0 || last < first) return false;
    bool all = true;
    for (auto page = first; page <= last; ++page) all = requestPage(handle, page, priority) && all;
    return all;
}

AudioCacheStats AudioPageCache::stats() const noexcept
{
    return {m_impl->hits.load(), m_impl->misses.load(), m_impl->queued.load(), m_impl->dropped.load(),
            m_impl->decoded.load(), m_impl->failures.load(), m_impl->evicted.load(),
            m_impl->resident.load(), m_impl->open.load()};
}

void AudioPageCache::notifyWorker() noexcept { m_impl->condition.notify_one(); }

void AudioPageCache::workerRun(const std::atomic<bool>& shutdown)
{
    unsigned fairness = 0;
    while (!shutdown.load(std::memory_order_acquire)) {
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
        const bool ok = entry->reader->read(&buffer, 0, static_cast<int>(page->validSampleCount),
                                             page->firstSample, true, true);
        if (!ok) { slot.state.store(0); m_impl->failures.fetch_add(1); continue; }
        for (int channel = 0; channel < entry->channels; ++channel)
            std::copy_n(buffer.getReadPointer(channel), AudioPage::kSamplesPerChannel,
                        page->planarPcm.data() + static_cast<size_t>(channel) * AudioPage::kSamplesPerChannel);
        const auto bytes = page->byteSize();
        while (m_impl->resident.load() + bytes > m_budgetBytes) {
            Impl::PageSlot* victim = nullptr;
            std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
            for (const auto& candidateEntry : m_impl->entries)
                for (std::int64_t i = 0; i < candidateEntry->pageCount; ++i) {
                    auto& candidate = candidateEntry->pageSlots[static_cast<size_t>(i)];
                    if (candidate.state.load() == 2 && candidate.accessEpoch.load() < oldest) {
                        oldest = candidate.accessEpoch.load(); victim = &candidate;
                    }
                }
            if (!victim) break;
            auto* retired = victim->page.exchange(nullptr, std::memory_order_acq_rel);
            victim->state.store(0, std::memory_order_release);
            while (victim->readers.load(std::memory_order_acquire) != 0 && !shutdown.load())
                std::this_thread::yield();
            if (retired) { m_impl->resident.fetch_sub(retired->byteSize()); delete retired;
                m_impl->evicted.fetch_add(1); }
        }
        if (m_impl->resident.load() + bytes > m_budgetBytes || !entry->active.load()
            || entry->generation.load() != request.generation) { slot.state.store(0); continue; }
        slot.accessEpoch.store(m_impl->epoch.fetch_add(1));
        slot.page.store(page.release(), std::memory_order_release);
        slot.state.store(2, std::memory_order_release);
        m_impl->resident.fetch_add(bytes); m_impl->decoded.fetch_add(1);
    }

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
