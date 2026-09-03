#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <memory>
#include <cmath>
#include <limits>
#include <vector>

#include "domain/TrackData.h"
#include "domain/DomainTypes.h"
#include "waveform/WaveformLineBuilder.h"

namespace analysis {

// Bumped whenever a change makes previously stored analysis results unusable,
// which forces every cached track to be analysed again.
// Legacy aggregate version remains for old database rows.  New writes record
// the independently invalidatable sections below, so a renderer or beat-grid
// change no longer needs to pretend that waveform/key/phrase are one blob.
inline constexpr std::uint32_t kCurrentAnalysisVersion = 3;

struct AnalysisSectionVersions final {
    std::uint32_t geometry = 1;
    std::uint32_t spectralWaveform = 1;
    std::uint32_t overview = 1;
    std::uint32_t beatGrid = 3;
    std::uint32_t key = 1;
    std::uint32_t phrase = 1;
    std::uint32_t renderArtifacts = 1;

    [[nodiscard]] static constexpr AnalysisSectionVersions current() noexcept
    { return {1, 1, 1, 3, 1, 1, 1}; }

    [[nodiscard]] constexpr bool waveformCurrent() const noexcept
    {
        const auto expected = current();
        return geometry == expected.geometry
            && spectralWaveform == expected.spectralWaveform
            && overview == expected.overview;
    }
    [[nodiscard]] constexpr bool beatGridCurrent() const noexcept
    { return beatGrid == current().beatGrid; }
    [[nodiscard]] constexpr bool keyCurrent() const noexcept
    { return key == current().key; }
    [[nodiscard]] constexpr bool phraseCurrent() const noexcept
    { return phrase == current().phrase; }
    [[nodiscard]] constexpr bool renderCurrent() const noexcept
    { return renderArtifacts == current().renderArtifacts; }

    [[nodiscard]] QString toStorageString() const
    {
        return QStringLiteral("g=%1;s=%2;o=%3;b=%4;k=%5;p=%6;r=%7")
            .arg(geometry).arg(spectralWaveform).arg(overview).arg(beatGrid)
            .arg(key).arg(phrase).arg(renderArtifacts);
    }
    [[nodiscard]] static AnalysisSectionVersions fromStorageString(const QString& value)
    {
        AnalysisSectionVersions result{};
        // Section metadata is all-or-nothing.  Accepting a truncated string
        // while defaulting its missing fields to current would create exactly
        // the mixed cache state this format is meant to prevent.
        std::uint8_t mask = 0;
        for (const auto& field : value.split(';', Qt::SkipEmptyParts)) {
            const auto pair = field.split('=');
            if (pair.size() != 2) continue;
            bool valid = false;
            const auto number = pair[1].toUInt(&valid);
            if (!valid || number == 0) continue;
            if (pair[0] == QLatin1String("g")) { result.geometry = number; mask |= 1U << 0; }
            else if (pair[0] == QLatin1String("s")) { result.spectralWaveform = number; mask |= 1U << 1; }
            else if (pair[0] == QLatin1String("o")) { result.overview = number; mask |= 1U << 2; }
            else if (pair[0] == QLatin1String("b")) { result.beatGrid = number; mask |= 1U << 3; }
            else if (pair[0] == QLatin1String("k")) { result.key = number; mask |= 1U << 4; }
            else if (pair[0] == QLatin1String("p")) { result.phrase = number; mask |= 1U << 5; }
            else if (pair[0] == QLatin1String("r")) { result.renderArtifacts = number; mask |= 1U << 6; }
            else continue;
        }
        if (mask == 0x7f)
            return result;
        // Zero is never valid and identifies a legacy row without section
        // metadata; it must pass the aggregate-version compatibility path.
        return {0, 0, 0, 0, 0, 0, 0};
    }
};

struct AnalysisIdentity {
    QString canonicalFilePath;
    std::uint64_t trackGeneration = 0;
    std::uint64_t fileSize = 0;
    std::int64_t fileModifiedMs = 0;
    std::uint32_t analysisVersion = kCurrentAnalysisVersion;
    AnalysisSectionVersions sections = AnalysisSectionVersions::current();
    std::uint64_t requestGeneration = 0;

    [[nodiscard]] bool matches(const AnalysisIdentity& other) const noexcept
    {
        return canonicalFilePath == other.canonicalFilePath
            && trackGeneration == other.trackGeneration
            && fileSize == other.fileSize
            && fileModifiedMs == other.fileModifiedMs
            && analysisVersion == other.analysisVersion
            && sections.geometry == other.sections.geometry
            && sections.spectralWaveform == other.sections.spectralWaveform
            && sections.overview == other.sections.overview
            && sections.beatGrid == other.sections.beatGrid
            && sections.key == other.sections.key
            && sections.phrase == other.sections.phrase
            && sections.renderArtifacts == other.sections.renderArtifacts
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
    AnalysisSectionVersions sections = AnalysisSectionVersions::current();
    bool complete = false;
    bool validated = false;
    QString error;
    int totalExpected = 0;
    float globalMaxPeak = 0.001f;
    std::shared_ptr<const QVector<TrackData::WaveformBin>> waveform;
    std::shared_ptr<const QVector<TrackData::SpectralWaveformPoint>> spectralWaveform;
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
    if (!validSize(value.waveform) || !validSize(value.spectralWaveform)
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
    if (value.spectralWaveform) {
        for (const auto& frame : *value.spectralWaveform) {
            if (!std::isfinite(frame.peak) || !std::isfinite(frame.rms)
                || !std::isfinite(frame.bass) || !std::isfinite(frame.mid)
                || !std::isfinite(frame.treble))
                return false;
        }
    }
    return true;
}

} // namespace analysis
