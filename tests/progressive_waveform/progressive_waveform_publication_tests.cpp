#include "domain/TrackData.h"

#include <QCoreApplication>

#include <algorithm>
#include <iostream>
#include <memory>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TrackData data;
    QVector<TrackData::WaveformBin> waveform(4);
    waveform[0].low = 0.75f;
    QVector<TrackData::RgbWaveformFrame> rgb(4);
    rgb[0].rms = 0.75f;
    rgb[0].low = 0.95f;
    bool ok = true;
    // Mirrors the control-thread drain of AnalyzerResultMailbox: completion is
    // deliberately absent while this first immutable chunk is applied.
    data.applyProgressiveWaveformChunk(8, 32, waveform, rgb);
    const auto visible = data.getWaveformData();
    ok &= require(visible.isEmpty(),
                  "progressive publication must not allocate a duration-sized GUI vector");
    ok &= require(data.getProgressiveOvrData().size() == TrackData::kProgressiveBins,
                  "sparse chunks must still update the fixed-size overview");

    // The scrolling renderer consumes the immutable line store, not the mutable
    // analysis vectors.  A first RGB chunk therefore has to publish its own
    // visible line immediately; waiting for applyAnalysisResult() made loaded
    // tracks appear blank until the entire analysis had finished.
    const auto lineStore = data.getWaveformLineStoreSnapshot();
    const auto lineChunk = lineStore ? lineStore->chunkAt(0) : nullptr;
    ok &= require(lineStore && lineStore->linesPerSecond == 1200,
                  "scrolling line store must retain full analysis resolution");
    ok &= require(lineChunk && lineChunk->lines && lineChunk->lines->size() == 32,
                  "first progressive line chunk was not published");
    if (lineChunk && lineChunk->lines && lineChunk->lines->size() > 8) {
        const auto& line = (*lineChunk->lines)[8];
        ok &= require(line.maximum > 0 && line.minimum < 0,
                      "progressive canonical line has no visible extent");
        ok &= require(line.red > line.green && line.red > line.blue,
                      "progressive canonical line lost its frequency colour");
    }

    // The production control clock batches worker bursts, then flushes all
    // touched source chunks once.  Batching must defer allocation only until
    // that same control tick, not until analysis completion.
    TrackData batched;
    batched.applyProgressiveWaveformChunk(8, 32, waveform, rgb, false);
    const auto beforeFlush = batched.getWaveformLineStoreSnapshot();
    ok &= require(beforeFlush && !beforeFlush->chunkAt(0),
                  "batched publication escaped before its control-tick flush");
    batched.flushProgressiveWaveformLines();
    const auto afterFlush = batched.getWaveformLineStoreSnapshot();
    ok &= require(afterFlush && afterFlush->chunkAt(0),
                  "control-tick flush did not publish progressive chunk");

    TrackData hourLong;
    constexpr int hourAt600BinsPerSecond = 60 * 60 * 600;
    hourLong.applyProgressiveWaveformChunk(0, hourAt600BinsPerSecond, waveform, rgb);
    const auto hourStore = hourLong.getWaveformLineStoreSnapshot();
    ok &= require(hourLong.getWaveformData().isEmpty()
                      && hourLong.getRgbWaveformData().isEmpty(),
                  "one-hour progressive load allocated full GUI timeline vectors");
    ok &= require(hourStore
                      && hourStore->totalLineCount == hourAt600BinsPerSecond
                      && hourStore->availableChunkCount() == 1,
                  "one-hour progressive load did not remain sparse");

    TrackData taggedBeatgrid;
    taggedBeatgrid.setBpmData(120.0, 0, 48000.0);
    taggedBeatgrid.ensureProvisionalBeatgrid(60.0 * 60.0);
    const auto immediateGrid = taggedBeatgrid.getBeatGrid();
    ok &= require(immediateGrid.size() >= 7199,
                  "tag BPM must create an immediate full-duration beatgrid");
    ok &= require(!immediateGrid.empty() && immediateGrid.front().positionSec <= 0.0
                      && std::any_of(immediateGrid.begin(), immediateGrid.end(),
                                     [](const auto& beat) { return beat.isDownbeat; }),
                  "immediate beatgrid must include beat and downbeat markers");
    ok &= require(!taggedBeatgrid.beatgridLockedByUser(),
                  "provisional tag beatgrid must remain replaceable by analysis");
    return ok ? 0 : 1;
}
