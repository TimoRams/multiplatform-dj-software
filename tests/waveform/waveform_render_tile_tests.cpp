#include "waveform/render/WaveformTileRasterizer.h"
#include "waveform/WaveformLineStore.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
    constexpr int requestedTiles = 80;
    constexpr std::uint32_t populatedChunkIndex = requestedTiles - 1;
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

    // Elevated audio pressure rejects disposable raster requests synchronously;
    // re-enabling accepts only a freshly published current-view request.
    {
        waveform_render::WaveformTileRasterizer pressureRasterizer([&] {
            readyCondition.notify_all();
        });
        pressureRasterizer.setActiveTrackGeneration(snapshot->trackGeneration);
        const auto pressureSpan = waveform_render::renderTileSpan(
            populatedChunkIndex, 1.0, totalLines);
        const auto pressureKey = waveform_render::WaveformTileRasterizer::makeKey(
            *snapshot, populatedChunkIndex, pressureSpan, 1.0, 128, 1.0);
        pressureRasterizer.setWorkEnabled(false);
        pressureRasterizer.request({pressureKey, pressureSpan, snapshot,
                                    1.0, 128.0, 1.0, 0.0});
        const auto suspendedStats = pressureRasterizer.stats();
        ok &= require(!suspendedStats.workEnabled
                          && suspendedStats.pendingRequests == 0
                          && suspendedStats.rasterizedTiles == 0,
                      "render-pressure tier accepted disposable raster work");

        pressureRasterizer.setWorkEnabled(true);
        pressureRasterizer.request({pressureKey, pressureSpan, snapshot,
                                    1.0, 128.0, 1.0, 0.0});
        std::shared_ptr<const waveform_render::RasterizedRenderTile> resumedTile;
        std::unique_lock lock(readyMutex);
        ok &= require(readyCondition.wait_for(
                          lock, std::chrono::seconds(2), [&] {
                              resumedTile = pressureRasterizer.find(pressureKey);
                              return static_cast<bool>(resumedTile);
                          }),
                      "raster work did not resume with a fresh viewport request");
    }

    waveform_render::WaveformTileRasterizer rasterizer([&] {
        readyCondition.notify_all();
    });
    rasterizer.setActiveTrackGeneration(snapshot->trackGeneration);

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
                      && newestTile->renderedColumns
                          == waveform_render::kRenderTilePhysicalWidth,
                  "real source tile did not use every physical display column");
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
    constexpr QRgb detailBackground = 0xff101114u;
    const auto detailImageX = [](int corePhysicalX) {
        return corePhysicalX
            + waveform_render::kRenderTileFilterGutterPhysicalPixels;
    };
    ok &= require(oneSidedTile
                      && oneSidedTile->image.pixel(detailImageX(0), 115)
                          != detailBackground
                      && oneSidedTile->image.pixel(detailImageX(0), 126)
                          == detailBackground,
                  "positive-only extrema were incorrectly extended to zero");
    ok &= require(oneSidedTile
                      && oneSidedTile->image.pixel(detailImageX(600), 140)
                          != detailBackground
                      && oneSidedTile->image.pixel(detailImageX(600), 129)
                          == detailBackground,
                  "negative-only extrema were incorrectly extended to zero");
    ok &= require(oneSidedTile
                      && oneSidedTile->image.pixel(detailImageX(1), 115)
                          != detailBackground,
                  "full-resolution detail inserted an artificial horizontal gap");

    // Image-level precision regression. A complete detail tile must aggregate
    // every physical column that intersects the track, regardless of zoom or
    // DPR. The former alternating empty-column mask was a spatial carrier that
    // aliased into thick/thin bands during subpixel scrolling.
    const std::array<double, 4> zoomLevels{0.08, 0.22, 1.0, 10.0};
    const std::array<double, 4> devicePixelRatios{1.0, 1.25, 2.0, 3.0};
    for (const double zoom : zoomLevels) {
        for (const double dpr : devicePixelRatios) {
            const double physicalPixelsPerLine = zoom * dpr;
            constexpr double logicalHeight = 256.0;
            const int physicalHeight = std::max(
                1, static_cast<int>(std::ceil(logicalHeight * dpr)));
            const auto preciseSpan = waveform_render::renderTileSpan(
                0, physicalPixelsPerLine, oneSidedLineCount);
            const auto preciseKey = waveform_render::WaveformTileRasterizer::makeKey(
                *oneSidedSnapshot, 0, preciseSpan, physicalPixelsPerLine,
                physicalHeight, dpr);
            rasterizer.request({preciseKey, preciseSpan, oneSidedSnapshot,
                                physicalPixelsPerLine, logicalHeight, dpr, 0.0});
            std::shared_ptr<const waveform_render::RasterizedRenderTile> preciseTile;
            {
                std::unique_lock lock(readyMutex);
                ok &= require(readyCondition.wait_for(
                                  lock, std::chrono::seconds(2), [&] {
                                      preciseTile = rasterizer.find(preciseKey);
                                      return static_cast<bool>(preciseTile);
                                  }),
                              "zoom/DPR precision tile was not rasterized");
            }
            if (!preciseTile)
                continue;
            ok &= require(preciseTile->image.width()
                              == waveform_render::kRenderTileTexturePhysicalWidth
                              && preciseTile->span.physicalWidth()
                                  == waveform_render::kRenderTilePhysicalWidth,
                          "zoom changed the physical detail texture width");
            std::uint64_t expectedColumns = 0;
            bool everyActiveColumnHasInk = true;
            for (int x = 0; x < preciseTile->span.physicalWidth(); ++x) {
                const bool active
                    = waveform_render::physicalStrokeIntersectsTrack(
                        preciseSpan.physicalBegin + x, physicalPixelsPerLine,
                        oneSidedLineCount);
                expectedColumns += active ? 1u : 0u;
                if (!active)
                    continue;
                bool hasInk = false;
                for (int y = 0; y < preciseTile->image.height(); ++y) {
                    if (preciseTile->image.pixel(detailImageX(x), y)
                        != detailBackground) {
                        hasInk = true;
                        break;
                    }
                }
                everyActiveColumnHasInk = everyActiveColumnHasInk && hasInk;
            }
            ok &= require(expectedColumns > 0
                              && preciseTile->renderedColumns == expectedColumns
                              && everyActiveColumnHasInk,
                          "zoom/DPR failed to adapt detail to physical pixels");
        }
    }
    ok &= require(waveform_render::bestAvailableCoverage(false, true)
                      == waveform_render::WaveformCoverage::Fallback,
                  "missing high-resolution data must retain fallback coverage");
    ok &= require(waveform_render::bestAvailableCoverage(true, true)
                      == waveform_render::WaveformCoverage::HighResolution,
                  "ready high-resolution tiles must cover the fallback");
    ok &= require(waveform_render::detailTileMayBeDisplayed(true, true)
                      && !waveform_render::detailTileMayBeDisplayed(false, true)
                      && !waveform_render::detailTileMayBeDisplayed(true, false),
                  "a tile with real audio must display once its key is current");
    // Regression: a tile whose source lines are all present must be publishable
    // as complete detail immediately — detail that is READY is never withheld
    // waiting for some later analysis milestone. Holding ready detail back
    // (combined with correctly suppressing the magnified fallback) leaves the
    // deck showing nothing at all, which is strictly worse than showing the
    // real, already-available waveform.
    {
        WaveformLineStore readyStore;
        constexpr std::uint32_t readyLineCount
            = 2 * WaveformLineStore::kChunkSize;
        readyStore.reset(201, readyLineCount);
        auto readyLines = std::make_shared<std::vector<WaveformLine>>(
            WaveformLineStore::kChunkSize);
        for (auto& line : *readyLines) {
            line.minimum = -8000;
            line.maximum = 8000;
            line.red = 200; line.green = 80; line.blue = 40;
            line.flags = waveform_line_flags::kAvailable;
        }
        ok &= require(readyStore.publish({
                          201, 0, 0, WaveformLineStore::kChunkSize,
                          readyLineCount, std::move(readyLines)})
                          == WaveformLineStore::PublishResult::Accepted,
                      "ready-detail fixture was rejected");
        auto adjacentLines = std::make_shared<std::vector<WaveformLine>>(
            WaveformLineStore::kChunkSize);
        for (auto& line : *adjacentLines) {
            line.minimum = -14'000;
            line.maximum = 14'000;
            line.red = 70;
            line.green = 170;
            line.blue = 235;
            line.flags = waveform_line_flags::kAvailable;
        }
        ok &= require(readyStore.publish({
                          201, 1, WaveformLineStore::kChunkSize,
                          WaveformLineStore::kChunkSize, readyLineCount,
                          std::move(adjacentLines)})
                          == WaveformLineStore::PublishResult::Accepted,
                      "adjacent filter-gutter fixture was rejected");
        const auto readySnapshot = readyStore.snapshot();
        rasterizer.setActiveTrackGeneration(readySnapshot->trackGeneration);
        const auto readySpan = waveform_render::renderTileSpan(
            0, 1.0, readyLineCount);
        const auto readyKey = waveform_render::WaveformTileRasterizer::makeKey(
            *readySnapshot, 0, readySpan, 1.0, 256, 1.0);
        rasterizer.request({readyKey, readySpan, readySnapshot,
                            1.0, 256.0, 1.0, 0.0});
        std::shared_ptr<const waveform_render::RasterizedRenderTile> readyTile;
        {
            std::unique_lock lock(readyMutex);
            ok &= require(readyCondition.wait_for(
                              lock, std::chrono::seconds(2), [&] {
                                  readyTile = rasterizer.find(readyKey);
                                  return static_cast<bool>(readyTile);
                              }),
                          "ready-detail tile was not rasterized promptly");
        }
        ok &= require(readyTile && readyTile->hasAnySourceData
                          && readyTile->hasCompleteSourceData,
                      "fully populated source lines must publish as complete detail");
        if (readyTile) {
            const int centreY = readyTile->image.height() / 2;
            bool continuousCoverage = true;
            for (int x = 0; x < readyTile->span.physicalWidth(); ++x) {
                continuousCoverage = continuousCoverage
                    && readyTile->image.pixel(detailImageX(x), centreY)
                        != detailBackground;
            }
            ok &= require(continuousCoverage,
                          "constant waveform changed coverage across pixel phases");

            // The hidden filter texels must contain the actual adjacent audio,
            // not a repeated/clamped edge. That gives bilinear sampling the
            // same two columns on both sides of a texture boundary.
            const auto adjacentSpan = waveform_render::renderTileSpan(
                1, 1.0, readyLineCount);
            const auto adjacentKey
                = waveform_render::WaveformTileRasterizer::makeKey(
                    *readySnapshot, 1, adjacentSpan, 1.0, 256, 1.0);
            rasterizer.request({adjacentKey, adjacentSpan, readySnapshot,
                                1.0, 256.0, 1.0, 0.0});
            std::shared_ptr<const waveform_render::RasterizedRenderTile>
                adjacentTile;
            {
                std::unique_lock lock(readyMutex);
                ok &= require(readyCondition.wait_for(
                                  lock, std::chrono::seconds(2), [&] {
                                      adjacentTile = rasterizer.find(adjacentKey);
                                      return static_cast<bool>(adjacentTile);
                                  }),
                              "adjacent filter-gutter tile was not rasterized");
            }
            bool seamlessGutters = static_cast<bool>(adjacentTile);
            if (adjacentTile) {
                const int coreWidth = readyTile->span.physicalWidth();
                for (int y = 0; y < readyTile->image.height(); ++y) {
                    seamlessGutters = seamlessGutters
                        && readyTile->image.pixel(
                               detailImageX(coreWidth - 1), y)
                            == adjacentTile->image.pixel(0, y)
                        && readyTile->image.pixel(
                               detailImageX(coreWidth), y)
                            == adjacentTile->image.pixel(detailImageX(0), y);
                }
            }
            ok &= require(seamlessGutters,
                          "linear filtering saw a clamped tile-edge seam");
        }
    }

    // Regression: the coarse whole-track fallback overview must never be shown
    // magnified into fake detail. It carries a fixed texel count for the whole
    // track, so at ordinary deck zoom levels stretching it across the timeline
    // turns each aggregated sample into a broad smeared block — the "huge wide
    // blocks visible right after load, correct waveform only much later"
    // symptom. Only a view where the whole track roughly fits the viewport may
    // display it.
    {
        constexpr std::uint32_t fallbackTexels = 2048;
        // A 7-minute track at the canonical 1200 lines/s.
        constexpr std::uint32_t sevenMinuteLines = 7 * 60 * 1200;

        // Default deck zoom: ~54x magnification. Must be suppressed.
        const double deckZoomMagnification =
            waveform_render::fallbackOverviewMagnification(
                fallbackTexels, sevenMinuteLines, 0.22, 1.0);
        ok &= require(deckZoomMagnification > 10.0,
                      "test fixture no longer reproduces the magnified fallback");
        ok &= require(!waveform_render::fallbackOverviewMayRepresentDetail(
                          deckZoomMagnification),
                      "hugely magnified whole-track overview must not stand in for detail");

        // Fully zoomed out far enough that the track fits the texel budget.
        const double zoomedOutMagnification =
            waveform_render::fallbackOverviewMagnification(
                fallbackTexels, sevenMinuteLines,
                static_cast<double>(fallbackTexels)
                    / static_cast<double>(sevenMinuteLines),
                1.0);
        ok &= require(waveform_render::fallbackOverviewMayRepresentDetail(
                          zoomedOutMagnification),
                      "1:1 whole-track overview must remain usable as context");

        // DPR participates: the same logical zoom on a 2x display doubles the
        // physical stretch, so the budget has to be evaluated in physical px.
        const double retinaMagnification =
            waveform_render::fallbackOverviewMagnification(
                fallbackTexels, sevenMinuteLines,
                static_cast<double>(fallbackTexels)
                    / static_cast<double>(sevenMinuteLines),
                2.0);
        ok &= require(retinaMagnification > zoomedOutMagnification,
                      "device pixel ratio must scale fallback magnification");

        // Degenerate inputs must never report a usable fallback.
        ok &= require(!waveform_render::fallbackOverviewMayRepresentDetail(
                          waveform_render::fallbackOverviewMagnification(
                              0, sevenMinuteLines, 0.22, 1.0))
                      && !waveform_render::fallbackOverviewMayRepresentDetail(
                          waveform_render::fallbackOverviewMagnification(
                              fallbackTexels, 0, 0.22, 1.0))
                      && !waveform_render::fallbackOverviewMayRepresentDetail(
                          waveform_render::fallbackOverviewMagnification(
                              fallbackTexels, sevenMinuteLines, 0.0, 1.0)),
                      "degenerate fallback geometry must never be displayed");
    }

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
            220'000, 256, 1000, 0, 1};
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
    ok &= require(waveform_render::viewKeyFor(requiredKeys[0])
                      != waveform_render::viewKeyFor(*displayedKeys[0]),
                  "old and new zoom configurations shared one ViewKey");

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
