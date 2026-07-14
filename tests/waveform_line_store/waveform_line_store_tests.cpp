#include "waveform/WaveformLineStore.h"

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
    ok &= require(store.publish(makeChunk(9, 1, total, chunkSize)) == WaveformLineStore::PublishResult::Accepted,
                  "middle chunk accepted");
    ok &= require(store.publish(makeChunk(9, 2, total, chunkSize)) == WaveformLineStore::PublishResult::Accepted,
                  "partial final chunk accepted");
    ok &= require(store.snapshot()->availableChunkCount() == 3, "all chunks visible without store replacement");
    ok &= require(store.publish(makeChunk(9, 2, total, chunkSize)) == WaveformLineStore::PublishResult::Duplicate,
                  "duplicate chunk is idempotent");
    ok &= require(store.publish(makeChunk(8, 0, total, chunkSize)) == WaveformLineStore::PublishResult::Rejected,
                  "stale generation rejected");
    auto invalid = makeChunk(9, 0, total, chunkSize);
    invalid.firstLineIndex = 3;
    ok &= require(store.publish(std::move(invalid)) == WaveformLineStore::PublishResult::Rejected,
                  "invalid fixed range rejected");
    return ok ? 0 : 1;
}
