#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
struct VisualAnchor {
    double position = 0.0;
    double rate = 1.0;
    double elapsed = 0.0;
    bool reverse = false;

    double visual() const { return reverse ? position - elapsed * rate : position + elapsed * rate; }
    void reconcile(double authoritative, double delta)
    {
        elapsed += delta;
        const double predicted = visual();
        const double error = authoritative - predicted;
        const double correction = std::clamp(error * 0.20, -0.0025, 0.0025);
        position = predicted + correction;
        elapsed = 0.0;
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
            anchor.reconcile(authoritative, dt);
            const double now = anchor.visual();
            ok &= require(reverse ? now <= previous + 1e-9 : now >= previous - 1e-9,
                          "continuous visual motion must not reverse direction");
            previous = now;
        }
    }
    return ok ? 0 : 1;
}
