#include "app/WaveformZoomController.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
bool near(double a, double b) { return std::abs(a - b) < 1.0e-12; }
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main()
{
    bool ok = true;
    ok &= require(near(WaveformZoomController::validatedZoom(-1.0), WaveformZoomController::kMinimum),
                  "minimum clamp");
    ok &= require(near(WaveformZoomController::validatedZoom(99.0), 10.0), "maximum clamp");
    ok &= require(near(WaveformZoomController::validatedZoom(std::numeric_limits<double>::infinity()), 0.22),
                  "invalid persisted zoom falls back to default");
    ok &= require(near(WaveformZoomController::increasedZoom(1.0), 1.15), "exponential zoom in");
    ok &= require(near(WaveformZoomController::decreasedZoom(1.15), 1.0), "exponential zoom out");
    ok &= require(near(WaveformZoomController::increasedZoom(10.0), 10.0), "maximum is stable");
    ok &= require(near(WaveformZoomController::decreasedZoom(WaveformZoomController::kMinimum),
                       WaveformZoomController::kMinimum),
                  "minimum is stable");
    double value = WaveformZoomController::kDefault;
    for (int i = 0; i < 100; ++i) value = WaveformZoomController::increasedZoom(value);
    ok &= require(near(value, WaveformZoomController::kMaximum), "repeated shortcut reaches maximum");
    ok &= require(WaveformZoomController::lodLevelForPhysicalPixels(
                      WaveformZoomController::kMinimum) == 4,
                  "minimum zoom selects 75-lines-per-second LOD");
    ok &= require(WaveformZoomController::lodLevelForPhysicalPixels(0.22) == 3,
                  "default zoom selects 150-lines-per-second LOD");
    ok &= require(WaveformZoomController::lodLevelForPhysicalPixels(10.0) == 0,
                  "maximum zoom retains canonical 1200-lines-per-second detail");
    return ok ? 0 : 1;
}
