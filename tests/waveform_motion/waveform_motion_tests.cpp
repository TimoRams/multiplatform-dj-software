#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
struct VisualAnchor {
    double position = 0.0;
    double rate = 1.0;
    double rateCorrection = 0.0;
    double elapsed = 0.0;
    bool reverse = false;

    double visual() const { return reverse ? position - elapsed * rate : position + elapsed * rate; }
    double reconcile(double authoritative, double delta)
    {
        elapsed += delta;
        const double predicted = visual();
        const double error = authoritative - predicted;
        const double direction = reverse ? -1.0 : 1.0;
        const double targetCorrection = std::clamp(
            direction * error / 0.75, -0.015, 0.015);
        rateCorrection += (targetCorrection - rateCorrection) * 0.08;
        position = predicted;
        rate = 1.0 + rateCorrection;
        elapsed = 0.0;
        return visual() - predicted;
    }
};
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main()
{
    bool ok = true;
    for (const bool reverse : {false, true}) {
        VisualAnchor anchor{.reverse = reverse};
        double authoritative = reverse ? 10.0 : 0.0;
        anchor.position = authoritative;
        double previous = authoritative;
        for (int tick = 0; tick < 1200; ++tick) {
            const double dt = (tick % 5 == 0) ? 0.0091 : 1.0 / 120.0;
            authoritative += reverse ? -dt : dt;
            const double blockPhaseNoise = static_cast<double>((tick % 7) - 3) * 0.0007;
            const double correctionJump = anchor.reconcile(
                authoritative + blockPhaseNoise, dt);
            const double now = anchor.visual();
            ok &= require(std::abs(correctionJump) < 1e-12,
                          "audio-clock reconciliation must not jump the visual position");
            ok &= require(reverse ? now <= previous + 1e-9 : now >= previous - 1e-9,
                          "continuous visual motion must not reverse direction");
            ok &= require(std::abs(anchor.rateCorrection) <= 0.015 + 1e-12,
                          "visual clock trim remains bounded");
            previous = now;
        }
    }
    return ok ? 0 : 1;
}
