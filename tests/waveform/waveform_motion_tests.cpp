#include "audio/AudioPresentationClock.h"
#include "waveform/render/WaveformRenderMath.h"

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
    for (const double speed : {0.5, 1.0, 1.5}) {
        for (const double direction : {-1.0, 1.0}) {
            AudioPresentationClock first, second, shifted;
            const double rateA = direction * speed;
            const double rateB = rateA * 0.5;
            double positionA = 100.0, positionB = 50.0;
            double time = 1000.0;
            for (int frame = 0; frame < 7200; ++frame) {
                const double dt = (frame % 5 == 0) ? 0.0091 : 1.0 / 120.0;
                time += dt;
                positionA += dt * rateA;
                positionB += dt * rateB;
                first.publish({positionA, time, rateA, 1000.0});
                second.publish({positionB, time, rateB, 1000.0});
                shifted.publish({positionB + 0.1, time, rateB, 1000.0});
                const double now = time + (frame % 7) * 0.0007;
                const double visualA = first.snapshot().positionAt(now, 0.025);
                const double visualB = second.snapshot().positionAt(now, 0.025);
                // 80 BPM at speed, 160 BPM at half that speed: identical
                // output BPM, with unrelated source-time coordinates.
                const double beatA = visualA * 80.0 / 60.0;
                const double beatB = visualB * 160.0 / 60.0;
                ok &= require(std::abs(beatA - beatB) < 1e-9,
                              "equal output BPM keeps aligned beats at identical phase");
                const double ppsA = waveform_render::timelinePixelsPerSecond(0.22, 1200.0, speed);
                const double ppsB = waveform_render::timelinePixelsPerSecond(0.22, 1200.0, speed * 0.5);
                ok &= require(std::abs(60.0 / 80.0 * ppsA - 60.0 / 160.0 * ppsB) < 1e-9,
                              "slower/faster tracks at equal BPM have identical pixel spacing");
                const double offsetB = shifted.snapshot().positionAt(now, 0.025);
                ok &= require(std::abs((offsetB - visualB) - 0.1) < 1e-12,
                              "manual phase offset is not pulled into alignment");
            }
        }
    }
    AudioPresentationPosition looping {0.1, 1.0, 1.0, 10.0, 0.5, 1.0};
    ok &= require(std::abs(looping.positionAt(1.01) - 0.11) < 1e-12,
                  "playback before loop-in is not prematurely wrapped");
    looping.seconds = 0.99;
    ok &= require(std::abs(looping.positionAt(1.02) - 0.51) < 1e-12,
                  "forward loop wrap is projected at the audio boundary");
    looping.seconds = 0.51;
    ok &= require(std::abs(looping.positionAt(1.0, 0.02) - 0.99) < 1e-12,
                  "audible latency projects back through a loop boundary");
    looping.rate = -1.0;
    ok &= require(std::abs(looping.positionAt(1.02) - 0.99) < 1e-12,
                  "reverse loop wrap follows the same audio timeline");
    AudioPresentationPosition paused {2.0, 1.0, 0.0, 10.0};
    ok &= require(paused.positionAt(100.0, 0.025) == 2.0,
                  "paused clock remains exactly stationary");
    AudioPresentationPosition stalled {2.0, 1.0, 1.0, 10.0};
    ok &= require(stalled.positionAt(100.0) == 2.05,
                  "missing callbacks cannot free-run indefinitely");
    return ok ? 0 : 1;
}
