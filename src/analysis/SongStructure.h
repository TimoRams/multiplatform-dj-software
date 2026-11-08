#pragma once

// Song-part classification ("phrase analysis"): turns a time-ordered list of
// energy runs (silence / low / mid / high, aligned to 16-beat blocks) into DJ
// song parts: Intro, Verse, Build, Drop, Breakdown, Outro and Silence.
//
// Rules, in track order:
//   * the loudest energy level present is the "peak" level; its runs are Drops;
//   * everything before the first peak is Intro, except the run directly before
//     it, which becomes a Build when the intro has more than one audible run;
//   * between peaks, low runs are Breakdowns, mid runs leading into a peak after
//     a breakdown are Builds, other mid runs are Verses;
//   * everything after the last peak is Outro;
//   * DJ-friendly edges: when the track opens (or ends) directly on a peak run
//     and there are other peaks, that edge run is the Intro (Outro) beat;
//   * silent runs stay Silence at the edges and become Breakdowns inside.
// Pure std code so it can be unit-tested and applied to legacy stored data.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace brockdj::structure {

enum class Level { Silence, Low, Mid, High };

struct LevelRun {
    double startSec = 0.0;
    double endSec = 0.0;
    Level level = Level::Mid;
    float confidence = 0.6f;
};

struct Part {
    std::string label;
    double startSec = 0.0;
    double endSec = 0.0;
    float confidence = 0.6f;
};

inline constexpr std::string_view kIntro = "Intro";
inline constexpr std::string_view kVerse = "Verse";
inline constexpr std::string_view kBuild = "Build";
inline constexpr std::string_view kDrop = "Drop";
inline constexpr std::string_view kBreakdown = "Breakdown";
inline constexpr std::string_view kOutro = "Outro";
inline constexpr std::string_view kSilence = "Silence";

// Longer pre-drop runs are verses, not builds.
inline constexpr double kMaxBuildSec = 48.0;

[[nodiscard]] inline bool isSongPartLabel(std::string_view label) noexcept
{
    return label == kIntro || label == kVerse || label == kBuild || label == kDrop
        || label == kBreakdown || label == kOutro || label == kSilence;
}

// Energy labels written by the original phrase analyser.
[[nodiscard]] inline bool levelFromEnergyLabel(std::string_view label, Level& level) noexcept
{
    if (label == "HighEnergy") { level = Level::High; return true; }
    if (label == "MediumEnergy" || label == "Unknown") { level = Level::Mid; return true; }
    if (label == "LowEnergy") { level = Level::Low; return true; }
    if (label == "SilenceOrOutro") { level = Level::Silence; return true; }
    return false;
}

[[nodiscard]] inline std::string_view colorFor(std::string_view label) noexcept
{
    if (label == kIntro) return "#4A86C5";
    if (label == kVerse) return "#6FA35A";
    if (label == kBuild) return "#E0A93B";
    if (label == kDrop) return "#E0533D";
    if (label == kBreakdown) return "#8B67C2";
    if (label == kOutro) return "#3F6A9E";
    if (label == kSilence) return "#555555";
    return "#808080";
}

struct BlockEnergy {
    float overall = 0.0f;  // block RMS normalised to the loudest block (0..1)
    float low = 0.0f;      // low-band RMS normalised to the loudest block (0..1)
    bool hasSignal = true;
};

// Energy level per block, relative to the track's own dynamic range, so that
// heavily compressed masters still separate breakdowns from drops.
[[nodiscard]] inline std::vector<Level> assignLevels(const std::vector<BlockEnergy>& blocks)
{
    constexpr float kSilenceNorm = 0.055f;
    constexpr float kFlatRange = 0.05f;
    std::vector<Level> levels(blocks.size(), Level::Mid);
    std::vector<float> score(blocks.size(), 0.0f);
    std::vector<float> audible;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        score[i] = 0.5f * blocks[i].overall + 0.5f * blocks[i].low;
        if (blocks[i].hasSignal && blocks[i].overall >= kSilenceNorm)
            audible.push_back(score[i]);
    }
    if (audible.empty()) {
        std::fill(levels.begin(), levels.end(), Level::Silence);
        return levels;
    }
    std::sort(audible.begin(), audible.end());
    auto quantile = [&](float q) {
        const auto idx = static_cast<std::size_t>(q * static_cast<float>(audible.size() - 1) + 0.5f);
        return audible[std::min(idx, audible.size() - 1)];
    };
    const float lo = quantile(0.10f);
    const float hi = quantile(0.90f);
    const float range = hi - lo;

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        if (!b.hasSignal || b.overall < kSilenceNorm) {
            levels[i] = Level::Silence;
        } else if (b.overall < 0.28f || b.low < 0.18f) {
            levels[i] = Level::Low;  // clearly quiet or no bass at all
        } else if (range < kFlatRange) {
            levels[i] = Level::Mid;
        } else {
            const float rel = (score[i] - lo) / range;
            levels[i] = rel >= 0.68f ? Level::High : rel <= 0.30f ? Level::Low : Level::Mid;
        }
    }
    return levels;
}

[[nodiscard]] inline std::vector<Part> classify(std::vector<LevelRun> runs)
{
    std::vector<Part> parts;
    runs.erase(std::remove_if(runs.begin(), runs.end(),
                              [](const LevelRun& r) { return !(r.endSec > r.startSec); }),
               runs.end());
    if (runs.empty())
        return parts;
    std::stable_sort(runs.begin(), runs.end(),
                     [](const LevelRun& a, const LevelRun& b) { return a.startSec < b.startSec; });

    Level peak = Level::Silence;
    for (const auto& r : runs)
        peak = std::max(peak, r.level);

    const int n = static_cast<int>(runs.size());
    std::vector<std::string_view> labels(static_cast<std::size_t>(n));
    auto at = [&](int i) -> const LevelRun& { return runs[static_cast<std::size_t>(i)]; };

    if (peak <= Level::Low) {
        // No real energy contrast (ambient, spoken word, mostly silence).
        for (int i = 0; i < n; ++i)
            labels[static_cast<std::size_t>(i)] = at(i).level == Level::Silence ? kSilence : kVerse;
    } else {
        int first = -1;
        int last = -1;
        for (int i = 0; i < n; ++i) {
            if (at(i).level == peak) {
                if (first < 0) first = i;
                last = i;
            }
        }

        int peakRuns = 0;
        for (int i = 0; i < n; ++i)
            peakRuns += at(i).level == peak ? 1 : 0;
        int firstAudible = -1;
        int lastAudible = -1;
        for (int i = 0; i < n; ++i) {
            if (at(i).level != Level::Silence) {
                if (firstAudible < 0) firstAudible = i;
                lastAudible = i;
            }
        }
        const bool introOnPeak = peakRuns > 1 && first == firstAudible;
        const bool outroOnPeak = peakRuns > 1 && last == lastAudible && last != first;

        int audibleBeforeFirst = 0;
        for (int i = 0; i < first; ++i)
            audibleBeforeFirst += at(i).level != Level::Silence ? 1 : 0;

        for (int i = 0; i < n; ++i) {
            const auto& r = at(i);
            std::string_view label;
            if (i < first) {
                if (r.level == Level::Silence)
                    label = kSilence;
                else if (i == first - 1 && audibleBeforeFirst > 1
                         && r.endSec - r.startSec <= kMaxBuildSec)
                    label = kBuild;
                else
                    label = kIntro;
            } else if (i > last) {
                label = r.level == Level::Silence ? kSilence : kOutro;
            } else if (r.level == peak) {
                label = (i == first && introOnPeak) ? kIntro
                      : (i == last && outroOnPeak) ? kOutro : kDrop;
            } else if (r.level == Level::Low || r.level == Level::Silence) {
                label = kBreakdown;
            } else {
                const bool leadsIntoPeak = i + 1 < n && at(i + 1).level == peak;
                const bool afterBreak = i > 0 && at(i - 1).level < Level::Mid;
                label = leadsIntoPeak && afterBreak && r.endSec - r.startSec <= kMaxBuildSec
                    ? kBuild : kVerse;
            }
            labels[static_cast<std::size_t>(i)] = label;
        }
    }

    for (int i = 0; i < n; ++i) {
        const auto& r = at(i);
        const auto label = labels[static_cast<std::size_t>(i)];
        if (!parts.empty() && parts.back().label == label
            && r.startSec - parts.back().endSec < 0.5) {
            auto& p = parts.back();
            const double a = p.endSec - p.startSec;
            const double b = r.endSec - r.startSec;
            p.confidence = static_cast<float>((p.confidence * a + r.confidence * b) / (a + b));
            p.endSec = std::max(p.endSec, r.endSec);
            continue;
        }
        parts.push_back({std::string(label), r.startSec, r.endSec, r.confidence});
    }
    return parts;
}

} // namespace brockdj::structure
