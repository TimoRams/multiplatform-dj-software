#include "audio/LoudnessMeter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <vector>

using namespace brockdj::audio;

namespace {

int g_failures = 0;

void expectNear(const char* what, double actual, double expected, double tolerance)
{
    if (std::abs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL %s: got %.4f expected %.4f (±%.3f)\n", what, actual, expected, tolerance);
        ++g_failures;
    }
}

void expectTrue(const char* what, bool condition)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++g_failures;
    }
}

// Feeds a sine (or silence) through the K-weighting meter into an accumulator.
void feed(KWeightedBlockMeter& meter, LoudnessAccumulator& acc, double sampleRate, double seconds,
          double amplitudeL, double amplitudeR, double frequency = 997.0)
{
    constexpr int kChunk = 480;
    std::vector<float> left(kChunk), right(kChunk);
    const auto total = static_cast<long long>(seconds * sampleRate);
    static double phase = 0.0;
    for (long long done = 0; done < total; done += kChunk) {
        const int n = static_cast<int>(std::min<long long>(kChunk, total - done));
        for (int i = 0; i < n; ++i) {
            const double s = std::sin(phase);
            phase += 2.0 * std::numbers::pi * frequency / sampleRate;
            left[static_cast<std::size_t>(i)] = static_cast<float>(amplitudeL * s);
            right[static_cast<std::size_t>(i)] = static_cast<float>(amplitudeR * s);
        }
        meter.process(left.data(), right.data(), n, [&](double e) { acc.addBlock(e); });
    }
}

void testSineReference(double sampleRate)
{
    KWeightedBlockMeter meter;
    meter.prepare(sampleRate);
    LoudnessAccumulator acc;
    // BS.1770 reference: a 0 dBFS 997 Hz sine in one channel reads -3.01 LUFS.
    feed(meter, acc, sampleRate, 5.0, 1.0, 0.0);
    expectNear("mono 997 Hz momentary", acc.momentaryLufs(), -3.01, 0.1);
    expectNear("mono 997 Hz short-term", acc.shortTermLufs(), -3.01, 0.1);
    expectNear("mono 997 Hz integrated", acc.integratedLufs(), -3.01, 0.1);

    meter.reset();
    acc.reset();
    feed(meter, acc, sampleRate, 5.0, 1.0, 1.0);
    expectNear("stereo 997 Hz short-term", acc.shortTermLufs(), 0.0, 0.1);

    meter.reset();
    acc.reset();
    feed(meter, acc, sampleRate, 5.0, 0.1, 0.1);   // -20 dB
    expectNear("stereo -20 dB short-term", acc.shortTermLufs(), -20.0, 0.1);
}

void testGating()
{
    KWeightedBlockMeter meter;
    meter.prepare(48000.0);
    LoudnessAccumulator acc;
    expectTrue("no integrated before signal", !acc.hasIntegrated());
    expectNear("silence floor", acc.integratedLufs(), kLufsFloor, 1e-9);

    feed(meter, acc, 48000.0, 10.0, 0.0, 0.0);   // silence: absolute gate
    expectTrue("silence is gated", !acc.hasIntegrated());
    feed(meter, acc, 48000.0, 10.0, 0.1, 0.1);   // -20 LUFS
    expectNear("silence ignored by integration", acc.integratedLufs(), -20.0, 0.15);

    // A quiet passage 30 LU below is removed by the relative gate (-10 LU).
    feed(meter, acc, 48000.0, 10.0, 0.00316, 0.00316);   // ~ -50 LUFS
    expectNear("relative gate excludes quiet passage", acc.integratedLufs(), -20.0, 0.15);
    expectNear("max short-term", acc.maxShortTermLufs(), -20.0, 0.15);

    acc.reset();
    expectTrue("reset clears integration", !acc.hasIntegrated());
}

void testQueue()
{
    LoudnessBlockQueue queue;
    for (int i = 0; i < 10; ++i)
        expectTrue("push", queue.push(static_cast<float>(i)));
    std::vector<float> drained;
    queue.drain([&](float v) { drained.push_back(v); });
    expectTrue("drained count", drained.size() == 10);
    for (std::size_t i = 0; i < drained.size(); ++i)
        expectNear("drain order", drained[i], static_cast<double>(i), 1e-9);

    int pushed = 0;
    for (int i = 0; i < 1000; ++i)
        pushed += queue.push(1.0f) ? 1 : 0;
    expectTrue("queue bounded", pushed < 1000 && pushed >= 255);
    drained.clear();
    queue.drain([&](float v) { drained.push_back(v); });
    expectTrue("drain after overflow", static_cast<int>(drained.size()) == pushed);
}

void testConversions()
{
    expectNear("energy round trip", energyToLufs(lufsToEnergy(-14.0)), -14.0, 1e-9);
    expectNear("zero energy floors", energyToLufs(0.0), kLufsFloor, 1e-9);
}

} // namespace

int main()
{
    testSineReference(44100.0);
    testSineReference(48000.0);
    testSineReference(96000.0);
    testGating();
    testQueue();
    testConversions();
    if (g_failures == 0)
        std::puts("loudness_meter: all tests passed");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
