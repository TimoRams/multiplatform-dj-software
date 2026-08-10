#pragma once

#include <QString>
#include <QVector>
#include <cstdint>
#include <memory>
#include <cmath>
#include <limits>
#include <vector>

#include "TrackData.h"
#include "TrackSegment.h"
#include "waveform/WaveformLineBuilder.h"

namespace analysis {

inline constexpr std::uint32_t kCurrentAnalysisVersion = 1;

struct AnalysisIdentity {
    QString canonicalFilePath;
    std::uint64_t trackGeneration = 0;
    std::uint64_t fileSize = 0;
    std::int64_t fileModifiedMs = 0;
    std::uint32_t analysisVersion = kCurrentAnalysisVersion;
    std::uint64_t requestGeneration = 0;

    [[nodiscard]] bool matches(const AnalysisIdentity& other) const noexcept
    {
        return canonicalFilePath == other.canonicalFilePath
            && trackGeneration == other.trackGeneration
            && fileSize == other.fileSize
            && fileModifiedMs == other.fileModifiedMs
            && analysisVersion == other.analysisVersion
            && requestGeneration == other.requestGeneration;
    }
};

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
    AnalysisIdentity identity;
    int analysisVersion = 0;
    bool complete = false;
    bool validated = false;
    QString error;
    int totalExpected = 0;
    float globalMaxPeak = 0.001f;
    std::shared_ptr<const QVector<TrackData::WaveformBin>> waveform;
    std::shared_ptr<const QVector<TrackData::RgbWaveformFrame>> rgbWaveform;
    std::shared_ptr<const QVector<TrackData::RgbWaveformFrame>> overviewWaveform;
    std::shared_ptr<const QVector<TrackData::PeakFrame>> peakMip;
    std::shared_ptr<const waveform::PreparedWaveformLines> preparedWaveformLines;
    double bpm = 0.0;
    qint64 firstBeatSample = 0;
    double sampleRate = 44100.0;
    ConfidenceInfo confidence;
    BeatGridInfo beatGrid;
    std::vector<BeatMarker> beats;
    std::vector<TrackSegment> phrases;
    QString detectedKey;
};

inline bool validateResult(const AnalysisResult& value) noexcept
{
    constexpr int kMaxWaveformBins = 100'000'000;
    if (!value.complete || !value.error.isEmpty()
        || !std::isfinite(value.bpm) || value.bpm < 0.0
        || !std::isfinite(value.sampleRate) || value.sampleRate <= 0.0
        || value.totalExpected < 0 || value.totalExpected > kMaxWaveformBins)
        return false;
    const auto validSize = [](const auto& ptr) {
        return !ptr || ptr->size() <= kMaxWaveformBins;
    };
    if (!validSize(value.waveform) || !validSize(value.rgbWaveform)
        || !validSize(value.peakMip) || !validSize(value.overviewWaveform))
        return false;
    double previous = -std::numeric_limits<double>::infinity();
    for (const auto& beat : value.beats) {
        if (!std::isfinite(beat.positionSec) || beat.positionSec <= previous
            || beat.beatInBar < 1 || beat.beatInBar > 4)
            return false;
        previous = beat.positionSec;
    }
    previous = -std::numeric_limits<double>::infinity();
    for (const auto& node : value.beatGrid.tempoNodes) {
        if (!std::isfinite(node.positionSec) || !std::isfinite(node.bpm)
            || node.positionSec < previous || node.bpm <= 0.0)
            return false;
        previous = node.positionSec;
    }
    if (value.rgbWaveform) {
        for (const auto& frame : *value.rgbWaveform) {
            if (!std::isfinite(frame.rms) || !std::isfinite(frame.low)
                || !std::isfinite(frame.lowMid) || !std::isfinite(frame.mid)
                || !std::isfinite(frame.high))
                return false;
        }
    }
    return true;
}

} // namespace analysis
