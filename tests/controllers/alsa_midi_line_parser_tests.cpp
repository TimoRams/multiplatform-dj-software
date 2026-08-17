#include "controllers/midi/AlsaMidiLineParser.h"
#include "controllers/midi/MidiEchoGuard.h"

#include <iostream>
#include <set>

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
    ok &= require(midi_internal::shouldAcceptAlsaChannelFaderSource(
                      QStringLiteral("20:0"), QStringLiteral("20:0"), 0x13),
                  "channel-fader MSB from selected ALSA port is accepted");
    ok &= require(!midi_internal::shouldAcceptAlsaChannelFaderSource(
                      QStringLiteral("20:1"), QStringLiteral("20:0"), 0x33),
                  "channel-fader LSB from secondary ALSA port is ignored");
    ok &= require(midi_internal::shouldAcceptAlsaChannelFaderSource(
                      QStringLiteral("20:1"), QStringLiteral("20:0"), 0x20),
                  "secondary ALSA ports remain active for non-fader controls");

    // One press mirrored onto two ports must count once, or every latching
    // button switches itself straight back off.
    {
        midi_internal::AlsaCrossPortButtonFilter filter;
        constexpr int kFxOn = 10000 + 4 * 2000 + 0x46;
        ok &= require(filter.accept(QStringLiteral("20:0"), kFxOn, 127, 10.000),
                      "first press is accepted");
        ok &= require(!filter.accept(QStringLiteral("20:1"), kFxOn, 127, 10.002),
                      "same press mirrored on a second port is dropped");
        ok &= require(filter.accept(QStringLiteral("20:0"), kFxOn, 0, 10.060),
                      "the release is a different value and gets through");
        ok &= require(!filter.accept(QStringLiteral("20:1"), kFxOn, 0, 10.061),
                      "the mirrored release is dropped too");
        ok &= require(filter.accept(QStringLiteral("20:0"), kFxOn, 127, 10.300),
                      "a genuine second press later on is accepted");
        ok &= require(filter.accept(QStringLiteral("20:0"), kFxOn, 127, 10.302),
                      "a repeat from the same port is never treated as a mirror");
        ok &= require(filter.accept(QStringLiteral("20:1"), kFxOn, 127, 10.400),
                      "a copy outside the window is a real event");
    }

    // A lamp write that loops back in must not be read as a button press.
    {
        midi_internal::MidiOutputEchoGuard guard;
        constexpr int kFxOn = 10000 + 4 * 2000 + 0x46;
        ok &= require(!guard.isEcho(kFxOn, 127, 5.000),
                      "a press with no preceding lamp write is a real event");
        guard.noteSent(kFxOn, 127, 5.100);
        ok &= require(guard.isEcho(kFxOn, 127, 5.101),
                      "the lamp write coming straight back is dropped");
        ok &= require(!guard.isEcho(kFxOn, 127, 5.102),
                      "one write can only be echoed once");
        guard.noteSent(kFxOn, 127, 5.200);
        ok &= require(!guard.isEcho(kFxOn, 127, 5.400),
                      "a press well after the write is a real event");
        guard.noteSent(kFxOn, 127, 5.500);
        ok &= require(!guard.isEcho(kFxOn, 64, 5.501),
                      "a different velocity is not our write");
        ok &= require(!guard.isEcho(kFxOn, 0, 5.501),
                      "releases are never dropped");
    }
    {
        // Switching a lamp off must not shadow a press arriving right after it.
        midi_internal::MidiOutputEchoGuard guard;
        constexpr int kFxOn = 10000 + 4 * 2000 + 0x46;
        guard.noteSent(kFxOn, 0, 6.000);
        ok &= require(!guard.isEcho(kFxOn, 127, 6.001),
                      "a lamp-off write casts no shadow over a press");
    }

    const std::set<int> mappedIds { 11019, 13019 };
    ok &= require(midi_internal::resolveMappedAlsaMessageId(13019, 1019, mappedIds) == 13019,
                  "mapped FLX10 channel 2 resolves exactly");
    ok &= require(midi_internal::resolveMappedAlsaMessageId(15019, 1019, mappedIds) == 15019,
                  "unmapped channel 3 cannot alias onto mapped channel 2");
    return ok ? 0 : 1;
}
