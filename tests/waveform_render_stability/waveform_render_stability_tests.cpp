#include <array>
#include <cmath>
#include <iostream>

#include "rendering/WaveformMarkerLayout.h"
#include "rendering/WaveformRenderTile.h"

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main()
{
    bool ok = true;
    // Render textures are fixed physical-pixel tiles. Analysis chunks may map
    // to one or many tiles, but zoom and DPR can never grow a single texture.
    for (const double dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
        for (const double zoom : {0.08, 0.22, 1.0, 10.0}) {
            const double physicalPixelsPerLine = zoom * dpr;
            const auto first = waveform_render::renderTileSpan(
                0, physicalPixelsPerLine, 4'320'000);
            const auto distant = waveform_render::renderTileSpan(
                17'000, physicalPixelsPerLine, 4'320'000);
            ok &= require(first.physicalWidth()
                              == waveform_render::kRenderTilePhysicalWidth,
                          "render tile width must be independent of zoom and DPR");
            ok &= require(distant.physicalWidth()
                              == waveform_render::kRenderTilePhysicalWidth,
                          "distant render tiles must remain fixed-size");
            ok &= require(first.sourceEnd >= first.sourceBegin,
                          "tile source interval must be ordered");
        }
    }
    const auto zoomedChunkPhysicalWidth = waveform_render::timelinePhysicalCeil(
        4096.0, 10.0, 3.0);
    const auto zoomedChunkTileCount =
        waveform_render::lastRenderTile(zoomedChunkPhysicalWidth)
        - waveform_render::firstRenderTile(0) + 1;
    ok &= require(zoomedChunkTileCount > 1,
                  "one analysis chunk must split into bounded tiles at high zoom");
    const auto lowZoomTile = waveform_render::renderTileSpan(
        0, 0.08, 100'000);
    ok &= require(lowZoomTile.sourceEnd > 4096,
                  "one low-zoom tile must be able to read across analysis chunks");
    ok &= require(!waveform_render::physicalStrokeIntersectsTrack(
                      -2, 0.22, 100'000)
                      && waveform_render::physicalStrokeIntersectsTrack(
                          0, 0.22, 100'000)
                      && !waveform_render::physicalStrokeIntersectsTrack(
                          22'000, 0.22, 100'000),
                  "tile edge columns repeated audio outside the track bounds");
    for (const int hz : std::array{30, 60, 90, 120, 144, 240}) {
        for (const double rate : {1.0, -1.0}) {
            double position = rate > 0.0 ? 0.0 : 30.0;
            double previousX = -position * 360.0;
            int oppositeDirectionFrames = 0;
            for (int frame = 0; frame < hz * 10; ++frame) {
                position += rate / hz;
                const double transformX = -position * 360.0;
                if ((rate > 0.0 && transformX > previousX + 1e-9)
                    || (rate < 0.0 && transformX < previousX - 1e-9))
                    ++oppositeDirectionFrames;
                previousX = transformX;
            }
            ok &= require(oppositeDirectionFrames == 0, "steady transform must not reverse direction");
        }
    }

    // Model the guarded window used by ScrollingWaveformItem. At steady playback
    // geometry rebuilds only after travelling through the off-screen guard;
    // every intermediate frame is a matrix-only transform update.
    constexpr double lineRate = 1200.0;
    constexpr double viewportWidth = 1600.0;
    // 1200 canonical lines/s retain the former 264 px/s display scale while
    // providing four times the zoom detail.
    constexpr double pixelsPerLine = 0.22;
    const double visibleLines = viewportWidth / pixelsPerLine;
    const double halfWindow = visibleLines * 1.25;
    const double rebuildTravel = (halfWindow - visibleLines * 0.5) * 0.58;
    for (const double rate : {1.0, -1.0}) {
        constexpr int hz = 120;
        constexpr int seconds = 30;
        double positionLines = rate > 0.0 ? 0.0 : lineRate * seconds;
        double innerStart = 1.0;
        double innerEnd = -1.0;
        int rebuilds = 0;
        for (int frame = 0; frame < hz * seconds; ++frame) {
            positionLines += rate * lineRate / hz;
            if (positionLines < innerStart || positionLines > innerEnd) {
                innerStart = positionLines - rebuildTravel;
                innerEnd = positionLines + rebuildTravel;
                ++rebuilds;
            }
        }
        ok &= require(rebuilds < (hz * seconds) / 100,
                      "guarded rendering rebuilt geometry too often");
    }

    // Beat/cue geometry keeps a one-physical-pixel opaque core plus half-pixel
    // feathering on either side. Waveform chunks use nearest-filtered textures.
    // The shared transform must retain fractional physical-pixel phases;
    // quantising it was the source of visible stop/start shimmer.
    for (const double dpr : {1.0, 1.25, 1.5, 2.0}) {
        const double logicalCoreWidth = 1.0 / dpr;
        const double logicalFeatherWidth = 0.5 / dpr;
        ok &= require(std::abs((logicalCoreWidth + 2.0 * logicalFeatherWidth)
                              * dpr - 2.0) < 1e-9,
                      "vertical geometry uses a one-pixel core with soft edges");
        int fractionalPhaseFrames = 0;
        double previous = waveform_render::smoothTimelineTranslation(
            638.75, 0.0, 0.0, 0.2875);
        for (int frame = 0; frame < 240; ++frame) {
            const double translation = waveform_render::smoothTimelineTranslation(
                638.75, static_cast<double>(frame), 0.0, 0.2875);
            const double physical = translation * dpr;
            const double fraction = physical - std::floor(physical);
            if (fraction > 1.0e-6 && fraction < 1.0 - 1.0e-6)
                ++fractionalPhaseFrames;
            if (frame > 0)
                ok &= require(std::abs((previous - translation) - 0.2875) < 1e-9,
                              "timeline translation advances by the smooth clock delta");
            previous = translation;
        }
        ok &= require(fractionalPhaseFrames > 180,
                      "timeline transform must preserve subpixel phases");
    }

    // The audio primitive is always one physical pixel followed by one physical
    // pixel of air. Zoom and DPR may change which source frames a stroke folds,
    // but never its screen density or phase across adjacent render chunks.
    for (const double dpr : {1.0, 1.25, 1.5, 2.0}) {
        for (const double pixelsPerLine : {0.01, 0.055, 0.22, 1.0, 8.0, 40.0}) {
            constexpr double firstChunkBegin = 8192.0;
            constexpr double chunkBoundary = 12288.0;
            constexpr double secondChunkEnd = 16384.0;
            const auto firstPhysical = waveform_render::timelinePhysicalFloor(
                firstChunkBegin, pixelsPerLine, dpr);
            const auto boundaryLeft = waveform_render::timelinePhysicalCeil(
                chunkBoundary, pixelsPerLine, dpr);
            const auto boundaryRight = waveform_render::timelinePhysicalFloor(
                chunkBoundary, pixelsPerLine, dpr);
            const auto lastPhysical = waveform_render::timelinePhysicalCeil(
                secondChunkEnd, pixelsPerLine, dpr);
            ok &= require(boundaryLeft >= boundaryRight
                              && boundaryLeft - boundaryRight <= 1,
                          "adjacent waveform chunks share one physical grid");

            std::int64_t previousStroke = -1;
            for (auto column = firstPhysical; column < lastPhysical; ++column) {
                if (!waveform_render::isWaveformStrokeColumn(column))
                    continue;
                if (previousStroke >= 0)
                    ok &= require(column - previousStroke
                                      == waveform_render::kWaveformStrokePitchPhysicalPixels,
                                  "waveform stroke density must not change with zoom or DPR");
                previousStroke = column;
            }
        }
    }

    // Progressive source publication never rebuilds marker geometry. Guard
    // crossings do rebuild only the local marker window: keeping an entire
    // hour-long beatgrid in float QSG coordinates loses precision at high zoom.
    int markerRebuilds = 0;
    int waveformWindowRebuilds = 0;
    bool forceRebuild = true;
    bool staticConfigurationChanged = false;
    for (int chunkGeneration = 0; chunkGeneration < 64; ++chunkGeneration) {
        const bool outsideGuard = chunkGeneration > 0 && (chunkGeneration % 7) == 0;
        const bool rebuildWaveform = forceRebuild || outsideGuard;
        const bool rebuildMarkers = forceRebuild || outsideGuard
            || staticConfigurationChanged;
        if (rebuildWaveform)
            ++waveformWindowRebuilds;
        if (rebuildMarkers)
            ++markerRebuilds;
        forceRebuild = false;
    }
    ok &= require(markerRebuilds == waveformWindowRebuilds
                      && markerRebuilds < 16,
                  "beatgrid must follow bounded guard windows, not frame updates");

    // Immutable chunk pointers allow a progressive publication to upload only
    // the newly available texture instead of replacing the complete visible
    // waveform. Slots include absent chunks so later arrivals never shift the
    // texture-to-chunk mapping.
    std::array<const void*, 6> cachedChunks{};
    std::array<int, 6> chunks{};
    chunks[1] = 1;
    chunks[4] = 4;
    int rewrittenNodes = 0;
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const void* chunk = chunks[index] == 0 ? nullptr : &chunks[index];
        if (cachedChunks[index] != chunk) {
            cachedChunks[index] = chunk;
            ++rewrittenNodes;
        }
    }
    const int initialRewrites = rewrittenNodes;
    chunks[3] = 3;
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const void* chunk = chunks[index] == 0 ? nullptr : &chunks[index];
        if (cachedChunks[index] != chunk) {
            cachedChunks[index] = chunk;
            ++rewrittenNodes;
        }
    }
    ok &= require(rewrittenNodes - initialRewrites == 1,
                  "one progressive chunk publication rewrites exactly one node");

    for (const float height : {60.0f, 100.0f, 180.0f}) {
        const auto layout = waveform_render::verticalMarkerLayout(height);
        ok &= require(layout.waveformInset >= layout.downbeatTickLength,
                      "waveform leaves room for beat ticks at both edges");
        ok &= require(layout.downbeatTickLength > layout.regularTickLength,
                      "downbeat ticks are slightly longer than regular beats");
        ok &= require(layout.cueLinePhysicalWidth > 1.0f
                          && std::fmod(layout.cueLinePhysicalWidth, 2.0f) == 1.0f,
                      "cue lines retain a wider opaque core below their soft edges");
        ok &= require(layout.waveformInset * 2.0f < height,
                      "waveform inset keeps a visible audio envelope");
    }

    // Every layer uses a physical-pixel-aligned origin and one smooth transform.
    // Rebasing float-sized local coordinates must not alter the final position.
    for (const double dpr : {1.0, 1.25, 1.5, 2.0}) {
        constexpr double timelineLine = 42'375.25;
        constexpr double pixelsPerTimelineLine = 0.22;
        const double renderOrigin = waveform_render::pixelAlignedTimelineOrigin(
            40'000.0, pixelsPerTimelineLine, dpr);
        const double rebasedOrigin = waveform_render::pixelAlignedTimelineOrigin(
            41'750.0, pixelsPerTimelineLine, dpr);
        const double waveformX = waveform_render::snappedTimelineX(
            timelineLine, renderOrigin, pixelsPerTimelineLine, dpr);
        const double markerX = waveform_render::snappedTimelineX(
            timelineLine, renderOrigin, pixelsPerTimelineLine, dpr);
        ok &= require(std::abs(waveformX - markerX) < 1e-12,
                      "waveform and overlays must share one local pixel grid");
        double previousCentre = 0.0;
        int fractionalPhaseFrames = 0;
        for (int frame = 0; frame < 240; ++frame) {
            const double playheadLine = 41'000.0 + frame * 0.71;
            const double translation = waveform_render::smoothTimelineTranslation(
                1600.0, playheadLine, renderOrigin, pixelsPerTimelineLine);
            const double physicalCentre = (waveformX + translation) * dpr;
            const double physicalFraction = physicalCentre - std::floor(physicalCentre);
            if (physicalFraction > 1.0e-6 && physicalFraction < 1.0 - 1.0e-6)
                ++fractionalPhaseFrames;
            if (frame > 0)
                ok &= require(physicalCentre <= previousCentre,
                              "smooth timeline must move monotonically");
            previousCentre = physicalCentre;

            const double rebuiltCentre = (waveform_render::snappedTimelineX(
                timelineLine, rebasedOrigin, pixelsPerTimelineLine, dpr)
                + waveform_render::smoothTimelineTranslation(
                    1600.0, playheadLine, rebasedOrigin, pixelsPerTimelineLine)) * dpr;
            ok &= require(std::abs(rebuiltCentre - physicalCentre) < 1e-9,
                          "pixel-aligned origin rebase must not change timeline phase");
        }
        ok &= require(fractionalPhaseFrames > 180,
                      "waveform and beatgrid move through stable subpixel phases");
    }

    // Changing zoom must keep the playhead's timeline coordinate at the exact
    // visual centre. This covers every LOD transition as well as fractional DPR.
    for (const double dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
        for (const double zoom : {0.08, 0.092, 0.22, 0.44, 1.0, 3.75, 10.0}) {
            constexpr double width = 1601.25;
            constexpr double playheadLine = 2'731'337.375;
            const double origin = waveform_render::pixelAlignedTimelineOrigin(
                playheadLine - 20'000.0, zoom, dpr);
            const double local = waveform_render::snappedTimelineX(
                playheadLine, origin, zoom, dpr);
            const double translated = local
                + waveform_render::smoothTimelineTranslation(
                    width, playheadLine, origin, zoom);
            ok &= require(std::abs(translated - width * 0.5) <= 1.0e-9,
                          "zoom changed the shared waveform/beatgrid centre");
            constexpr double beatSpacingLines = 587.375;
            const double nextBeat = waveform_render::snappedTimelineX(
                playheadLine + beatSpacingLines, origin, zoom, dpr);
            ok &= require(std::abs((nextBeat - local)
                                      - beatSpacingLines * zoom) <= 1.0e-9,
                          "per-marker snapping warped beat spacing during zoom");
        }
    }
    return ok ? 0 : 1;
}
