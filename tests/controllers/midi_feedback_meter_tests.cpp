#include "controllers/midi/LevelMeterBallistics.h"
#include "controllers/flx10/Flx10TransportControls.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <cmath>
#include <iostream>

int main()
{
    for (const double value : {-0.05, -0.01, 0.0, 0.01, 0.05})
        assert(flx10::tempoNearZero(value));
    for (const double value : {-8.0, -0.051, 0.051, 8.0})
        assert(!flx10::tempoNearZero(value));
    assert(std::abs(LevelMeterBallistics::linearPeakToDb(1.0f)) < 0.001f);
    assert(std::abs(LevelMeterBallistics::linearPeakToDb(0.1f) + 20.0f) < 0.001f);

    LevelMeterBallistics meter;
    assert(meter.update(0.0f, 1.0 / 30.0) == 0);
    assert(meter.update(1.0f, 1.0 / 30.0) == 89); // 0 dBFS reference, immediate attack

    // The FLX10 owns release and peak hold: send downward changes immediately.
    assert(meter.update(0.1f, 1.0 / 120.0) == 57);
    assert(meter.update(0.0f, 1.0 / 120.0) == 0);
    assert(meter.update(0.1f, 1.0 / 120.0) == 57);
    assert(meter.update(std::pow(10.0f, -19.0f / 20.0f), 1.0 / 120.0) == 58);
    assert(meter.update(0.0f, 0.1) == 0);
    assert(meter.update(std::numeric_limits<float>::quiet_NaN(), 0.01) == 0);

    meter.reset();
    const auto minusTwentyDb = meter.update(0.1f, 1.0 / 30.0);
    assert(minusTwentyDb == 57);

    assert(LevelMeterBallistics::dbToPioneerMidi(-24.0f) == 50);
    assert(LevelMeterBallistics::dbToPioneerMidi(-18.0f) == 60);
    assert(LevelMeterBallistics::dbToPioneerMidi(-12.0f) == 70);
    assert(LevelMeterBallistics::dbToPioneerMidi(-6.0f) == 79);
    assert(LevelMeterBallistics::dbToPioneerMidi(0.0f) == 89);
    assert(LevelMeterBallistics::dbToPioneerMidi(3.0f) == 94);
    assert(LevelMeterBallistics::dbToPioneerMidi(6.0f) == 99);
    assert(LevelMeterBallistics::dbToPioneerMidi(9.0f) == 104);
    assert(LevelMeterBallistics::dbToPioneerMidi(12.0f) == 109);

    meter.reset();
    assert(meter.update(1.0f, 1.0 / 30.0) == 89);
    assert(meter.update(1.01f, 1.0 / 30.0) == 109);

    // The controller owns clip hold: send the current level again immediately.
    assert(meter.update(0.5f, 1.0 / 30.0) < 109);

    std::cout << "MIDI feedback meter tests passed\n";
    return 0;
}
