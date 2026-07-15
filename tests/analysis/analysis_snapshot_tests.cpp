#include "analysis/AnalysisResult.h"
#include "analysis/AnalysisWorkingData.h"
#include "domain/TrackData.h"

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
    rgb[0].low = 0.55f; rgb[1].low = 0.80f; rgb[2].low = 1.0f;
    result.rgbWaveform = std::make_shared<const QVector<TrackData::RgbWaveformFrame>>(rgb);
    result.overviewWaveform = result.rgbWaveform;
    result.waveform = std::make_shared<const QVector<TrackData::WaveformBin>>(3);
    result.peakMip = std::make_shared<const QVector<TrackData::PeakFrame>>(3);
    result.beats = {{0.0, true, true, 0, 1, 1, 1.0f},
                    {0.46875, true, false, 0, 1, 2, 1.0f}};

    ok &= require(analysis::validateResult(result), "valid immutable result rejected");
    result.validated = true;
    const auto oldRgb = result.rgbWaveform;
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
        ok &= require(line.red > line.green && line.red > line.blue,
                      "low-band canonical line lost its frequency colour");
        ok &= require(line.minimum < 0 && line.maximum > 0,
                      "canonical line lost its vertical peak extent");
    }

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

    return ok ? 0 : 1;
}
