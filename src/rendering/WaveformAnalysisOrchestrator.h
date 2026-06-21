#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <QString>

class TrackData;

namespace waveform_internal {

struct AnalysisOrchestratorInput
{
    juce::AudioFormatReader& reader;
    TrackData* trackData = nullptr;
    juce::Thread& thread;
    int pointsPerSecond = 600;
    juce::int64 totalSamples = 0;
    double sampleRate = 0.0;
    double duration = 0.0;
    bool haveFullWaveform = false;
};

bool runAnalysisOrchestrator(const AnalysisOrchestratorInput& input);

} // namespace waveform_internal
