#include "controllers/flx10/Flx10Protocol.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using flx10_protocol::jogPhaseBytes;
using flx10_protocol::kJogPhaseTicksPerRevolution;
using flx10_protocol::kJogPhaseTicksPerSecond;
using flx10_protocol::kJogRevolutionSeconds;

void basicEncoding()
{
    assert(jogPhaseBytes(0.0).value() == 0);
    assert(jogPhaseBytes(0.128).value() == 256);
    assert(jogPhaseBytes(0.5).value() == 1000);
    assert(jogPhaseBytes(kJogRevolutionSeconds).value() == 0);
    assert(jogPhaseBytes(std::numeric_limits<double>::quiet_NaN()).value() == 0);
    assert(jogPhaseBytes(std::numeric_limits<double>::infinity()).value() == 0);
}

void continuousAtDisplayRate()
{
    constexpr double displayPeriod = 1.0 / 60.0;
    std::uint16_t previous = jogPhaseBytes(0.0).value();
    for (int frame = 1; frame < 60 * 60 * 4; ++frame) {
        const std::uint16_t current = jogPhaseBytes(frame * displayPeriod).value();
        const int circularDelta = (static_cast<int>(current) - static_cast<int>(previous)
                                   + kJogPhaseTicksPerRevolution)
            % kJogPhaseTicksPerRevolution;
        const int expected = static_cast<int>(displayPeriod * kJogPhaseTicksPerSecond);
        assert(circularDelta >= expected - 1);
        assert(circularDelta <= expected + 1);
        previous = current;
    }
}

void jogStateUsesProtocolCadence()
{
    static_assert(flx10_protocol::kJogStateIntervalMs == 5,
                  "FLX10 xx27 state must remain on its 200 Hz protocol clock");
}

void waveformMarkersUseTheVisiblePwv5Path()
{
    QByteArray waveform;
    for (int index = 0; index < 300; ++index)
        waveform += flx10_protocol::encodePwv5Entry(4, 1, 2, 3);

    assert(flx10_protocol::overlayPwv5Marker(
        waveform, 1.0, 2.0, 1, 31, 7, 4, 0));
    const QByteArray amber = flx10_protocol::encodePwv5Entry(31, 7, 4, 0);
    for (int entry = 149; entry <= 151; ++entry)
        assert(waveform.mid(entry * 2, 2) == amber);
    assert(!flx10_protocol::overlayPwv5Marker(
        waveform, -1.0, 2.0, 1, 31, 7, 7, 7));
    assert(!flx10_protocol::overlayPwv5Marker(
        waveform, 20.0, 2.0, 1, 31, 7, 7, 7));

    assert(flx10_protocol::waveformEntryForTimeline(60.0, 120.0, 301) == 150);
    assert(flx10_protocol::waveformEntryForTimeline(120.0, 120.0, 301) == 300);
    assert(flx10_protocol::waveformEntryForTimeline(1.0, 0.0, 301) == -1);

    static_assert(flx10_protocol::kUploadWindowsPerTick == 2);
    static_assert(flx10_protocol::kHidTransferTimeoutMs <= 8);
    static_assert(flx10_protocol::kHidTransientRetries == 1);
    static_assert(flx10_protocol::kXx36TrickleIntervalMs == 50,
                  "each loaded FLX10 deck needs its own 20 Hz waveform keepalive");
}

void longPositionDoesNotOverflow()
{
    for (double seconds : {60.0, 3600.0, 86400.0, 1.0e6, 1.0e9}) {
        const auto phase = jogPhaseBytes(seconds);
        assert(phase.value() < kJogPhaseTicksPerRevolution);
        assert(phase.high <= 0x0E);
    }
}

void tempoDoesNotMoveAbsoluteProgress()
{
    flx10_protocol::DeckDisplaySnapshot snapshot;
    snapshot.sourcePositionSec = 300.0;
    snapshot.trackDurationSec = 600.0;
    snapshot.bpm = 120.0;
    snapshot.tempoPercent = 0.0;
    const QByteArray neutral = flx10_protocol::encodeXx27Packet(1, snapshot);

    snapshot.tempoPercent = 10.0;
    snapshot.bpm = 132.0;
    const QByteArray faster = flx10_protocol::encodeXx27Packet(1, snapshot);
    snapshot.tempoPercent = -10.0;
    snapshot.bpm = 108.0;
    const QByteArray slower = flx10_protocol::encodeXx27Packet(1, snapshot);

    // Position and source duration fields stay byte-identical. Only BPM/tempo
    // metadata may differ when the fader moves at a fixed source cursor.
    assert(neutral.mid(5, 8) == faster.mid(5, 8));
    assert(neutral.mid(5, 8) == slower.mid(5, 8));
    assert(neutral.mid(13, 5) != faster.mid(13, 5));
    assert(neutral.mid(13, 5) != slower.mid(13, 5));

    const auto timeline = flx10_protocol::xx27TimelineEncoding(
        snapshot.sourcePositionSec, snapshot.trackDurationSec);
    assert(std::abs(timeline.progress() - 0.5) < 1.0e-12);
}

void latestDisplayStateWinsWithoutBacklog()
{
    flx10_protocol::LatestDisplayPacketSlots pendingDisplay;
    bool replaced = false;
    for (std::uint64_t sequence = 1; sequence <= 1000; ++sequence) {
        flx10_protocol::DeckDisplaySnapshot snapshot;
        snapshot.sourcePositionSec = static_cast<double>(sequence) * 0.001;
        snapshot.trackDurationSec = 120.0;
        replaced = pendingDisplay.publish(
            flx10_protocol::encodeXx27Packet(1, snapshot), sequence);
    }
    assert(replaced);
    assert(pendingDisplay.size() == 1);

    flx10_protocol::DeckDisplaySnapshot deckTwo;
    deckTwo.sourcePositionSec = 42.0;
    deckTwo.trackDurationSec = 120.0;
    assert(!pendingDisplay.publish(flx10_protocol::encodeXx27Packet(2, deckTwo), 2000));
    assert(pendingDisplay.size() == 2);

    const auto deckOneLatest = pendingDisplay.takeNext();
    const auto deckTwoLatest = pendingDisplay.takeNext();
    assert(deckOneLatest && deckOneLatest->sequence == 1000);
    assert(deckTwoLatest && deckTwoLatest->sequence == 2000);
    assert(pendingDisplay.empty());
}

void waveformSweepKeepsItsOriginAndCoversEveryWindow()
{
    constexpr int totalWindows = 37;
    constexpr int capturedStartWindow = 19;
    std::array<bool, totalWindows> seen {};
    for (int sent = 0; sent < totalWindows; ++sent) {
        const int window = flx10_protocol::waveformSweepWindow(
            capturedStartWindow, sent, totalWindows);
        assert(window >= 0 && window < totalWindows);
        assert(!seen[static_cast<std::size_t>(window)]);
        seen[static_cast<std::size_t>(window)] = true;
    }
    assert(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
    assert(flx10_protocol::waveformSweepWindow(-1, 0, totalWindows)
           == totalWindows - 1);
    assert(flx10_protocol::waveformSweepWindow(8, 123, 0) == 0);
}

} // namespace

int main()
{
    basicEncoding();
    continuousAtDisplayRate();
    jogStateUsesProtocolCadence();
    waveformMarkersUseTheVisiblePwv5Path();
    longPositionDoesNotOverflow();
    tempoDoesNotMoveAbsoluteProgress();
    latestDisplayStateWinsWithoutBacklog();
    waveformSweepKeepsItsOriginAndCoversEveryWindow();
    return 0;
}
