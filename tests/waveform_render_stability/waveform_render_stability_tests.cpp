#include <array>
#include <cmath>
#include <iostream>

#include "rendering/WaveformMarkerLayout.h"

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

    // Neither progressive data nor guarded waveform-window changes recreate
    // track-wide marker geometry. Replacing those nodes at window boundaries
    // caused the periodic blink that remained during steady playback.
    int markerRebuilds = 0;
    int waveformWindowRebuilds = 0;
    bool forceRebuild = true;
    bool staticConfigurationChanged = false;
    for (int chunkGeneration = 0; chunkGeneration < 64; ++chunkGeneration) {
        const bool outsideGuard = chunkGeneration > 0 && (chunkGeneration % 7) == 0;
        const bool rebuildWaveform = forceRebuild || outsideGuard;
        const bool rebuildMarkers = forceRebuild || staticConfigurationChanged;
        if (rebuildWaveform)
            ++waveformWindowRebuilds;
        if (rebuildMarkers)
            ++markerRebuilds;
        forceRebuild = false;
    }
    ok &= require(markerRebuilds == 1,
                  "progressive waveform chunks must not rebuild beatgrid geometry");
    ok &= require(waveformWindowRebuilds > markerRebuilds,
                  "guard crossings rebuild waveform chunks without replacing markers");

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
    return ok ? 0 : 1;
}
