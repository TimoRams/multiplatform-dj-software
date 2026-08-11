#pragma once

#include "analysis/internal/AnalysisWorkingData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <QVector>
#include <functional>
#include "waveform/WaveformTypes.h"
#include "waveform/WaveformDemand.h"

namespace waveform_internal {

struct EnvelopePassInput
{
    using ChunkCallback = std::function<void(int firstBin, int totalBins,
                                             QVector<TrackData::WaveformBin>,
                                             QVector<TrackData::RgbWaveformFrame>,
                                             WaveformNormalizationState)>;
    juce::AudioFormatReader& reader;
    analysis::AnalysisWorkingData* trackData = nullptr;
    juce::Thread& thread;
    int pointsPerSecond = 600;
    double seekHintSec = 0.0;
    // The deck can seek while the progressive pass is running. Read the latest
    // cursor position before scheduling another priority window instead of
    // continuing to prefetch around the position that started the analysis.
    std::function<double()> currentSeekHintSec;
    std::function<waveform::WaveformDemand()> currentDemand;
    // Interactive audio always wins over background analysis. The analyzer
    // supplies a lock-free flag that becomes true while a platter is held.
    std::function<bool()> realtimeInteractionActive;
    // The bounded playhead/guard bootstrap runs before entering the global
    // duration-dependent analysis gate. This callback acquires that gate for
    // the sequential full-track work that follows.
    std::function<bool()> acquireBackgroundSlot;
    juce::int64 totalSamples = 0;
    double sampleRate = 0.0;
    int numPoints = 0;
    // Short tracks retain legacy vectors for backward-compatible analysis
    // payloads. Long tracks write immutable prepared line chunks directly.
    bool retainLegacyWaveform = true;
    ChunkCallback publishChunk;
};

QVector<TrackData::RgbWaveformFrame> buildInstantOverview(
    juce::AudioFormatReader* reader, int maxBins = 512);

bool runEnvelopePass(const EnvelopePassInput& input);

} // namespace waveform_internal
