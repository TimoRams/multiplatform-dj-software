#include "controllers/flx10/Flx10ProtocolCommon.h"

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

void longPositionDoesNotOverflow()
{
    for (double seconds : {60.0, 3600.0, 86400.0, 1.0e6, 1.0e9}) {
        const auto phase = jogPhaseBytes(seconds);
        assert(phase.value() < kJogPhaseTicksPerRevolution);
        assert(phase.high <= 0x0E);
    }
}

void beatgridRangeDoesNotWrap()
{
    std::uint32_t sample = 0;
    assert(flx10_protocol::xx2fSampleForMilliseconds(0.0, sample));
    assert(sample == 0);

    const double maximumMs = static_cast<double>(flx10_protocol::kXx2fMaximumSample)
        * 1000.0 / static_cast<double>(flx10_protocol::kXx2fSampleRate);
    assert(flx10_protocol::xx2fSampleForMilliseconds(maximumMs - 0.01, sample));
    assert(sample <= flx10_protocol::kXx2fMaximumSample);
    assert(!flx10_protocol::xx2fSampleForMilliseconds(maximumMs + 1.0, sample));
    assert(!flx10_protocol::xx2fSampleForMilliseconds(13.0 * 60.0 * 1000.0, sample));
}

} // namespace

int main()
{
    basicEncoding();
    continuousAtDisplayRate();
    longPositionDoesNotOverflow();
    beatgridRangeDoesNotWrap();
    return 0;
}
