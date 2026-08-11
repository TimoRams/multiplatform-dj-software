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

    // A persisted line cache can arrive out of chronological order: the
    // playhead chunk is intentionally restored before the beginning.
    TrackData cachedLines;
    QVector<TrackData::RgbWaveformFrame> cachedOverview(32);
    cachedOverview[10].rms = 0.5f;
    cachedLines.initializeCachedWaveformLines(9000, 100,
                                              std::move(cachedOverview));
    auto playheadChunk = std::make_shared<std::vector<WaveformLine>>(808);
    (*playheadChunk)[308].minimum = -12000;
    (*playheadChunk)[308].maximum = 14000;
    (*playheadChunk)[308].red = 220;
    cachedLines.applyCachedWaveformLineChunk(8192, 9000, 100,
                                             std::move(playheadChunk));
    const auto cachedStore = cachedLines.getWaveformLineStoreSnapshot();
    const auto cachedPlayhead = cachedStore ? cachedStore->chunkAt(2) : nullptr;
    ok &= require(cachedStore && cachedStore->linesPerSecond == 100
                      && cachedStore->availableChunkCount() == 1,
                  "cached waveform restore must remain sparse and retain its rate");
    ok &= require(cachedPlayhead && cachedPlayhead->lines
                      && (*cachedPlayhead->lines)[308].maximum == 14000,
                  "out-of-order playhead cache chunk was not published exactly");
    ok &= require(!cachedLines.getOverviewRgbData().isEmpty(),
                  "cached waveform header must publish its full-track overview");

    TrackData batchedCache;
    constexpr int batchedTotal = 4096 * 4;
    batchedCache.initializeCachedWaveformLines(
        batchedTotal, 1200, QVector<TrackData::RgbWaveformFrame>(32));
    int batchSignals = 0;
    QObject::connect(&batchedCache, &TrackData::dataUpdated,
                     [&batchSignals] { ++batchSignals; });
    WaveformLineBatch guardedWindow;
    guardedWindow.push_back({4096,
        std::make_shared<std::vector<WaveformLine>>(4096)});
    guardedWindow.push_back({8192,
        std::make_shared<std::vector<WaveformLine>>(4096)});
    batchedCache.applyCachedWaveformLineBatch(
        batchedTotal, 1200, std::move(guardedWindow));
    const auto guardedSnapshot = batchedCache.getWaveformLineStoreSnapshot();
    ok &= require(guardedSnapshot && guardedSnapshot->chunkAt(1)
                      && guardedSnapshot->chunkAt(2),
                  "viewport and guard chunks must become visible in one batch");
    ok &= require(batchSignals == 1,
                  "one guarded cache batch must emit one publication signal");

    TrackData stableNormalization;
    QVector<TrackData::RgbWaveformFrame> finalRgb(4);
    finalRgb[0].rms = 0.82f;
    finalRgb[0].low = 0.9f;
    stableNormalization.applyProgressiveWaveformChunk(
        0, 32, {}, finalRgb, true, WaveformNormalizationState::Final);
    const auto finalSnapshot = stableNormalization.getWaveformLineStoreSnapshot();
    const auto finalChunk = finalSnapshot ? finalSnapshot->chunkAt(0) : nullptr;
    const auto finalMaximum = finalChunk && finalChunk->lines
        ? (*finalChunk->lines)[0].maximum : 0;
    QVector<TrackData::RgbWaveformFrame> latePreview(4);
    latePreview[0].rms = 0.08f;
    latePreview[0].high = 1.0f;
    stableNormalization.applyProgressiveWaveformChunk(
        0, 32, {}, latePreview, true, WaveformNormalizationState::Preview);
    const auto afterPreview = stableNormalization.getWaveformLineStoreSnapshot();
    const auto protectedChunk = afterPreview ? afterPreview->chunkAt(0) : nullptr;
    ok &= require(protectedChunk && protectedChunk->lines
                      && (*protectedChunk->lines)[0].maximum == finalMaximum,
                  "late preview normalization changed a delivered final range");
    ok &= require(protectedChunk && protectedChunk->lines
                      && ((*protectedChunk->lines)[0].flags
                          & waveform_line_flags::kFinal) != 0,
                  "final waveform range lost its immutable state flag");

    const auto previousVisualGeneration = afterPreview
        ? afterPreview->trackGeneration : 0;
    stableNormalization.beginVisualTrackLoad(previousVisualGeneration + 7);
    const auto invalidatedVisual = stableNormalization.getWaveformLineStoreSnapshot();
    ok &= require(invalidatedVisual
                      && invalidatedVisual->trackGeneration > previousVisualGeneration
                      && invalidatedVisual->totalLineCount == 0
                      && invalidatedVisual->availableChunkCount() == 0,
                  "new track request retained the previous visual generation");
    ok &= require(stableNormalization.getOverviewRgbData().isEmpty(),
                  "new track request retained the previous overview fallback");

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
