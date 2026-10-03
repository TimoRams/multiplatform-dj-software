#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <functional>
#include <vector>

#include "analysis/AnalysisTypes.h"
#include "domain/DomainTypes.h"

#include <QString>

namespace analysis {

class AnalysisFeatureExtractor {
public:
    struct Options {
        int frameSize = 2048;
        int hopSize = 512;
        // 0 = full track. For deck analysis, the first few minutes are enough for BPM.
        double maxDurationSec = 0.0;
    };

    AnalysisFeatureExtractor();
    explicit AnalysisFeatureExtractor(Options options);

    AnalysisFeatures extract(juce::AudioFormatReader& reader,
                             juce::Thread* cancelThread = nullptr,
                             const std::function<void(double)>& onProgress = {},
                             const std::function<bool()>& shouldPause = {}) const;

private:
    Options m_options;
};

} // namespace analysis

class PhraseAnalyzer {
public:
    std::vector<TrackSegment> analyze(juce::AudioFormatReader& reader,
                                      const std::vector<double>& beatTimestamps,
                                      double durationSec) const;

    std::vector<TrackSegment> analyze(const analysis::AnalysisFeatures& features,
                                      const std::vector<TrackData::BeatMarker>& beats,
                                      double durationSec) const;

private:
    struct PhraseBlock {
        float startTime = 0.0f;
        float endTime = 0.0f;
        float overallRms = 0.0f;
        float lowBandRms = 0.0f;
        float overallNorm = 0.0f;
        float lowBandNorm = 0.0f;
        QString label;
        QString colorHex;
        bool hasSignal = false;
    };

    std::vector<PhraseBlock> buildBlocks(const std::vector<double>& beatTimestamps,
                                         double durationSec) const;
    void extractFeatures(juce::AudioFormatReader& reader,
                         std::vector<PhraseBlock>& blocks) const;
    void normalizeAndLabel(std::vector<PhraseBlock>& blocks) const;
    std::vector<TrackSegment> smoothAndMergeSegments(std::vector<PhraseBlock>& blocks,
                                                     double durationSec) const;
};
