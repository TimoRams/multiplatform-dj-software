#pragma once

#include <QString>
#include <vector>

#include "TrackData.h"
#include "TrackSegment.h"

namespace analysis {

using BeatGridType = TrackData::BeatGridType;
using ConfidenceInfo = TrackData::ConfidenceInfo;
using BeatMarker = TrackData::BeatMarker;
using TempoNode = TrackData::TempoNode;
using BeatGridInfo = TrackData::BeatGridInfo;

struct AnalysisFeatures {
    double sampleRate = 44100.0;
    int frameSize = 2048;
    int hopSize = 512;
    double durationSec = 0.0;
    std::vector<float> rms;
    std::vector<float> lowEnergy;
    std::vector<float> midEnergy;
    std::vector<float> highEnergy;
    std::vector<float> spectralFlux;
    std::vector<float> lowSpectralFlux;
    std::vector<float> transientStrength;
    std::vector<float> energyNovelty;
    std::vector<float> onsetStrength;

    double frameToSeconds(size_t frame) const {
        return (static_cast<double>(frame) * static_cast<double>(hopSize)) / sampleRate;
    }

    size_t secondsToFrame(double seconds) const {
        if (seconds <= 0.0 || sampleRate <= 0.0 || hopSize <= 0)
            return 0;
        return static_cast<size_t>((seconds * sampleRate) / static_cast<double>(hopSize));
    }
};

struct TempoCandidate {
    double bpm = 0.0;
    double score = 0.0;
    QString source;
};

struct TempoEstimate {
    double bpm = 0.0;
    float confidence = 0.0f;
    std::vector<TempoCandidate> candidates;
};

struct BeatTrackingResult {
    std::vector<BeatMarker> beats;
    float confidence = 0.0f;
};

struct DownbeatResult {
    int phase = 0;
    float confidence = 0.0f;
};

struct BeatGridFitResult {
    double bpm = 0.0;
    qint64 firstBeatSample = 0;
    double selectedOffsetSec = 0.0;
    double stableRegionStartSec = 0.0;
    double stableRegionEndSec = 0.0;
    double phaseScore = 0.0;
    BeatGridInfo grid;
    std::vector<BeatMarker> beats;
    float confidence = 0.0f;
};

struct AnalysisResult {
    int analysisVersion = 0;
    double bpm = 0.0;
    qint64 firstBeatSample = 0;
    double sampleRate = 44100.0;
    ConfidenceInfo confidence;
    BeatGridInfo beatGrid;
    std::vector<BeatMarker> beats;
    std::vector<TrackSegment> phrases;
};

} // namespace analysis
