#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "AnalysisResult.h"

namespace analysis {

class AnalysisFeatureExtractor {
public:
    struct Options {
        int frameSize = 2048;
        int hopSize = 512;
    };

    AnalysisFeatureExtractor();
    explicit AnalysisFeatureExtractor(Options options);

    AnalysisFeatures extract(juce::AudioFormatReader& reader,
                             juce::Thread* cancelThread = nullptr) const;

private:
    Options m_options;
};

} // namespace analysis
