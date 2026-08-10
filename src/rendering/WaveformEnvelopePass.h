#pragma once

#include "analysis/AnalysisWorkingData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <QVector>
#include <functional>

namespace waveform_internal {

struct EnvelopePassInput
{
    using ChunkCallback = std::function<void(int firstBin, int totalBins,
                                             QVector<TrackData::WaveformBin>,
                                             QVector<TrackData::RgbWaveformFrame>)>;
    juce::AudioFormatReader& reader;
    analysis::AnalysisWorkingData* trackData = nullptr;
    juce::Thread& thread;
    int pointsPerSecond = 600;
    double seekHintSec = 0.0;
    // The deck can seek while the progressive pass is running. Read the latest
    // cursor position before scheduling another priority window instead of
    // continuing to prefetch around the position that started the analysis.
    std::function<double()> currentSeekHintSec;
    // Interactive audio always wins over background analysis. The analyzer
    // supplies a lock-free flag that becomes true while a platter is held.
    std::function<bool()> realtimeInteractionActive;
    juce::int64 totalSamples = 0;
    double sampleRate = 0.0;
    int numPoints = 0;
    ChunkCallback publishChunk;
};

QVector<TrackData::RgbWaveformFrame> buildInstantOverview(
    juce::AudioFormatReader* reader, int maxBins = 512);

bool runEnvelopePass(const EnvelopePassInput& input);

} // namespace waveform_internal
