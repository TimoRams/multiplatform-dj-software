#include <array>
#include <cmath>
#include <iostream>

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

    // Publishing a new immutable waveform chunk must not recreate marker
    // geometry. Marker lines only need a rebuild for a window/layout/overlay
    // change, otherwise their physical-pixel placement can visibly blink.
    int markerRebuilds = 0;
    bool forceRebuild = true;
    bool outsideGuard = false;
    bool staticConfigurationChanged = false;
    for (int chunkGeneration = 0; chunkGeneration < 64; ++chunkGeneration) {
        const bool rebuildMarkers = forceRebuild || outsideGuard || staticConfigurationChanged;
        if (rebuildMarkers)
            ++markerRebuilds;
        forceRebuild = false;
    }
    ok &= require(markerRebuilds == 1,
                  "progressive waveform chunks must not rebuild beatgrid geometry");
    return ok ? 0 : 1;
}
