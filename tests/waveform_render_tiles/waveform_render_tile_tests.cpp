#include "rendering/WaveformTileRasterizer.h"
#include "waveform/WaveformLineStore.h"

#include <QCoreApplication>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

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
        snapshot->trackGeneration, 1234, 512, 128};
    rasterizer.requestOverview({overviewKey, std::move(overviewSamples)});
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
    ok &= require(waveform_render::bestAvailableCoverage(false, true)
                      == waveform_render::WaveformCoverage::Fallback,
                  "missing high-resolution data must retain fallback coverage");
    ok &= require(waveform_render::bestAvailableCoverage(true, true)
                      == waveform_render::WaveformCoverage::HighResolution,
                  "ready high-resolution tiles must cover the fallback");
    return ok ? 0 : 1;
}
