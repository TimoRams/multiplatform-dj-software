#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <QString>
#include <vector>

#include "TrackSegment.h"

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

class PhraseAnalyzer {
public:
    std::vector<TrackSegment> analyze(juce::AudioFormatReader& reader,
                                      const std::vector<double>& beatTimestamps,
                                      double durationSec) const;

private:
    std::vector<PhraseBlock> buildBlocks(const std::vector<double>& beatTimestamps,
                                         double durationSec) const;
    void extractFeatures(juce::AudioFormatReader& reader,
                         std::vector<PhraseBlock>& blocks) const;
    void normalizeAndLabel(std::vector<PhraseBlock>& blocks) const;
    std::vector<TrackSegment> mergeToSegments(const std::vector<PhraseBlock>& blocks,
                                              double durationSec) const;
};
