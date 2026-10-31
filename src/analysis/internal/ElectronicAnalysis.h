#pragma once

#include "analysis/AnalysisTypes.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <array>

namespace analysis {
struct ElectronicFeatures {
    AnalysisFeatures rhythm;
    double attackStep = 44.0 / 44100.0;
    std::vector<float> attacks;
    std::vector<std::array<float, 12>> bands;
    std::array<double, 36> chroma{};
};
struct ElectronicKey {
    QString camelot;
    float confidence = 0.0f;
};
using AnalysisCooperate = std::function<bool()>;
ElectronicFeatures extractElectronicFeatures(juce::AudioFormatReader& reader,
    const AnalysisCooperate& cooperate = {}, const std::function<void(double)>& progress = {});
BeatGridFitResult fitElectronicGrid(const ElectronicFeatures& features, AnalysisOptions options = {},
    const AnalysisCooperate& cooperate = {});
ElectronicKey detectElectronicKey(const ElectronicFeatures& features);
}
