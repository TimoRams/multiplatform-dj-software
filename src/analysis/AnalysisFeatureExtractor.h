#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <functional>

#include "AnalysisResult.h"

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
                             const std::function<void(double)>& onProgress = {}) const;

private:
    Options m_options;
};

} // namespace analysis
