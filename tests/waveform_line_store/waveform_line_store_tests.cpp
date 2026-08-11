#include "waveform/WaveformLineStore.h"
#include "waveform/WaveformLodPyramid.h"

#include <iostream>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

WaveformLineChunk makeChunk(std::uint64_t generation, std::uint32_t index,
                            std::uint32_t total, std::uint32_t chunkSize)
{
    const auto first = index * chunkSize;
    const auto count = std::min(chunkSize, total - first);
    auto lines = std::make_shared<std::vector<WaveformLine>>(count);
    (*lines)[0] = {.minimum = -100, .maximum = 200, .red = 255, .green = 80, .blue = 40, .flags = 1};
    return {generation, index, first, count, total, std::move(lines)};
}
}

int main()
{
    constexpr std::uint32_t chunkSize = 4096;
    constexpr std::uint32_t total = chunkSize * 2 + 17;
    WaveformLineStore store;
    bool ok = true;
    ok &= require(store.snapshot()->totalLineCount == 0, "empty store has no timeline");
    store.reset(9, total, 300, chunkSize);
    const auto initial = store.snapshot();
    ok &= require(initial->chunks->size() == 3, "fixed table includes partial final chunk");
    ok &= require(store.publish(makeChunk(9, 0, total, chunkSize)) == WaveformLineStore::PublishResult::Accepted,
                  "first chunk accepted");
    const auto afterFirst = store.snapshot();
    ok &= require(afterFirst->availableChunkCount() == 1 && !afterFirst->chunkAt(1),
                  "missing chunks remain absent in the same timeline");
    const auto firstChunkRevision = waveform::WaveformLodPyramid::sourceRevision(
        *afterFirst, 0, 0, chunkSize);
    const auto missingChunkRevision = waveform::WaveformLodPyramid::sourceRevision(
        *afterFirst, 0, chunkSize, chunkSize * 2);
    ok &= require(store.publish(makeChunk(9, 1, total, chunkSize)) == WaveformLineStore::PublishResult::Accepted,
                  "middle chunk accepted");
    ok &= require(store.publish(makeChunk(9, 2, total, chunkSize)) == WaveformLineStore::PublishResult::Accepted,
                  "partial final chunk accepted");
    ok &= require(waveform::WaveformLodPyramid::sourceRevision(
                      *store.snapshot(), 0, 0, chunkSize) == firstChunkRevision,
                  "unrelated chunk publication invalidated a stable render range");
    ok &= require(waveform::WaveformLodPyramid::sourceRevision(
                      *store.snapshot(), 0, chunkSize, chunkSize * 2)
                      != missingChunkRevision,
                  "new chunk publication did not invalidate its render range");
    ok &= require(store.snapshot()->availableChunkCount() == 3, "all chunks visible without store replacement");
    const auto lodSnapshot = store.snapshot();
    const auto lodSample = waveform::WaveformLodPyramid::sample(
        *lodSnapshot, 4, chunkSize / 16);
    ok &= require(lodSample.hasData && lodSample.complete
                      && lodSample.line.maximum == 200,
                  "LOD pyramid did not derive a complete cross-chunk level");
    ok &= require(waveform::WaveformLodPyramid::linesPerSecond(1200, 4) == 75,
                  "LOD pyramid rate does not reach 75 lines per second");
    WaveformLodBatch persistedLod;
    auto persistedLines = std::make_shared<std::vector<WaveformLine>>(
        (total + 15) / 16);
    (*persistedLines)[chunkSize / 16].maximum = 3210;
    persistedLod.push_back({4, 16, 0,
        static_cast<int>(persistedLines->size()), std::move(persistedLines)});
    ok &= require(store.publishLodBatch(std::move(persistedLod)),
                  "persisted LOD level was rejected");
    const auto persistedSample = waveform::WaveformLodPyramid::sample(
        *store.snapshot(), 4, chunkSize / 16);
    ok &= require(persistedSample.complete
                      && persistedSample.line.maximum == 3210,
                  "renderer did not consume the persisted LOD sample");
    ok &= require(store.publish(makeChunk(9, 2, total, chunkSize)) == WaveformLineStore::PublishResult::Duplicate,
                  "duplicate chunk is idempotent");
    const auto revisedFirst = chunkSize;
    auto revisedLines = std::make_shared<std::vector<WaveformLine>>(chunkSize);
    (*revisedLines)[0] = {.minimum = -100, .maximum = 900,
                          .red = 255, .green = 80, .blue = 40, .flags = 1};
    WaveformLineChunk revised{9, 1, revisedFirst, chunkSize, total, std::move(revisedLines)};
    ok &= require(store.publish(std::move(revised)) == WaveformLineStore::PublishResult::Accepted,
                  "new immutable revision of a progressive chunk was rejected");
    const auto revisedChunk = store.snapshot()->chunkAt(1);
    ok &= require(revisedChunk && (*revisedChunk->lines)[0].maximum == 900,
                  "latest snapshot did not expose progressive chunk revision");
    ok &= require(store.publish(makeChunk(8, 0, total, chunkSize)) == WaveformLineStore::PublishResult::Rejected,
                  "stale generation rejected");
    auto invalid = makeChunk(9, 0, total, chunkSize);
    invalid.firstLineIndex = 3;
    ok &= require(store.publish(std::move(invalid)) == WaveformLineStore::PublishResult::Rejected,
                  "invalid fixed range rejected");
    return ok ? 0 : 1;
}
