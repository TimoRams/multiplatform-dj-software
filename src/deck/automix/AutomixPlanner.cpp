#include "deck/automix/AutomixPlanner.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace brockdj::automix {

namespace {

constexpr double kEpsilon = 1e-6;

[[nodiscard]] double smoothStep(double x) noexcept
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

[[nodiscard]] std::vector<double> validDownbeats(const TrackTimeline& track)
{
    std::vector<double> result;
    result.reserve(track.downbeats.size());
    for (const double beat : track.downbeats) {
        if (beat >= -0.01 && (track.durationSec <= 0.0 || beat < track.durationSec))
            result.push_back(std::max(0.0, beat));
    }
    return result;
}

[[nodiscard]] bool isQuiet(PhraseKind kind) noexcept
{
    return kind == PhraseKind::Silence || kind == PhraseKind::Low;
}

// Start of the trailing quiet run (outro), or -1 when the track has none worth
// mixing over. Only the last half of a track can hold its outro.
[[nodiscard]] double outroStartSec(const TrackTimeline& track, double minimumLengthSec) noexcept
{
    if (track.phrases.empty() || track.durationSec <= 0.0)
        return -1.0;
    double start = -1.0;
    for (auto it = track.phrases.rbegin(); it != track.phrases.rend() && isQuiet(it->kind); ++it)
        start = it->startSec;
    if (start < 0.0 || start < track.durationSec * 0.5
        || track.durationSec - start < minimumLengthSec)
        return -1.0;
    return start;
}

// Where audible content ends. A short trailing silence is a tail, not an outro:
// the fade has to be complete before it, or the mix would open on a gap.
[[nodiscard]] double audibleEndSec(const TrackTimeline& track,
                                   const AutomixSettings& settings,
                                   double transitionSec) noexcept
{
    double end = track.durationSec - settings.endMarginSec;
    if (!track.phrases.empty()) {
        const auto& last = track.phrases.back();
        if (last.kind == PhraseKind::Silence && last.startSec > track.durationSec * 0.7
            && last.endSec - last.startSec < transitionSec * 0.5)
            end = std::min(end, last.startSec);
    }
    return std::max(0.0, end);
}

[[nodiscard]] double normalizedTempoRatio(double outBpm, double inBpm) noexcept
{
    if (outBpm <= 0.0 || inBpm <= 0.0)
        return 1.0;
    double ratio = outBpm / inBpm;
    // Sync matches half/double tempo rather than stretching a track 2x.
    while (ratio > 1.5)
        ratio *= 0.5;
    while (ratio < 0.75)
        ratio *= 2.0;
    return ratio;
}

void finishBeatPlan(TransitionPlan& plan, const AutomixSettings& settings) noexcept
{
    const int beatsPerBar = std::max(1, settings.beatsPerBar);
    if (plan.bars >= 2) {
        plan.bassSwapProgress = static_cast<double>(plan.bars / 2) / plan.bars;
        plan.bassSwapWidth = 1.0 / (plan.bars * beatsPerBar);
    } else {
        plan.bassSwapProgress = 0.5;
        plan.bassSwapWidth = 0.25;
    }
}

} // namespace

PhraseKind phraseKindFromLabel(const char* label) noexcept
{
    if (!label)
        return PhraseKind::Unknown;
    if (std::strcmp(label, "SilenceOrOutro") == 0 || std::strcmp(label, "Silence") == 0)
        return PhraseKind::Silence;
    if (std::strcmp(label, "LowEnergy") == 0 || std::strcmp(label, "Intro") == 0
        || std::strcmp(label, "Breakdown") == 0 || std::strcmp(label, "Outro") == 0)
        return PhraseKind::Low;
    if (std::strcmp(label, "MediumEnergy") == 0 || std::strcmp(label, "Verse") == 0
        || std::strcmp(label, "Build") == 0)
        return PhraseKind::Medium;
    if (std::strcmp(label, "HighEnergy") == 0 || std::strcmp(label, "Drop") == 0)
        return PhraseKind::High;
    return PhraseKind::Unknown;
}

double introStartSec(const TrackTimeline& track) noexcept
{
    double silenceEnd = 0.0;
    if (!track.phrases.empty()) {
        const auto& first = track.phrases.front();
        if (first.kind == PhraseKind::Silence
            && (track.durationSec <= 0.0 || first.endSec < track.durationSec * 0.3))
            silenceEnd = std::max(0.0, first.endSec);
    }
    for (const double beat : track.downbeats) {
        if (beat >= silenceEnd - 0.05 && beat >= -0.01)
            return std::max(0.0, beat);
    }
    return silenceEnd;
}

TransitionPlan planTransition(const TrackTimeline& out,
                              const TrackTimeline& in,
                              const AutomixSettings& settings,
                              double earliestOutStartSec,
                              bool forceSoon)
{
    TransitionPlan plan;
    if (out.durationSec <= 0.0)
        return plan;

    const double earliest = std::max(0.0, earliestOutStartSec);
    plan.inStartSec = introStartSec(in);
    const auto downbeats = validDownbeats(out);
    plan.beatAligned = out.bpm > 0.0 && in.bpm > 0.0 && downbeats.size() >= 2;

    if (!plan.beatAligned) {
        plan.anchor = TransitionAnchor::Time;
        plan.inPerOut = 1.0;
        const double end = audibleEndSec(out, settings, settings.fallbackSeconds);
        double length = std::max(1.0, settings.fallbackSeconds);
        double start = end - length;
        if (forceSoon || start < earliest) {
            start = earliest;
            length = end - start;
        }
        if (length < 0.5) {
            length = out.durationSec - start;
            if (length <= 0.05)
                return plan;
        }
        plan.outStartSec = start;
        plan.outLengthSec = length;
        plan.valid = true;
        return plan;
    }

    plan.inPerOut = normalizedTempoRatio(out.bpm, in.bpm);
    const int beatsPerBar = std::max(1, settings.beatsPerBar);
    const double barSec = 60.0 * beatsPerBar / out.bpm;
    const int maxBars = std::max(1, settings.transitionBars);
    const int minBars = std::clamp(settings.minimumBars, 1, maxBars);
    const double end = audibleEndSec(out, settings, maxBars * barSec);
    const auto count = static_cast<int>(downbeats.size());

    const auto spanSec = [&](int index, int bars) {
        if (index + bars < count)
            return downbeats[static_cast<std::size_t>(index + bars)] - downbeats[static_cast<std::size_t>(index)];
        return bars * barSec;
    };
    const auto fits = [&](int index, int bars) {
        const double start = downbeats[static_cast<std::size_t>(index)];
        return start >= earliest - kEpsilon && start + spanSec(index, bars) <= end + kEpsilon;
    };
    const auto choose = [&](int index, int bars, TransitionAnchor anchor) {
        plan.anchor = anchor;
        plan.bars = bars;
        plan.outStartSec = downbeats[static_cast<std::size_t>(index)];
        plan.outLengthSec = spanSec(index, bars);
        plan.valid = true;
        finishBeatPlan(plan, settings);
        return plan;
    };

    if (!forceSoon) {
        const double outro = outroStartSec(out, minBars * barSec);
        const int phraseBars = std::max(1, settings.phraseBars);
        std::vector<int> lengths;
        for (int bars = maxBars; bars > minBars; bars /= 2)
            lengths.push_back(bars);
        lengths.push_back(minBars);
        for (const int bars : lengths) {
            if (outro >= 0.0) {
                const auto nearest = std::min_element(downbeats.begin(), downbeats.end(),
                    [outro](double a, double b) { return std::abs(a - outro) < std::abs(b - outro); });
                const int index = static_cast<int>(nearest - downbeats.begin());
                if (fits(index, bars))
                    return choose(index, bars, TransitionAnchor::Outro);
            }
            for (int index = count - 1; index >= 0; --index) {
                if (index % phraseBars == 0 && fits(index, bars))
                    return choose(index, bars, TransitionAnchor::Phrase);
            }
            for (int index = count - 1; index >= 0; --index) {
                if (fits(index, bars))
                    return choose(index, bars, TransitionAnchor::Bar);
            }
        }
    }

    // Forced, or nothing fits any more: next bar line, shortened to what remains.
    for (int index = 0; index < count; ++index) {
        const double start = downbeats[static_cast<std::size_t>(index)];
        if (start < earliest - kEpsilon)
            continue;
        const int bars = std::min(maxBars, static_cast<int>(std::floor((end - start) / barSec + kEpsilon)));
        if (bars >= 1)
            return choose(index, bars, TransitionAnchor::Immediate);
        break;
    }

    const double remaining = out.durationSec - earliest;
    if (remaining <= 0.05)
        return plan;
    plan.anchor = TransitionAnchor::Immediate;
    plan.bars = 0;
    plan.outStartSec = earliest;
    plan.outLengthSec = std::min(remaining, maxBars * barSec);
    plan.valid = true;
    finishBeatPlan(plan, settings);
    return plan;
}

TransitionLevels transitionLevelsAt(const TransitionPlan& plan, double progress) noexcept
{
    TransitionLevels levels;
    const double p = std::clamp(progress, 0.0, 1.0);
    const double swap = std::clamp(plan.bassSwapProgress, 0.05, 0.95);
    levels.incoming = p < swap ? 0.5 * smoothStep(p / swap)
                               : 0.5 + 0.5 * smoothStep((p - swap) / (1.0 - swap));
    // The low band is handed over on the swap downbeat within one beat, so
    // the two kick drums never play on top of each other at full weight.
    const double width = std::clamp(plan.bassSwapWidth, 0.005, 0.5);
    const double t = std::clamp((p - swap) / width, 0.0, 1.0);
    levels.outLowKill = t;
    levels.inLowKill = 1.0 - t;
    if (p >= 1.0) {
        levels.incoming = 1.0;
        levels.outLowKill = 1.0;
        levels.inLowKill = 0.0;
    }
    return levels;
}

double incomingPositionFor(const TransitionPlan& plan, double outPositionSec) noexcept
{
    return plan.inStartSec + (outPositionSec - plan.outStartSec) * plan.inPerOut;
}

} // namespace brockdj::automix
