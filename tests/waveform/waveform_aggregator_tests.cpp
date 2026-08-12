#include "controllers/flx10/Flx10Protocol.h"
#include "waveform/WaveformAggregator.h"
#include "waveform/WaveformLineStore.h"

#include <QByteArray>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

// Deterministic fixture: a loud low-frequency (red) first half and a quieter
// high-frequency (blue) second half, so aggregation, colour weighting and
// PWV5 quantisation all have something unambiguous to preserve.
std::shared_ptr<const WaveformLineStoreSnapshot> makeFixture(
    WaveformLineStore& store, std::uint32_t totalLines)
{
    store.reset(4242, totalLines);
    const auto chunkSize = WaveformLineStore::kChunkSize;
    std::vector<WaveformLineChunk> publication;
    for (std::uint32_t first = 0; first < totalLines; first += chunkSize) {
        const auto count = std::min(chunkSize, totalLines - first);
        auto lines = std::make_shared<std::vector<WaveformLine>>(count);
        for (std::uint32_t local = 0; local < count; ++local) {
            const auto global = first + local;
            auto& line = (*lines)[local];
            if (global < totalLines / 2) {
                line.minimum = -30000;
                line.maximum = 30000;
                line.red = 240; line.green = 40; line.blue = 20;
            } else {
                line.minimum = -6000;
                line.maximum = 6000;
                line.red = 30; line.green = 60; line.blue = 220;
            }
            line.flags = waveform_line_flags::kAvailable
                | waveform_line_flags::kFinal;
        }
        publication.push_back({4242, first / chunkSize, first, count,
                               totalLines, std::move(lines)});
    }
    if (store.publishBatch(std::move(publication))
        != WaveformLineStore::PublishResult::Accepted) {
        return nullptr;
    }
    return store.snapshot();
}

std::uint16_t decodeLe16(const QByteArray& data, int entry)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(data.at(entry * 2))
        | (static_cast<std::uint8_t>(data.at(entry * 2 + 1)) << 8));
}

} // namespace

int main()
{
    bool ok = true;

    // ── TEST 1: PWV5 golden vectors ─────────────────────────────────────────
    // Pioneer packing: (red<<13)|(green<<10)|(blue<<7)|(height<<2), 2 byte LE.
    struct GoldenCase { int h, r, g, b; std::uint16_t expected; };
    const GoldenCase golden[] = {
        {0,  0, 0, 0, 0x0000},
        {31, 7, 7, 7, static_cast<std::uint16_t>((7 << 13) | (7 << 10) | (7 << 7) | (31 << 2))},
        {1,  0, 0, 0, static_cast<std::uint16_t>(1 << 2)},
        {0,  7, 0, 0, static_cast<std::uint16_t>(7 << 13)},
        {0,  0, 7, 0, static_cast<std::uint16_t>(7 << 10)},
        {0,  0, 0, 7, static_cast<std::uint16_t>(7 << 7)},
        {21, 5, 3, 6, static_cast<std::uint16_t>((5 << 13) | (3 << 10) | (6 << 7) | (21 << 2))},
    };
    for (const auto& c : golden) {
        const auto encoded = flx10_protocol::encodePwv5Entry(c.h, c.r, c.g, c.b);
        ok &= require(encoded.size() == 2, "PWV5 entry must be exactly 2 bytes");
        ok &= require(encoded.size() == 2 && decodeLe16(encoded, 0) == c.expected,
                      "PWV5 golden vector mismatch");
    }
    // Out-of-range inputs must clamp, never wrap into a neighbouring field.
    ok &= require(decodeLe16(flx10_protocol::encodePwv5Entry(99, 99, 99, 99), 0)
                      == decodeLe16(flx10_protocol::encodePwv5Entry(31, 7, 7, 7), 0),
                  "PWV5 encoder must clamp rather than overflow fields");

    // ── TEST 3: target width independence ───────────────────────────────────
    // Chunk size must never determine how many columns come out.
    WaveformLineStore store;
    constexpr std::uint32_t totalLines = 8 * WaveformLineStore::kChunkSize + 123;
    const auto snapshot = makeFixture(store, totalLines);
    ok &= require(static_cast<bool>(snapshot), "fixture store must publish");
    if (!snapshot)
        return 1;

    for (const int width : {1, 7, 150, 720, 1500, 4096}) {
        int produced = 0;
        for (int i = 0; i < width; ++i) {
            const auto range = waveform::sourceLineRangeForColumn(
                totalLines, i, width);
            ok &= require(range.valid(), "every output column needs a source range");
            const auto column = waveform::aggregateWaveformColumn(*snapshot, range);
            ok &= require(column.hasData, "fully populated store must fill every column");
            ++produced;
        }
        ok &= require(produced == width,
                      "output column count must follow target width, not chunk size");
    }

    // ── TEST 2 / TEST 8: one shared semantic for every consumer ─────────────
    // Desktop and FLX10 ask the same function for the same timeline range, so
    // the column they get must be bit-identical; the only permitted difference
    // is the FLX10's PWV5 quantisation afterwards.
    constexpr int flx10Entries = 900;
    QByteArray pwv5;
    std::vector<waveform::WaveformColumn> desktopColumns;
    desktopColumns.reserve(flx10Entries);
    for (int i = 0; i < flx10Entries; ++i) {
        const auto range = waveform::sourceLineRangeForColumn(
            totalLines, i, flx10Entries);
        const auto column = waveform::aggregateWaveformColumn(*snapshot, range);
        desktopColumns.push_back(column);
        pwv5 += flx10_protocol::encodePwv5Column(column);
    }
    ok &= require(pwv5.size() == flx10Entries * 2,
                  "PWV5 stream must hold exactly one 2-byte entry per column");

    // Re-deriving the column for the same range must be deterministic.
    for (int i = 0; i < flx10Entries; ++i) {
        const auto range = waveform::sourceLineRangeForColumn(
            totalLines, i, flx10Entries);
        const auto again = waveform::aggregateWaveformColumn(*snapshot, range);
        const auto& first = desktopColumns[static_cast<std::size_t>(i)];
        ok &= require(again.minimum == first.minimum
                          && again.maximum == first.maximum
                          && again.red == first.red
                          && again.green == first.green
                          && again.blue == first.blue,
                      "shared aggregator must be deterministic for a range");
    }

    // Decoding PWV5 back must preserve the relative structure the desktop sees:
    // the loud red half taller than the quiet blue half, and the hue flipped.
    const int firstEntry = flx10Entries / 4;          // inside the loud half
    const int secondEntry = (flx10Entries * 3) / 4;   // inside the quiet half
    const auto loud = decodeLe16(pwv5, firstEntry);
    const auto quiet = decodeLe16(pwv5, secondEntry);
    const auto heightOf = [](std::uint16_t v) { return (v >> 2) & 0x1F; };
    const auto redOf = [](std::uint16_t v) { return (v >> 13) & 0x07; };
    const auto blueOf = [](std::uint16_t v) { return (v >> 7) & 0x07; };

    ok &= require(heightOf(loud) > heightOf(quiet),
                  "PWV5 must keep the loud section taller than the quiet one");
    ok &= require(desktopColumns[firstEntry].amplitude()
                      > desktopColumns[secondEntry].amplitude(),
                  "desktop columns must show the same loud/quiet relationship");
    ok &= require(redOf(loud) > blueOf(loud),
                  "low-band-dominated section must encode red-dominant");
    ok &= require(blueOf(quiet) > redOf(quiet),
                  "high-band-dominated section must encode blue-dominant");

    // ── LOD is an implementation detail ─────────────────────────────────────
    // Zooming out increases the source lines folded per column but must never
    // make a populated column report "no data".
    for (const int width : {4, 40, 400}) {
        for (int i = 0; i < width; ++i) {
            const auto range = waveform::sourceLineRangeForColumn(
                totalLines, i, width);
            const auto column = waveform::aggregateWaveformColumn(*snapshot, range);
            ok &= require(column.hasData,
                          "folding more source lines must not empty a column");
        }
    }

    // Degenerate inputs stay safe.
    ok &= require(!waveform::aggregateWaveformColumn(*snapshot, {5, 5}).hasData,
                  "empty range must not report data");
    ok &= require(!waveform::sourceLineRangeForColumn(totalLines, 0, 0).valid(),
                  "zero target width must not produce a range");
    ok &= require(!waveform::sourceLineRangeForColumn(0, 0, 10).valid(),
                  "empty track must not produce a range");

    return ok ? 0 : 1;
}
