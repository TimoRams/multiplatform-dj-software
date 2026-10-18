#include "analysis/AnalysisTypes.h"
#include "analysis/internal/AnalysisWorkingData.h"
#include "domain/TrackData.h"
#include "domain/BeatGridTiming.h"

#include <QCoreApplication>
#include <QThread>
#include <cmath>
#include <iostream>

namespace {
bool require(bool value, const char* message)
{
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    analysis::AnalysisResult result;
    result.complete = true;
    result.analysisVersion = analysis::kCurrentAnalysisVersion;
    result.identity.canonicalFilePath = QStringLiteral("/tmp/a.wav");
    result.identity.trackGeneration = 7;
    result.identity.fileSize = 1234;
    result.identity.fileModifiedMs = 88;
    result.identity.requestGeneration = 9;
    result.totalExpected = 3;
    result.sampleRate = 48000.0;
    result.bpm = 128.0;
    QVector<TrackData::RgbWaveformFrame> rgb(3);
    rgb[0].rms = 0.25f; rgb[1].rms = 0.5f; rgb[2].rms = 0.75f;
    rgb[0].bass = 0.55f; rgb[1].bass = 0.80f; rgb[2].bass = 1.0f;
    result.spectralWaveform = std::make_shared<const QVector<TrackData::SpectralWaveformPoint>>(rgb);
    result.overviewWaveform = result.spectralWaveform;
    QVector<TrackData::WaveformBin> geometry(3);
    geometry.fill({-0.5f, 0.5f, 0.5f, 0.4f});
    result.waveform = std::make_shared<const QVector<TrackData::WaveformBin>>(
        geometry);
    result.peakMip = std::make_shared<const QVector<TrackData::PeakFrame>>(3);
    result.beats = {{0.0, true, true, 0, 1, 1, 1.0f},
                    {0.46875, true, false, 0, 1, 2, 1.0f}};

    ok &= require(analysis::validateResult(result), "valid immutable result rejected");
    result.validated = true;
    const auto oldRgb = result.spectralWaveform;
    TrackData data;
    int rgbSignals = 0;
    QObject::connect(&data, &TrackData::rgbWaveformUpdated, [&] { ++rgbSignals; });
    ok &= require(data.applyAnalysisResult(result), "owner did not accept result");
    ok &= require(data.getRgbWaveformData().size() == 3, "RGB snapshot not published");
    ok &= require(oldRgb->at(2).rms == 0.75f, "old reader snapshot lost lifetime");
    ok &= require(rgbSignals == 1, "snapshot publish signal count is wrong");
    const auto lineStore = data.getWaveformLineStoreSnapshot();
    const auto firstLineChunk = lineStore ? lineStore->chunkAt(0) : nullptr;
    ok &= require(firstLineChunk && firstLineChunk->lines && !firstLineChunk->lines->empty(),
                  "canonical waveform line chunk not published");
    if (firstLineChunk && firstLineChunk->lines && !firstLineChunk->lines->empty()) {
        const auto& line = firstLineChunk->lines->front();
        ok &= require(line.bass > line.mid && line.bass > line.treble,
                      "canonical line lost its neutral frequency data");
        ok &= require(line.minimum < 0 && line.maximum > 0,
                      "canonical line lost its vertical peak extent");
    }

    // An exact grid imported from an external device is authoritative. A later
    // local waveform/key analysis may publish its other results, but must not
    // replace the imported tempo or beat positions.
    std::vector<TrackData::BeatMarker> importedBeats {
        {0.5, true, true, 0, 1, 1, 1.0f, false, true},
        {1.0, true, false, 0, 1, 2, 1.0f, false, true},
        {1.5, true, false, 0, 1, 3, 1.0f, false, true},
        {2.0, true, false, 0, 1, 4, 1.0f, false, true}
    };
    TrackData::BeatGridInfo importedGrid;
    importedGrid.type = TrackData::BeatGridType::ConstantTempo;
    importedGrid.lockedByUser = true;
    importedGrid.origin = TrackData::AnalysisOrigin::RekordboxDevice;
    data.setBpmData(128.0, 24'000, 48'000.0, importedBeats,
                    TrackData::ConfidenceInfo{}, importedGrid);
    analysis::AnalysisResult localReplacement = result;
    localReplacement.bpm = 90.0;
    localReplacement.firstBeatSample = 4'800;
    localReplacement.beats = {{0.1, true, true, 0, 1, 1, 1.0f}};
    ok &= require(data.applyAnalysisResult(localReplacement),
                  "local analysis was not accepted around imported grid");
    const auto preservedBeats = data.getBeatGrid();
    const auto preservedGrid = data.getBeatGridInfo();
    ok &= require(data.getBpm() == 128.0 && preservedBeats.size() == 4
                      && preservedBeats.front().positionSec == 0.5
                      && preservedBeats.back().positionSec == 2.0,
                  "local analysis overwrote imported exact beat positions");
    ok &= require(preservedGrid.lockedByUser
                      && preservedGrid.origin == TrackData::AnalysisOrigin::RekordboxDevice,
                  "local analysis lost imported beatgrid origin/lock");

    TrackData dynamicData;
    std::vector<TrackData::BeatMarker> dynamicBeats{
        {0.0, true, false, 0, 1, 3, 1.0f},
        {0.5, true, false, 0, 1, 4, 1.0f},
        {1.0, true, true, 1, 2, 1, 1.0f},
        {1.4, true, false, 1, 2, 2, 1.0f},
        {1.7, true, true, 2, 3, 1, 1.0f},
        {2.0, true, false, 2, 3, 2, 1.0f}};
    TrackData::BeatGridInfo dynamicInfo;
    dynamicInfo.type = TrackData::BeatGridType::DynamicTempo;
    dynamicInfo.lockedByUser = true;
    dynamicInfo.tempoNodes = {{0.0, 120.0, 1.0f}, {1.0, 150.0, 1.0f},
                             {1.4, 200.0, 1.0f}};
    dynamicData.setBpmData(120.0, 0, 48000.0, dynamicBeats, {}, dynamicInfo);
    ok &= require(std::abs(dynamicData.bpmAt(1.2) - 150.0) < 1.0e-9
                      && std::abs(dynamicData.bpmAt(2.2) - 200.0) < 1.0e-9,
                  "dynamic BPM follows actual adjacent beats including final interval");
    ok &= require(std::abs(beatgrid::beatPosition(dynamicBeats, 1.2, 120.0) - 2.5) < 1.0e-9
                      && std::abs(beatgrid::barPhase(dynamicBeats, 1.85, 120.0) - 0.125) < 1.0e-9,
                  "dynamic timing honors local spacing and later bar resets");
    dynamicData.setBpmData(90.0, 4800, 48000.0);
    ok &= require(dynamicData.getBpm() == 120.0
                      && dynamicData.getFirstBeatSample() == 0,
                  "incremental analysis cannot overwrite locked grid BPM or anchor");
    const auto beforeNudge = dynamicData.getBeatGridSnapshot();
    dynamicData.nudgeBeatgrid(0.02, 3.0);
    const auto afterNudge = dynamicData.getBeatGridSnapshot();
    ok &= require(beforeNudge != afterNudge
                      && beforeNudge->at(2).positionSec == 1.0
                      && afterNudge->at(2).positionSec == 1.02
                      && dynamicData.getBeatGridInfo().tempoNodes[1].positionSec == 1.02,
                  "grid nudge invalidates render snapshot and moves tempo nodes atomically");
    dynamicData.shiftBeatgridToDownbeat(1.42, 3.0);
    const auto reanchored = dynamicData.getBeatGridSnapshot();
    ok &= require(reanchored->size() == dynamicBeats.size()
                      && reanchored->at(3).isDownbeat
                      && std::abs(reanchored->at(3).positionSec - reanchored->at(2).positionSec - 0.4) < 1.0e-9
                      && dynamicData.getBeatGridInfo().type == TrackData::BeatGridType::DynamicTempo,
                  "downbeat edit preserves variable beat spacing instead of rebuilding a rigid grid");
    TrackData reloadedDynamic;
    const auto dynamicSeed = dynamicData.createAnalysisSeed();
    reloadedDynamic.setBpmData(dynamicSeed.bpm, dynamicSeed.firstBeatSample,
                              dynamicSeed.sampleRate, dynamicSeed.beats,
                              dynamicSeed.confidence, dynamicSeed.beatGrid);
    ok &= require(std::abs(reloadedDynamic.bpmAt(1.2) - 150.0) < 1.0e-9
                      && reloadedDynamic.getBeatGridInfo().type == TrackData::BeatGridType::DynamicTempo,
                  "local analysis snapshot round trip retains variable-tempo grid");
    ok &= require(reloadedDynamic.scaleDynamicBeatgrid(2.0)
                      && std::abs(reloadedDynamic.bpmAt(1.2) - 300.0) < 1.0e-9
                      && reloadedDynamic.getBeatGridSnapshot()->size() == dynamicBeats.size() * 2 - 1,
                  "doubling a dynamic grid inserts beats without flattening tempo changes");
    ok &= require(reloadedDynamic.scaleDynamicBeatgrid(0.5)
                      && std::abs(reloadedDynamic.bpmAt(1.2) - 150.0) < 1.0e-9
                      && reloadedDynamic.getBeatGridSnapshot()->size() == dynamicBeats.size(),
                  "halving restores original dynamic timestamps and tempo");
    reloadedDynamic.setBpm(60.0);
    ok &= require(std::abs(reloadedDynamic.bpmAt(1.0) - 75.0) < 1.0e-9
                      && std::abs(reloadedDynamic.bpmAt(2.1) - 100.0) < 1.0e-9
                      && reloadedDynamic.getBeatGridInfo().type == TrackData::BeatGridType::DynamicTempo,
                  "manual nominal BPM rescales dynamic timing without flattening it");

    analysis::AnalysisResult invalid = result;
    invalid.bpm = std::numeric_limits<double>::quiet_NaN();
    ok &= require(!analysis::validateResult(invalid), "NaN result accepted");
    invalid = result;
    invalid.identity.trackGeneration++;
    ok &= require(!invalid.identity.matches(result.identity), "stale generation matched");

    int progressPublications = 0;
    analysis::AnalysisWorkingData working([&](double, bool) { ++progressPublications; });
    for (int i = 0; i < 10000; ++i)
        working.reportAnalysisProgress(static_cast<double>(i) / 9999.0, true);
    working.reportAnalysisProgress(1.0, false);
    ok &= require(progressPublications <= 102, "progress was not coalesced to percent changes");

    // Long-track working state owns only immutable canonical chunks. Exercise
    // out-of-order and duplicate writes without allocating legacy full vectors.
    analysis::AnalysisWorkingData sparseWorking;
    constexpr int sparseLineCount = 4097;
    sparseWorking.setTotalExpected(sparseLineCount);
    sparseWorking.initializePreparedWaveformLines(sparseLineCount);
    QVector<TrackData::RgbWaveformFrame> sparseFrames(1024);
    QVector<TrackData::WaveformBin> sparseGeometry(1024);
    for (auto& frame : sparseFrames) {
        frame.rms = 0.5f;
        frame.bass = 0.8f;
        frame.mid = 0.3f;
    }
    sparseGeometry.fill({-0.5f, 0.5f, 0.5f, 0.4f});
    sparseWorking.writePreparedWaveformRange(2048, sparseGeometry, sparseFrames);
    sparseWorking.writePreparedWaveformRange(0, sparseGeometry, sparseFrames);
    sparseWorking.writePreparedWaveformRange(2048, sparseGeometry, sparseFrames);
    sparseWorking.writePreparedWaveformRange(1024, sparseGeometry, sparseFrames);
    sparseWorking.writePreparedWaveformRange(3072, sparseGeometry, sparseFrames);
    sparseWorking.writePreparedWaveformRange(
        4096, QVector<TrackData::WaveformBin>{sparseGeometry.front()},
        QVector<TrackData::RgbWaveformFrame>{sparseFrames.front()});
    const auto sparsePrepared = sparseWorking.preparedWaveformLines();
    ok &= require(sparsePrepared
                      && sparsePrepared->totalLineCount == sparseLineCount
                      && sparsePrepared->chunks.size() == 5,
                  "sparse long-track chunks did not finalize completely");
    ok &= require(sparsePrepared
                      && sparsePrepared->chunks.back()->size() == 1
                      && (sparsePrepared->chunks.back()->front().flags
                          & waveform_line_flags::kFinal) != 0,
                  "prepared tail chunk lost final immutable state");
    analysis::AnalysisIdentity sparseIdentity;
    sparseIdentity.trackGeneration = 11;
    auto sparseResult = std::move(sparseWorking).finish(sparseIdentity);
    ok &= require(sparseResult.preparedWaveformLines
                      && sparseResult.waveform->isEmpty()
                      && sparseResult.spectralWaveform->isEmpty(),
                  "long-track result retained duration-sized legacy vectors");

    sparseResult.complete = true;
    sparseResult.validated = true;
    sparseResult.sampleRate = 48'000.0;
    TrackData compactData;
    ok &= require(compactData.applyAnalysisResult(sparseResult),
                  "prepared analysis result was not accepted");
    const auto compactSeed = compactData.createAnalysisSeed();
    ok &= require(compactData.getWaveformData().isEmpty()
                      && compactData.getRgbWaveformData().isEmpty(),
                  "prepared result retained duplicate canonical vectors");
    ok &= require(compactSeed.preparedWaveformLines
                      == sparseResult.preparedWaveformLines,
                  "prepared analysis seed lost its immutable line snapshot");
    analysis::AnalysisWorkingData reseededWorking;
    reseededWorking.seed(compactSeed);
    auto reseededResult = std::move(reseededWorking).finish(
        compactSeed.identity);
    ok &= require(reseededResult.preparedWaveformLines
                      == sparseResult.preparedWaveformLines
                      && reseededResult.waveform->isEmpty()
                      && reseededResult.spectralWaveform->isEmpty(),
                  "prepared seed reuse recreated duplicate canonical vectors");

    return ok ? 0 : 1;
}
