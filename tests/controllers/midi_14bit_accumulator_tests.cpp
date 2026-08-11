#include "controllers/midi/Midi14BitAccumulator.h"

#include <iostream>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    bool ok = true;
    midi_internal::Midi14BitAccumulator accumulator;

    accumulator.pushLsb(25);
    ok &= require(!accumulator.takeValue().has_value(),
                  "FLX10 LSB waits for its matching MSB");
    accumulator.pushMsb(81);
    const auto first = accumulator.takeValue();
    ok &= require(first && *first == ((81 << 7) | 25),
                  "observed FLX10 LSB-first pair is combined coherently");
    ok &= require(!accumulator.takeValue().has_value(),
                  "a published pair cannot be reused by the next event");

    accumulator.pushMsb(80);
    ok &= require(!accumulator.takeValue().has_value(),
                  "MSB-first delivery also waits for its matching half");
    accumulator.pushLsb(127);
    const auto second = accumulator.takeValue();
    ok &= require(second && *second == ((80 << 7) | 127),
                  "MSB-first controllers remain supported");

    midi_internal::MidiUnpairedMsbGate fallbackGate;
    ok &= require(!fallbackGate.shouldPublish(0),
                  "first unpaired startup MSB is held");
    ok &= require(!fallbackGate.shouldPublish(0),
                  "repeated startup MSB is not mistaken for movement");
    ok &= require(fallbackGate.shouldPublish(1),
                  "a changed MSB enables coarse fallback");
    ok &= require(fallbackGate.shouldPublish(1),
                  "coarse fallback stays enabled after movement");

    midi_internal::MidiUnpairedMsbGate pairedGate;
    pairedGate.confirmPair();
    ok &= require(pairedGate.shouldPublish(127),
                  "a coherent 14-bit pair immediately confirms the control");

    return ok ? 0 : 1;
}
