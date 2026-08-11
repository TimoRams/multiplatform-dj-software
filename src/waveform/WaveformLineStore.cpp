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
                && a.red == b.red && a.green == b.green && a.blue == b.blue
                && a.flags == b.flags;
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

std::shared_ptr<const WaveformLodChunk>
WaveformLodLevelSnapshot::chunkAt(std::uint32_t index) const noexcept
{
    if (!chunks || index >= chunks->size()) return {};
    return (*chunks)[index];
}

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

std::shared_ptr<const WaveformLodLevelSnapshot>
WaveformLineStoreSnapshot::lodLevel(std::uint8_t level) const noexcept
{
    return level < lodLevels.size() ? lodLevels[level] : nullptr;
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
    next->dataGeneration = m_snapshot ? m_snapshot->dataGeneration + 1 : 1;
    next->linesPerSecond = linesPerSecond;
    next->chunkSize = chunkSize;
    next->totalLineCount = totalLineCount;
    next->chunks = std::make_shared<const std::vector<std::shared_ptr<const WaveformLineChunk>>>(
        static_cast<size_t>(chunkCount));
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
    const auto current = m_snapshot;
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
    m_snapshot = std::move(next);
    return PublishResult::Accepted;
}

bool WaveformLineStore::publishLodBatch(WaveformLodBatch batch)
{
    const auto current = m_snapshot;
    if (!current || current->trackGeneration == 0 || batch.empty())
        return false;

    const auto level = batch.front().level;
    const auto stride = batch.front().canonicalLineStride;
    const auto totalSamples = batch.front().totalSamples;
    if (level < 1 || level >= static_cast<int>(current->lodLevels.size())
        || stride <= 1 || totalSamples <= 0) {
        return false;
    }
    constexpr std::uint32_t lodChunkSize = 4096;
    const auto total = static_cast<std::uint32_t>(totalSamples);
    const auto chunkCount = (static_cast<std::uint64_t>(total)
        + lodChunkSize - 1) / lodChunkSize;
    if (chunkCount > std::numeric_limits<std::uint32_t>::max())
        return false;

    auto previousLevel = current->lodLevels[static_cast<std::size_t>(level)];
    if (previousLevel
        && (previousLevel->canonicalLineStride != static_cast<std::uint32_t>(stride)
            || previousLevel->totalSampleCount != total
            || previousLevel->chunkSize != lodChunkSize)) {
        return false;
    }
    for (const auto& block : batch) {
        if (block.level != level || block.canonicalLineStride != stride
            || block.totalSamples != totalSamples || block.firstSample < 0
            || block.firstSample % static_cast<int>(lodChunkSize) != 0
            || !block.lines || block.lines->empty()) {
            return false;
        }
        const auto first = static_cast<std::uint32_t>(block.firstSample);
        if (first >= total
            || block.lines->size() != std::min<std::size_t>(
                lodChunkSize, total - first)) {
            return false;
        }
    }

    auto table = previousLevel && previousLevel->chunks
        ? std::make_shared<std::vector<std::shared_ptr<const WaveformLodChunk>>>(
            *previousLevel->chunks)
        : std::make_shared<std::vector<std::shared_ptr<const WaveformLodChunk>>>(
            static_cast<std::size_t>(chunkCount));
    if (table->size() != chunkCount)
        return false;

    const auto revision = current->dataGeneration + 1;
    for (auto& block : batch) {
        const auto first = static_cast<std::uint32_t>(block.firstSample);
        const auto index = first / lodChunkSize;
        (*table)[index] = std::make_shared<const WaveformLodChunk>(
            WaveformLodChunk{revision, first,
                static_cast<std::uint32_t>(block.lines->size()),
                std::move(block.lines)});
    }

    auto nextLevel = std::make_shared<WaveformLodLevelSnapshot>();
    nextLevel->level = static_cast<std::uint8_t>(level);
    nextLevel->canonicalLineStride = static_cast<std::uint32_t>(stride);
    nextLevel->chunkSize = lodChunkSize;
    nextLevel->totalSampleCount = total;
    nextLevel->chunks = std::move(table);
    auto next = std::make_shared<WaveformLineStoreSnapshot>(*current);
    next->dataGeneration = revision;
    next->lodLevels[static_cast<std::size_t>(level)] = std::move(nextLevel);
    m_snapshot = std::move(next);
    return true;
}

std::shared_ptr<const WaveformLineStoreSnapshot> WaveformLineStore::snapshot() const
{
    return m_snapshot;
}
