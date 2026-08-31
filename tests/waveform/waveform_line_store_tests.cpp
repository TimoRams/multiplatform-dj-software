#include "waveform/WaveformLineStore.h"
#include "waveform/WaveformLodPyramid.h"

#include <algorithm>
#include <array>
#include <chrono>
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
    std::fill(lines->begin(), lines->end(), WaveformLine{
        .minimum = -100, .maximum = 200, .rms = 180, .bass = 255,
        .mid = 80, .treble = 40, .flags = waveform_line_flags::kAvailable});
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
    WaveformLineStore oneSidedStore;
    oneSidedStore.reset(10, 16, 1200, 16);
    auto oneSidedLines = std::make_shared<std::vector<WaveformLine>>(16);
    for (std::size_t index = 1; index < oneSidedLines->size(); ++index) {
        auto& line = (*oneSidedLines)[index];
        line = {.minimum = 1200, .maximum = 4800, .rms = 180,
                .bass = 200, .mid = 100, .treble = 50,
                .flags = waveform_line_flags::kAvailable};
    }
    ok &= require(oneSidedStore.publish({10, 0, 0, 16, 16,
                                         std::move(oneSidedLines)})
                      == WaveformLineStore::PublishResult::Accepted,
                  "one-sided LOD fixture was rejected");
    ok &= require(oneSidedStore.snapshot()->availableChunkCount() == 0
                      && oneSidedStore.snapshot()->chunkAt(0)
                      && oneSidedStore.snapshot()->chunkAt(0)->state
                          == WaveformChunkState::Loading,
                  "partially populated chunk was incorrectly classified READY");
    const auto oneSidedLod = waveform::WaveformLodPyramid::sample(
        *oneSidedStore.snapshot(), 4, 0);
    ok &= require(oneSidedLod.hasData && !oneSidedLod.complete
                      && oneSidedLod.line.minimum == 1200
                      && oneSidedLod.line.maximum == 4800,
                  "LOD aggregation used missing data as a false zero extremum");
    WaveformLineStore batchStore;
    batchStore.reset(11, chunkSize * 3, 1200, chunkSize);
    const auto beforeBatchGeneration = batchStore.snapshot()->dataGeneration;
    std::vector<WaveformLineChunk> viewportBatch;
    viewportBatch.push_back(makeChunk(11, 1, chunkSize * 3, chunkSize));
    viewportBatch.push_back(makeChunk(11, 2, chunkSize * 3, chunkSize));
    ok &= require(batchStore.publishBatch(std::move(viewportBatch))
                      == WaveformLineStore::PublishResult::Accepted,
                  "viewport batch publication was rejected");
    const auto afterBatch = batchStore.snapshot();
    ok &= require(afterBatch->chunkAt(1) && afterBatch->chunkAt(2)
                      && afterBatch->dataGeneration == beforeBatchGeneration + 1,
                  "viewport batch did not publish through one immutable table swap");
    WaveformLineStore finalStore;
    finalStore.reset(12, 16, 1200, 16);
    auto finalLines = std::make_shared<std::vector<WaveformLine>>(16);
    for (auto& line : *finalLines) {
        line.maximum = 400;
        line.flags = waveform_line_flags::kAvailable
            | waveform_line_flags::kFinal;
    }
    ok &= require(finalStore.publish({12, 0, 0, 16, 16, finalLines})
                      == WaveformLineStore::PublishResult::Accepted,
                  "final chunk fixture was rejected");
    auto lateLines = std::make_shared<std::vector<WaveformLine>>(*finalLines);
    (*lateLines)[0].maximum = 900;
    (*lateLines)[0].flags = waveform_line_flags::kAvailable;
    ok &= require(finalStore.publish({12, 0, 0, 16, 16, lateLines})
                      == WaveformLineStore::PublishResult::Duplicate
                      && finalStore.snapshot()->chunkAt(0)
                      && (*finalStore.snapshot()->chunkAt(0)->lines)[0].maximum == 400,
                  "late publication changed an immutable final chunk");
    ok &= require(store.publish(makeChunk(9, 2, total, chunkSize)) == WaveformLineStore::PublishResult::Duplicate,
                  "duplicate chunk is idempotent");
    const auto revisedFirst = chunkSize;
    auto revisedLines = std::make_shared<std::vector<WaveformLine>>(chunkSize);
    (*revisedLines)[0] = {.minimum = -100, .maximum = 900, .rms = 180,
                          .bass = 255, .mid = 80, .treble = 40, .flags = 1};
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

    // Two-hour chunk-size evaluation. Render textures stay fixed at 1024
    // physical pixels independently (covered by render-tile tests); this
    // measures the store/index side and records the seek granularity tradeoff.
    constexpr std::uint32_t twoHourLines = 2 * 60 * 60
        * WaveformLineStore::kCanonicalLinesPerSecond;
    for (const auto candidate : std::array<std::uint32_t, 4>{512, 1024, 2048, 4096}) {
        const auto started = std::chrono::steady_clock::now();
        WaveformLineStore candidateStore;
        candidateStore.reset(20 + candidate, twoHourLines,
                             WaveformLineStore::kCanonicalLinesPerSecond,
                             candidate);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        const auto candidateSnapshot = candidateStore.snapshot();
        const auto chunkCount = candidateSnapshot->chunks->size();
        const auto metadataBytes = chunkCount
            * sizeof(std::shared_ptr<const WaveformLineChunk>);
        const double secondsPerChunk = static_cast<double>(candidate)
            / WaveformLineStore::kCanonicalLinesPerSecond;
        std::cout << "waveform chunk candidate=" << candidate
                  << " lines, seconds=" << secondsPerChunk
                  << ", chunks=" << chunkCount
                  << ", pointer-table=" << metadataBytes
                  << " bytes, reset=" << elapsed << " us\n";
        if (candidate == WaveformLineStore::kChunkSize) {
            ok &= require(secondsPerChunk < 1.0,
                          "selected chunk cannot become ready within one second");
            ok &= require(metadataBytes < 160 * 1024,
                          "selected two-hour chunk index exceeds metadata budget");
        }
    }
    ok &= require(WaveformLineStore::kChunkSize == 1024,
                  "chunk benchmark selection and production constant diverged");
    return ok ? 0 : 1;
}
