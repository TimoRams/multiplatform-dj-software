#pragma once

#include "analysis/AnalysisWorkingData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <QVector>

namespace waveform_internal {

struct EnvelopePassInput
{
    juce::AudioFormatReader& reader;
    analysis::AnalysisWorkingData* trackData = nullptr;
    juce::Thread& thread;
    int pointsPerSecond = 600;
    double seekHintSec = 0.0;
    juce::int64 totalSamples = 0;
    double sampleRate = 0.0;
    int numPoints = 0;
};

QVector<TrackData::RgbWaveformFrame> buildInstantOverview(
    juce::AudioFormatReader* reader, int maxBins = 512);

bool runEnvelopePass(const EnvelopePassInput& input);

} // namespace waveform_internal
