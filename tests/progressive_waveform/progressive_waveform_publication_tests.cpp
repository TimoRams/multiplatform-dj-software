#include "domain/TrackData.h"

#include <QCoreApplication>

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
    bool ok = true;
    // Mirrors the control-thread drain of AnalyzerResultMailbox: completion is
    // deliberately absent while this first immutable chunk is applied.
    data.applyProgressiveWaveformChunk(8, 32, waveform, rgb);
    const auto visible = data.getWaveformData();
    ok &= require(visible.size() == 32, "progressive timeline must have stable final size");
    ok &= require(visible[8].low > 0.7f, "completed chunk must be visible before analysis completion");
    return ok ? 0 : 1;
}
