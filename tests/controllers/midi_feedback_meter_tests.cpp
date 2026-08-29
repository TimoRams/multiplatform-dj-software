#include "controllers/midi/feedback/LevelMeterBallistics.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
    assert(std::abs(LevelMeterBallistics::linearPeakToDb(1.0f)) < 0.001f);
    assert(std::abs(LevelMeterBallistics::linearPeakToDb(0.1f) + 20.0f) < 0.001f);

    LevelMeterBallistics meter;
    assert(meter.update(0.0f, 1.0 / 30.0) == 0);
    assert(meter.update(1.0f, 1.0 / 30.0) == 89); // 0 dBFS reference, immediate attack

    const auto firstRelease = meter.update(0.0f, 1.0 / 30.0);
    assert(firstRelease >= 87); // release is deliberately slower than attack
    for (int tick = 0; tick < 60; ++tick)
        (void)meter.update(0.0f, 1.0 / 30.0);
    assert(meter.update(0.0f, 1.0 / 30.0) == 0);

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

    std::cout << "MIDI feedback meter tests passed\n";
    return 0;
}
