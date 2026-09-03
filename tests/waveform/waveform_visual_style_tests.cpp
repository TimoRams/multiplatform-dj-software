#include "waveform/WaveformLineStore.h"
#include "waveform/WaveformVisualStyle.h"
#include "waveform/render/WaveformTileRasterizer.h"

#include <QCoreApplication>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

std::uint64_t imageSignature(const QImage& image)
{
    std::uint64_t signature = 1469598103934665603ULL;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            signature ^= row[x];
            signature *= 1099511628211ULL;
        }
    }
    return signature;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    using waveform_visual::WaveformRenderStyle;

    // Both styles consume the same shared-scale neutral values.  No component
    // is allowed to gain an independent per-band normalisation in mapping.
    const auto bassRgb = waveform_visual::map(
        WaveformRenderStyle::Rgb, {0.90f, 0.08f, 0.02f, 0.75f});
    const auto midRgb = waveform_visual::map(
        WaveformRenderStyle::Rgb, {0.04f, 0.90f, 0.06f, 0.75f});
    const auto highRgb = waveform_visual::map(
        WaveformRenderStyle::Rgb, {0.02f, 0.05f, 0.90f, 0.75f});
    ok &= require(bassRgb.components[0].color[0] > bassRgb.components[0].color[2],
                  "bass-dominant RGB must be red-dominant");
    ok &= require(midRgb.components[0].color[1] > midRgb.components[0].color[0]
                      && midRgb.components[0].color[1] > midRgb.components[0].color[2],
                  "mid-dominant RGB must be green-dominant");
    ok &= require(highRgb.components[0].color[2] > highRgb.components[0].color[0],
                  "treble-dominant RGB must be blue-dominant");

    const auto bassEq = waveform_visual::map(
        WaveformRenderStyle::EqColor, {0.90f, 0.08f, 0.02f, 0.75f});
    const auto midEq = waveform_visual::map(
        WaveformRenderStyle::EqColor, {0.04f, 0.90f, 0.06f, 0.75f});
    const auto highEq = waveform_visual::map(
        WaveformRenderStyle::EqColor, {0.02f, 0.05f, 0.90f, 0.75f});
    ok &= require(bassEq.components[0].color[0] > bassEq.components[0].color[2]
                      && bassEq.components[0].color[2] < 100,
                  "EQ Color must keep a weak high band from becoming blue");
    ok &= require(midEq.components[0].color[1] > midEq.components[0].color[2],
                  "EQ Color mid band must remain green-yellow");
    ok &= require(highEq.components[0].color[2] > highEq.components[0].color[0],
                  "EQ Color high band must remain cool blue");

    const auto threeBand = waveform_visual::map(
        WaveformRenderStyle::ThreeBand, {0.80f, 0.35f, 0.03f, 0.70f});
    ok &= require(threeBand.componentCount == 3
                      && threeBand.components[0].opacity == 0.03f
                      && threeBand.components[1].opacity == 0.35f
                      && threeBand.components[2].opacity == 0.80f,
                  "three-band mode must preserve each absolute band energy");
    ok &= require(threeBand.components[0].end <= threeBand.components[1].begin
                      && threeBand.components[1].end <= threeBand.components[2].begin,
                  "three-band components must occupy distinct visual lanes");

    WaveformLineStore store;
    constexpr std::uint32_t lineCount = WaveformLineStore::kChunkSize;
    store.reset(901, lineCount);
    auto lines = std::make_shared<std::vector<WaveformLine>>(lineCount);
    for (auto& line : *lines) {
        line.minimum = -18000;
        line.maximum = 21000;
        line.rms = 190;
        line.bass = 225;
        line.mid = 80;
        line.treble = 18;
        line.flags = waveform_line_flags::kAvailable | waveform_line_flags::kFinal;
    }
    ok &= require(store.publish({901, 0, 0, lineCount, lineCount, std::move(lines)})
                      == WaveformLineStore::PublishResult::Accepted,
                  "style fixture must publish neutral source lines");
    const auto snapshot = store.snapshot();
    const auto span = waveform_render::renderTileSpan(0, 1.0, lineCount);
    const auto rgbKey = waveform_render::WaveformTileRasterizer::makeKey(
        *snapshot, 0, span, 1.0, 96, 1.0, 0, 0xff101114u,
        WaveformRenderStyle::Rgb);
    const auto threeKey = waveform_render::WaveformTileRasterizer::makeKey(
        *snapshot, 0, span, 1.0, 96, 1.0, 0, 0xff101114u,
        WaveformRenderStyle::ThreeBand);
    const auto eqKey = waveform_render::WaveformTileRasterizer::makeKey(
        *snapshot, 0, span, 1.0, 96, 1.0, 0, 0xff101114u,
        WaveformRenderStyle::EqColor);
    ok &= require(rgbKey.sourceRevision == threeKey.sourceRevision
                      && rgbKey.sourceRevision == eqKey.sourceRevision
                      && rgbKey.renderStyle != threeKey.renderStyle
                      && rgbKey.renderStyle != eqKey.renderStyle
                      && rgbKey.visualStyleRevision != threeKey.visualStyleRevision,
                  "style switch must change render identity, never source analysis identity");

    std::mutex mutex;
    std::condition_variable ready;
    waveform_render::WaveformTileRasterizer rasterizer([&] { ready.notify_all(); });
    rasterizer.setActiveTrackGeneration(snapshot->trackGeneration);
    const auto request = [&](const auto& key) {
        rasterizer.request({key, span, snapshot, 1.0, 96.0, 1.0, 0.0});
    };
    std::shared_ptr<const waveform_render::RasterizedRenderTile> rgbTile;
    std::shared_ptr<const waveform_render::RasterizedRenderTile> eqTile;
    std::shared_ptr<const waveform_render::RasterizedRenderTile> threeTile;
    const auto rgbTileStart = std::chrono::steady_clock::now();
    request(rgbKey);
    {
        std::unique_lock lock(mutex);
        ok &= require(ready.wait_for(lock, std::chrono::seconds(2), [&] {
                          rgbTile = rasterizer.find(rgbKey);
                          return static_cast<bool>(rgbTile);
                      }), "RGB visible tile must rasterize lazily");
    }
    const auto rgbTileUsec = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - rgbTileStart).count();
    const auto eqTileStart = std::chrono::steady_clock::now();
    request(eqKey);
    {
        std::unique_lock lock(mutex);
        ok &= require(ready.wait_for(lock, std::chrono::seconds(2), [&] {
                          eqTile = rasterizer.find(eqKey);
                          return static_cast<bool>(eqTile);
                      }), "EQ Color visible tile must rasterize lazily");
    }
    const auto eqTileUsec = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - eqTileStart).count();
    const auto threeTileStart = std::chrono::steady_clock::now();
    request(threeKey);
    {
        std::unique_lock lock(mutex);
        ok &= require(ready.wait_for(lock, std::chrono::seconds(2), [&] {
                          threeTile = rasterizer.find(threeKey);
                          return static_cast<bool>(threeTile);
                      }), "three-band visible tile must rasterize lazily");
    }
    const auto threeTileUsec = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - threeTileStart).count();
    ok &= require(rgbTile && eqTile && threeTile
                      && imageSignature(rgbTile->image) != imageSignature(threeTile->image),
                  "RGB and true three-band tiles must not share one recoloured image");
    ok &= require(rgbTile && eqTile
                      && imageSignature(rgbTile->image) != imageSignature(eqTile->image),
                  "EQ Color must use a distinct frequency interpretation");
    ok &= require(rasterizer.find(rgbKey) == rgbTile,
                  "switching back must reuse the already rendered RGB tile");

    auto overviewSamples = std::make_shared<std::vector<waveform_render::OverviewSample>>(64);
    for (auto& sample : *overviewSamples)
        sample = {0.72f, 0.88f, 0.22f, 0.03f};
    const waveform_render::OverviewRenderKey rgbOverview{
        snapshot->trackGeneration, 1001, 256, 80, 0, lineCount, lineCount,
        static_cast<std::uint8_t>(WaveformRenderStyle::Rgb),
        waveform_visual::revision(WaveformRenderStyle::Rgb)};
    const waveform_render::OverviewRenderKey threeOverview{
        snapshot->trackGeneration, 1001, 256, 80, 0, lineCount, lineCount,
        static_cast<std::uint8_t>(WaveformRenderStyle::ThreeBand),
        waveform_visual::revision(WaveformRenderStyle::ThreeBand)};
    const waveform_render::OverviewRenderKey eqOverview{
        snapshot->trackGeneration, 1001, 256, 80, 0, lineCount, lineCount,
        static_cast<std::uint8_t>(WaveformRenderStyle::EqColor),
        waveform_visual::revision(WaveformRenderStyle::EqColor)};
    std::shared_ptr<const waveform_render::RasterizedOverview> rgbOverviewImage;
    const auto rgbOverviewStart = std::chrono::steady_clock::now();
    rasterizer.requestOverview({rgbOverview, overviewSamples});
    {
        std::unique_lock lock(mutex);
        ok &= require(ready.wait_for(lock, std::chrono::seconds(2), [&] {
                          rgbOverviewImage = rasterizer.findOverview(rgbOverview);
                          return static_cast<bool>(rgbOverviewImage);
                      }), "RGB overview must rasterize");
    }
    const auto rgbOverviewUsec = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - rgbOverviewStart).count();
    const auto eqOverviewStart = std::chrono::steady_clock::now();
    rasterizer.requestOverview({eqOverview, overviewSamples});
    std::shared_ptr<const waveform_render::RasterizedOverview> eqOverviewImage;
    {
        std::unique_lock lock(mutex);
        ok &= require(ready.wait_for(lock, std::chrono::seconds(2), [&] {
                          eqOverviewImage = rasterizer.findOverview(eqOverview);
                          return static_cast<bool>(eqOverviewImage);
                      }), "EQ Color overview must rasterize");
    }
    const auto eqOverviewUsec = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - eqOverviewStart).count();
    const auto threeOverviewStart = std::chrono::steady_clock::now();
    rasterizer.requestOverview({threeOverview, overviewSamples});
    std::shared_ptr<const waveform_render::RasterizedOverview> threeOverviewImage;
    {
        std::unique_lock lock(mutex);
        ok &= require(ready.wait_for(lock, std::chrono::seconds(2), [&] {
                          threeOverviewImage = rasterizer.findOverview(threeOverview);
                          return static_cast<bool>(threeOverviewImage);
                      }), "three-band overview must rasterize");
    }
    const auto threeOverviewUsec = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - threeOverviewStart).count();
    ok &= require(rgbOverviewImage && threeOverviewImage
                      && imageSignature(rgbOverviewImage->image)
                          != imageSignature(threeOverviewImage->image),
                  "overview must use the same selected style interpretation as tiles");
    ok &= require(rgbOverviewImage && eqOverviewImage
                      && imageSignature(rgbOverviewImage->image)
                          != imageSignature(eqOverviewImage->image),
                  "EQ Color overview must use its style-specific render key");
    ok &= require(rasterizer.findOverview(rgbOverview) == rgbOverviewImage,
                  "switching back must reuse the rendered RGB overview");

    const auto stats = rasterizer.stats();
    std::cout << "waveform style benchmark: tile-rgb=" << rgbTileUsec
              << " us, tile-eq-color=" << eqTileUsec
              << " us, tile-three-band=" << threeTileUsec
              << " us, overview-rgb=" << rgbOverviewUsec
              << " us, overview-eq-color=" << eqOverviewUsec
              << " us, overview-three-band=" << threeOverviewUsec
              << " us, tile-cache=" << stats.cacheBytes << " bytes\n";

    return ok ? 0 : 1;
}
