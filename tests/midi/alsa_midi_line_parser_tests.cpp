#include "midi/AlsaMidiLineParser.h"

#include <iostream>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool matches(const QString& line, int channel, int control, int value)
{
    const auto event = midi_internal::parseAlsaControlChange(line);
    return event.valid
        && event.channel == channel
        && event.control == control
        && event.value == value;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= require(matches(
        QStringLiteral("128:0 Control change 0, controller 0, value 63"), 0, 0, 63),
        "standard aseqdump tempo MSB is decoded");
    ok &= require(matches(
        QStringLiteral("Chan 2, Control change (177 32 127)"), 1, 32, 127),
        "parenthesized PipeWire tempo LSB uses the MIDI status channel");
    ok &= require(matches(
        QStringLiteral("28939.80s: Chan 2 Control/Mode Change (177 32 25)"), 1, 32, 25),
        "observed FLX10 monitor format is decoded");
    ok &= require(matches(
        QStringLiteral("Channel 2 Control change, controller 32, value 12"), 1, 32, 12),
        "verbose one-based channel is normalized");
    ok &= require(matches(
        QStringLiteral("Control change 1, 32, 65"), 1, 32, 65),
        "numeric fallback keeps the final channel/control/value triple");
    ok &= require(!midi_internal::parseAlsaControlChange(QStringLiteral("Control change")).valid,
                  "incomplete control changes are rejected");
    return ok ? 0 : 1;
}
