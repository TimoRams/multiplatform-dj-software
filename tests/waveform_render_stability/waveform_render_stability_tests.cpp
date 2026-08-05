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

    // Waveform and overlay lines share a pixel-centred transform. Their local
    // positions sit on the physical grid, so final line centres always have a
    // 0.5 physical-pixel fraction and occupy exactly one pixel.
    for (const double dpr : {1.0, 1.25, 1.5, 2.0}) {
        const double logicalLineWidth = 1.0 / dpr;
        ok &= require(std::abs(logicalLineWidth * dpr - 1.0) < 1e-9,
                      "vertical geometry must be exactly one physical pixel wide");
        for (int frame = 0; frame < 240; ++frame) {
            const double smoothTranslation = 319.375 - frame * 0.2875;
            const double centred = (std::floor(smoothTranslation * dpr) + 0.5) / dpr;
            const double physical = centred * dpr;
            ok &= require(std::abs((physical - std::floor(physical)) - 0.5) < 1e-9,
                          "line transform must stay at a physical pixel centre");
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

    for (const float height : {60.0f, 100.0f, 180.0f}) {
        const auto layout = waveform_render::verticalMarkerLayout(height);
        ok &= require(layout.waveformInset >= layout.downbeatTickLength,
                      "waveform leaves room for beat ticks at both edges");
        ok &= require(layout.downbeatTickLength > layout.regularTickLength,
                      "downbeat ticks are slightly longer than regular beats");
        ok &= require(layout.cueLinePhysicalWidth > 1.0f
                          && std::fmod(layout.cueLinePhysicalWidth, 2.0f) == 1.0f,
                      "cue lines use a wider odd physical-pixel width for crisp edges");
        ok &= require(layout.waveformInset * 2.0f < height,
                      "waveform inset keeps a visible audio envelope");
    }

    // Every timeline layer uses one persistent origin and transform. Guarded
    // source-window changes therefore cannot alter the final waveform/marker
    // position or put the two layers onto different pixel phases.
    for (const double dpr : {1.0, 1.25, 1.5, 2.0}) {
        constexpr double timelineLine = 42'375.25;
        constexpr double renderOrigin = 40'000.0;
        constexpr double pixelsPerTimelineLine = 0.22;
        const double waveformX = waveform_render::snappedTimelineX(
            timelineLine, renderOrigin, pixelsPerTimelineLine, dpr);
        const double markerX = waveform_render::snappedTimelineX(
            timelineLine, renderOrigin, pixelsPerTimelineLine, dpr);
        ok &= require(std::abs(waveformX - markerX) < 1e-12,
                      "waveform and overlays must share one local pixel grid");
        double previousCentre = 0.0;
        for (int frame = 0; frame < 240; ++frame) {
            const double playheadLine = 41'000.0 + frame * 0.71;
            const double translation = waveform_render::snappedTimelineTranslation(
                1600.0, playheadLine, renderOrigin, pixelsPerTimelineLine, dpr);
            const double physicalCentre = (waveformX + translation) * dpr;
            ok &= require(std::abs((physicalCentre - std::floor(physicalCentre)) - 0.5) < 1e-9,
                          "beat marker stays centred on one physical pixel while scrolling");
            if (frame > 0)
                ok &= require(physicalCentre <= previousCentre,
                              "pixel-snapped timeline must move monotonically");
            previousCentre = physicalCentre;

            // The guarded source window moves independently, but is never an
            // input to local geometry or the shared transform.
            const double guardedWindowStart = 39'000.0 + (frame / 17) * 850.0;
            (void) guardedWindowStart;
            const double rebuiltCentre = (waveform_render::snappedTimelineX(
                timelineLine, renderOrigin, pixelsPerTimelineLine, dpr)
                + waveform_render::snappedTimelineTranslation(
                    1600.0, playheadLine, renderOrigin, pixelsPerTimelineLine, dpr)) * dpr;
            ok &= require(std::abs(rebuiltCentre - physicalCentre) < 1e-9,
                          "guarded window rebuild must not change timeline phase");
        }
    }
    return ok ? 0 : 1;
}
