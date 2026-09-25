#include "WaveformLineStore.h"

#include <algorithm>
#include <limits>

namespace {

bool sameChunk(const WaveformLineChunk& left, const WaveformLineChunk& right)
{
    if (left.trackGeneration != right.trackGeneration
        || left.chunkIndex != right.chunkIndex
        || left.firstLineIndex != right.firstLineIndex
        || left.lineCount != right.lineCount
        || left.totalLineCount != right.totalLineCount
        || !left.lines || !right.lines
        || left.lines->size() != right.lines->size()) {
        return false;
    }

    return std::equal(left.lines->cbegin(), left.lines->cend(), right.lines->cbegin(),
        [](const WaveformLine& a, const WaveformLine& b) {
            return a.minimum == b.minimum && a.maximum == b.maximum
                && a.rms == b.rms && a.bass == b.bass && a.mid == b.mid
                && a.treble == b.treble && a.flags == b.flags;
        });
}

WaveformChunkState classifyChunk(const WaveformLineChunk& chunk)
{
    if (!chunk.lines || chunk.lines->empty())
        return WaveformChunkState::Missing;

    bool allAvailable = true;
    bool allFinal = true;
    for (const auto& line : *chunk.lines) {
        const bool available = (line.flags & waveform_line_flags::kAvailable) != 0;
        allAvailable = allAvailable && available;
        allFinal = allFinal && available
            && (line.flags & waveform_line_flags::kFinal) != 0;
    }
    if (!allAvailable)
        return WaveformChunkState::Loading;
    return allFinal ? WaveformChunkState::FinalReady
                    : WaveformChunkState::PreviewReady;
}

} // namespace

std::shared_ptr<const WaveformLineChunk> WaveformLineStoreSnapshot::chunkAt(std::uint32_t index) const noexcept
{
    if (!chunks || index >= chunks->size()) return {};
    return (*chunks)[index];
}

std::uint32_t WaveformLineStoreSnapshot::availableChunkCount() const noexcept
{
    if (!chunks) return 0;
    return static_cast<std::uint32_t>(std::count_if(chunks->cbegin(), chunks->cend(),
        [](const auto& chunk) { return chunk && chunk->isReady(); }));
}

void WaveformLineStore::reset(std::uint64_t trackGeneration, std::uint32_t totalLineCount,
                              std::uint32_t linesPerSecond, std::uint32_t chunkSize)
{
    if (trackGeneration == 0 || linesPerSecond == 0 || chunkSize == 0)
        return;
    const std::uint64_t chunkCount = (static_cast<std::uint64_t>(totalLineCount) + chunkSize - 1) / chunkSize;
    if (chunkCount > std::numeric_limits<std::uint32_t>::max())
        return;
    auto next = std::make_shared<WaveformLineStoreSnapshot>();
    next->trackGeneration = trackGeneration;
    next->linesPerSecond = linesPerSecond;
    next->chunkSize = chunkSize;
    next->totalLineCount = totalLineCount;
    next->chunks = std::make_shared<const std::vector<std::shared_ptr<const WaveformLineChunk>>>(
        static_cast<size_t>(chunkCount));
    std::lock_guard lock(m_snapshotMutex);
    next->dataGeneration = m_snapshot ? m_snapshot->dataGeneration + 1 : 1;
    m_snapshot = std::move(next);
}

WaveformLineStore::PublishResult WaveformLineStore::publish(WaveformLineChunk chunk)
{
    std::vector<WaveformLineChunk> batch;
    batch.push_back(std::move(chunk));
    return publishBatch(std::move(batch));
}

WaveformLineStore::PublishResult WaveformLineStore::publishBatch(
    std::vector<WaveformLineChunk> chunks)
{
    // Only the owner thread mutates, so reading the current snapshot outside
    // the lock and swapping under it is safe: no other writer can interleave.
    // Keeping the (potentially large) table copy below outside the lock is the
    // point — readers must not wait for it.
    const auto current = snapshot();
    if (!current || !current->chunks || chunks.empty())
        return PublishResult::Rejected;
    std::vector<bool> seen(current->chunks->size(), false);
    for (const auto& chunk : chunks) {
        if (chunk.trackGeneration != current->trackGeneration
            || chunk.totalLineCount != current->totalLineCount
            || !chunk.isWellFormed(current->chunkSize)
            || chunk.chunkIndex >= current->chunks->size()
            || seen[chunk.chunkIndex]) {
            return PublishResult::Rejected;
        }
        const auto expectedCount = std::min(
            current->chunkSize,
            current->totalLineCount - chunk.firstLineIndex);
        if (chunk.lineCount != expectedCount)
            return PublishResult::Rejected;
        seen[chunk.chunkIndex] = true;
    }

    bool changed = false;
    const auto revision = current->dataGeneration + 1;
    auto table = std::make_shared<std::vector<
        std::shared_ptr<const WaveformLineChunk>>>(*current->chunks);
    for (auto& chunk : chunks) {
        const auto& previous = (*current->chunks)[chunk.chunkIndex];
        if (previous && sameChunk(*previous, chunk))
            continue;
        // Final means immutable for this TrackGeneration. A late preview or a
        // duplicate final pass may not reshape an already delivered region.
        if (previous && previous->state == WaveformChunkState::FinalReady)
            continue;
        chunk.revision = revision;
        chunk.state = classifyChunk(chunk);
        (*table)[chunk.chunkIndex]
            = std::make_shared<const WaveformLineChunk>(std::move(chunk));
        changed = true;
    }
    if (!changed)
        return PublishResult::Duplicate;

    // One immutable table swap publishes the complete viewport/control-tick
    // batch. Render-thread snapshots can never observe its middle.
    auto next = std::make_shared<WaveformLineStoreSnapshot>(*current);
    next->dataGeneration = revision;
    next->chunks = std::move(table);
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
    return PublishResult::Accepted;
}

std::shared_ptr<const WaveformLineStoreSnapshot> WaveformLineStore::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}
