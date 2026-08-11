#include "rendering/WaveformTileRasterizer.h"
#include "waveform/WaveformLineStore.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    std::mutex readyMutex;
    std::condition_variable readyCondition;

    WaveformLineStore store;
    constexpr std::uint32_t totalLines = 256'000;
    store.reset(77, totalLines);
    constexpr std::uint32_t populatedChunkIndex = 19;
    auto populatedLines = std::make_shared<std::vector<WaveformLine>>(
        WaveformLineStore::kChunkSize);
    for (std::size_t index = 0; index < populatedLines->size(); ++index) {
        auto& line = (*populatedLines)[index];
        line.minimum = static_cast<std::int16_t>(-9000 - index % 3000);
        line.maximum = static_cast<std::int16_t>(11000 + index % 3000);
        line.red = 232;
        line.green = 96;
        line.blue = 48;
        line.flags = waveform_line_flags::kAvailable
            | waveform_line_flags::kFinal;
    }
    ok &= require(store.publish({
                      77, populatedChunkIndex,
                      populatedChunkIndex * WaveformLineStore::kChunkSize,
                      WaveformLineStore::kChunkSize, totalLines,
                      std::move(populatedLines)})
                      == WaveformLineStore::PublishResult::Accepted,
                  "populated render benchmark chunk must publish");
    const auto snapshot = store.snapshot();

    waveform_render::WaveformTileRasterizer rasterizer([&] {
        readyCondition.notify_all();
    });
    rasterizer.setActiveTrackGeneration(snapshot->trackGeneration);

    constexpr int requestedTiles = 80;
    for (int index = 0; index < requestedTiles; ++index) {
        const auto span = waveform_render::renderTileSpan(index, 1.0, totalLines);
        const auto key = waveform_render::WaveformTileRasterizer::makeKey(
            *snapshot, index, span, 1.0, 256, 1.0);
        rasterizer.request({key, span, snapshot, 1.0, 256.0, 1.0,
                            static_cast<double>(index)});

        std::unique_lock lock(readyMutex);
        ok &= require(readyCondition.wait_for(
                          lock, std::chrono::seconds(2), [&] {
                              return rasterizer.stats().rasterizedTiles
                                  >= static_cast<std::uint64_t>(index + 1);
                          }),
                      "tile worker did not complete a bounded request");
        if (!ok)
            break;
    }

    const auto stats = rasterizer.stats();
    ok &= require(stats.cacheBytes
                      <= waveform_render::WaveformTileRasterizer::kMaximumCacheBytes,
                  "long scrolling exceeded the tile-cache byte budget");
    ok &= require(stats.cacheEntries
                      <= waveform_render::WaveformTileRasterizer::kMaximumCacheEntries,
                  "long scrolling exceeded the tile-cache entry budget");
    ok &= require(stats.rasterizedTiles == requestedTiles,
                  "every unique tile should be rasterized exactly once");

    const auto newestKey = waveform_render::WaveformTileRasterizer::makeKey(
        *snapshot, requestedTiles - 1,
        waveform_render::renderTileSpan(requestedTiles - 1, 1.0, totalLines),
        1.0, 256, 1.0);
    const auto newestTile = rasterizer.find(newestKey);
    ok &= require(static_cast<bool>(newestTile),
                  "most recently rasterized tile was not retained");
    ok &= require(newestTile && newestTile->hasAnySourceData
                      && newestTile->renderedColumns == 512,
                  "real source tile did not retain the fixed stroke density");
    std::cout << "waveform tile benchmark: worst=" << stats.worstRasterUsec
              << " us, cache=" << stats.cacheBytes << " bytes, entries="
              << stats.cacheEntries << '\n';

    auto overviewSamples = std::make_shared<std::vector<
        waveform_render::OverviewSample>>(128);
    for (auto& sample : *overviewSamples) {
        sample.rms = 0.5f;
        sample.low = 0.8f;
        sample.mid = 0.25f;
    }
    const waveform_render::OverviewRenderKey overviewKey{
        snapshot->trackGeneration, 1234, 512, 128,
        0, totalLines, totalLines};
    rasterizer.requestOverview({overviewKey, overviewSamples});
    std::shared_ptr<const waveform_render::RasterizedOverview> overview;
    {
        std::unique_lock lock(readyMutex);
        ok &= require(readyCondition.wait_for(
                          lock, std::chrono::seconds(2), [&] {
                              overview = rasterizer.findOverview(overviewKey);
                              return static_cast<bool>(overview);
                          }),
                      "fallback overview was not rasterized promptly");
    }
    ok &= require(overview && !overview->image.isNull(),
                  "fallback overview must provide visible pixels");
    const waveform_render::OverviewRenderKey viewportOverviewKey{
        snapshot->trackGeneration, 1234, 2048, 128,
        96'000, 112'000, totalLines};
    rasterizer.requestOverview({viewportOverviewKey, overviewSamples});
    std::shared_ptr<const waveform_render::RasterizedOverview> viewportOverview;
    {
        std::unique_lock lock(readyMutex);
        ok &= require(readyCondition.wait_for(
                          lock, std::chrono::seconds(2), [&] {
                              viewportOverview = rasterizer.findOverview(
                                  viewportOverviewKey);
                              return static_cast<bool>(viewportOverview);
                          }),
                      "viewport fallback was not rasterized promptly");
    }
    ok &= require(viewportOverview
                      && viewportOverview->image.width() == 2048
                      && viewportOverview->key.sourceBegin == 96'000
                      && viewportOverview->key.sourceEnd == 112'000,
                  "viewport fallback lost its bounded source-to-pixel mapping");

    WaveformLineStore oneSidedStore;
    constexpr std::uint32_t oneSidedLineCount = 1024;
    oneSidedStore.reset(99, oneSidedLineCount);
    auto oneSidedLines = std::make_shared<std::vector<WaveformLine>>(
        oneSidedLineCount);
    for (std::size_t index = 0; index < oneSidedLines->size(); ++index) {
        auto& line = (*oneSidedLines)[index];
        if (index < oneSidedLines->size() / 2) {
            line.minimum = 1200;
            line.maximum = 4800;
        } else {
            line.minimum = -4800;
            line.maximum = -1200;
        }
        line.red = 240;
        line.green = 120;
        line.blue = 60;
        line.flags = waveform_line_flags::kAvailable;
    }
    ok &= require(oneSidedStore.publish({
                      99, 0, 0, oneSidedLineCount, oneSidedLineCount,
                      std::move(oneSidedLines)})
                      == WaveformLineStore::PublishResult::Accepted,
                  "one-sided raster fixture was rejected");
    const auto oneSidedSnapshot = oneSidedStore.snapshot();
    rasterizer.setActiveTrackGeneration(oneSidedSnapshot->trackGeneration);
    const auto oneSidedSpan = waveform_render::renderTileSpan(
        0, 1.0, oneSidedLineCount);
    const auto oneSidedKey = waveform_render::WaveformTileRasterizer::makeKey(
        *oneSidedSnapshot, 0, oneSidedSpan, 1.0, 256, 1.0);
    rasterizer.request({oneSidedKey, oneSidedSpan, oneSidedSnapshot,
                        1.0, 256.0, 1.0, 0.0});
    std::shared_ptr<const waveform_render::RasterizedRenderTile> oneSidedTile;
    {
        std::unique_lock lock(readyMutex);
        ok &= require(readyCondition.wait_for(
                          lock, std::chrono::seconds(2), [&] {
                              oneSidedTile = rasterizer.find(oneSidedKey);
                              return static_cast<bool>(oneSidedTile);
                          }),
                      "one-sided render tile was not rasterized promptly");
    }
    ok &= require(oneSidedTile
                      && qAlpha(oneSidedTile->image.pixel(0, 115)) > 0
                      && qAlpha(oneSidedTile->image.pixel(0, 126)) == 0,
                  "positive-only extrema were incorrectly extended to zero");
    ok &= require(oneSidedTile
                      && qAlpha(oneSidedTile->image.pixel(600, 140)) > 0
                      && qAlpha(oneSidedTile->image.pixel(600, 129)) == 0,
                  "negative-only extrema were incorrectly extended to zero");
    ok &= require(waveform_render::bestAvailableCoverage(false, true)
                      == waveform_render::WaveformCoverage::Fallback,
                  "missing high-resolution data must retain fallback coverage");
    ok &= require(waveform_render::bestAvailableCoverage(true, true)
                      == waveform_render::WaveformCoverage::HighResolution,
                  "ready high-resolution tiles must cover the fallback");

    // Regression: a guard-window rebase must not wait for one atomic batch.
    // Only two of six new-configuration tiles are deliberately made ready.
    // Those two publish immediately, old-scale keys are rejected per slot,
    // and the fallback covers every remaining slot.
    std::array<waveform_render::RenderTileKey, 6> requiredKeys{};
    std::array<std::optional<waveform_render::RenderTileKey>, 6> displayedKeys{};
    std::array<std::optional<waveform_render::RenderTileKey>, 6> preparedKeys{};
    for (std::size_t index = 0; index < requiredKeys.size(); ++index) {
        requiredKeys[index] = {
            77, 100 + index, static_cast<std::int64_t>(index),
            220'000, 256, 1000, 0};
        auto oldScaleKey = requiredKeys[index];
        oldScaleKey.physicalPixelsPerLineMicros = 1'000'000;
        displayedKeys[index] = oldScaleKey;
    }
    preparedKeys[1] = requiredKeys[1];
    preparedKeys[4] = requiredKeys[4];
    int progressivelyPublished = 0;
    int rejectedOldConfiguration = 0;
    for (std::size_t index = 0; index < requiredKeys.size(); ++index) {
        const bool displayedIsCurrent = waveform_render::currentRenderTileKey(
            displayedKeys[index], requiredKeys[index]);
        const bool preparedIsCurrent = waveform_render::currentRenderTileKey(
            preparedKeys[index], requiredKeys[index]);
        if (preparedIsCurrent)
            ++progressivelyPublished;
        if (displayedKeys[index] && !displayedIsCurrent)
            ++rejectedOldConfiguration;
        const auto coverage = waveform_render::bestAvailableCoverage(
            displayedIsCurrent || preparedIsCurrent, true);
        ok &= require(coverage != waveform_render::WaveformCoverage::Missing,
                      "partial rebase exposed a slot without tile or fallback");
    }
    ok &= require(progressivelyPublished == 2,
                  "ready tiles must publish before the batch is complete");
    ok &= require(rejectedOldConfiguration == 6,
                  "old zoom/window tile keys must never survive a rebase");

    // Rebase workload used to compare serial and parallel rasterization. Queue
    // all slots together, as ScrollingWaveformItem does after crossing a guard.
    WaveformLineStore rebaseStore;
    constexpr std::uint32_t rebaseTileCount = 24;
    constexpr std::uint32_t rebaseLineCount = rebaseTileCount
        * waveform_render::kRenderTilePhysicalWidth;
    rebaseStore.reset(88, rebaseLineCount);
    const auto rebaseChunkCount = (rebaseLineCount
        + WaveformLineStore::kChunkSize - 1) / WaveformLineStore::kChunkSize;
    for (std::uint32_t chunkIndex = 0; chunkIndex < rebaseChunkCount; ++chunkIndex) {
        const auto firstLine = chunkIndex * WaveformLineStore::kChunkSize;
        const auto lineCount = std::min(
            WaveformLineStore::kChunkSize, rebaseLineCount - firstLine);
        auto lines = std::make_shared<std::vector<WaveformLine>>(lineCount);
        for (std::size_t lineIndex = 0; lineIndex < lines->size(); ++lineIndex) {
            auto& line = (*lines)[lineIndex];
            line.minimum = static_cast<std::int16_t>(-12'000 - lineIndex % 4'000);
            line.maximum = static_cast<std::int16_t>(14'000 + lineIndex % 4'000);
            line.red = 220;
            line.green = 110;
            line.blue = 55;
            line.flags = waveform_line_flags::kAvailable
                | waveform_line_flags::kFinal;
        }
        ok &= require(rebaseStore.publish({
                          88, chunkIndex, firstLine, lineCount,
                          rebaseLineCount, std::move(lines)})
                          == WaveformLineStore::PublishResult::Accepted,
                      "rebase benchmark chunk must publish");
    }
    const auto rebaseSnapshot = rebaseStore.snapshot();
    waveform_render::WaveformTileRasterizer rebaseRasterizer([&] {
        readyCondition.notify_all();
    });
    rebaseRasterizer.setActiveTrackGeneration(rebaseSnapshot->trackGeneration);
    const auto rebaseStarted = std::chrono::steady_clock::now();
    for (std::uint32_t tileIndex = 0; tileIndex < rebaseTileCount; ++tileIndex) {
        const auto span = waveform_render::renderTileSpan(
            tileIndex, 1.0, rebaseLineCount);
        const auto key = waveform_render::WaveformTileRasterizer::makeKey(
            *rebaseSnapshot, tileIndex, span, 1.0, 512, 1.0);
        rebaseRasterizer.request({key, span, rebaseSnapshot, 1.0, 512.0,
                                  1.0, static_cast<double>(tileIndex)});
    }
    {
        std::unique_lock lock(readyMutex);
        ok &= require(readyCondition.wait_for(
                          lock, std::chrono::seconds(5), [&] {
                              return rebaseRasterizer.stats().rasterizedTiles
                                  >= rebaseTileCount;
                          }),
                      "guard-window tile batch did not complete in time");
    }
    const auto rebaseElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - rebaseStarted).count();
    const auto rebaseStats = rebaseRasterizer.stats();
    if (std::thread::hardware_concurrency() > 2) {
        ok &= require(rebaseStats.workerCount >= 2,
                      "multi-core systems must use more than one tile worker");
        ok &= require(rebaseStats.maximumConcurrentWorkers >= 2,
                      "guard-window tiles were not rasterized concurrently");
    } else {
        ok &= require(rebaseStats.workerCount == 1,
                      "small systems must reserve capacity for UI/audio work");
    }
    ok &= require(rebaseStats.pendingRequests == 0,
                  "completed tile batch left queued or in-flight requests");
    std::cout << "waveform 24-tile rebase: wall=" << rebaseElapsed
              << " us, worst=" << rebaseStats.worstRasterUsec
              << " us, total-worker=" << rebaseStats.totalRasterUsec
              << " us, workers=" << rebaseStats.workerCount
              << ", max-concurrent=" << rebaseStats.maximumConcurrentWorkers
              << '\n';
    return ok ? 0 : 1;
}
