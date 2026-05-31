#include "TempoEstimator.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace analysis {
namespace {

double autocorrScore(const std::vector<float>& curve, int lag)
{
    if (lag <= 0 || curve.size() <= static_cast<size_t>(lag + 8))
        return 0.0;

    double sum = 0.0;
    double normA = 0.0;
    double normB = 0.0;
    for (size_t i = static_cast<size_t>(lag); i < curve.size(); ++i) {
        const double a = curve[i];
        const double b = curve[i - static_cast<size_t>(lag)];
        sum += a * b;
        normA += a * a;
        normB += b * b;
    }
    const double denom = std::sqrt(normA * normB);
    return denom > 1.0e-12 ? sum / denom : 0.0;
}

double combScore(const std::vector<float>& curve, int lag)
{
    if (lag <= 1 || curve.empty())
        return 0.0;

    const int maxPhase = std::min(lag, static_cast<int>(curve.size()));
    double best = 0.0;
    for (int phase = 0; phase < maxPhase; ++phase) {
        double score = 0.0;
        int count = 0;
        for (size_t i = static_cast<size_t>(phase); i < curve.size(); i += static_cast<size_t>(lag)) {
            score += curve[i];
            ++count;
        }
        if (count > 4)
            best = std::max(best, score / std::sqrt(static_cast<double>(count)));
    }
    return best;
}

std::vector<size_t> pickOnsets(const std::vector<float>& curve)
{
    std::vector<size_t> peaks;
    if (curve.size() < 3)
        return peaks;

    const double mean = std::accumulate(curve.begin(), curve.end(), 0.0) / static_cast<double>(curve.size());
    const float threshold = static_cast<float>(mean * 1.45);
    for (size_t i = 1; i + 1 < curve.size(); ++i) {
        if (curve[i] >= threshold && curve[i] >= curve[i - 1] && curve[i] > curve[i + 1])
            peaks.push_back(i);
    }
    return peaks;
}

double foldToDjRange(double bpm, double preferredMin, double preferredMax)
{
    while (bpm > preferredMax && bpm * 0.5 >= 55.0)
        bpm *= 0.5;
    while (bpm < preferredMin && bpm * 2.0 <= 210.0)
        bpm *= 2.0;
    return bpm;
}

} // namespace

TempoEstimator::TempoEstimator()
    : TempoEstimator(Options{})
{
}

TempoEstimator::TempoEstimator(Options options)
    : m_options(options)
{
}

TempoEstimate TempoEstimator::estimate(const AnalysisFeatures& features) const
{
    TempoEstimate result;
    if (features.onsetStrength.size() < 32 || features.sampleRate <= 0.0 || features.hopSize <= 0)
        return result;

    std::vector<TempoCandidate> raw;
    const double framesPerSecond = features.sampleRate / static_cast<double>(features.hopSize);
    const int minLag = std::max(1, static_cast<int>(std::floor((framesPerSecond * 60.0) / m_options.maxBpm)));
    const int maxLag = std::max(minLag + 1, static_cast<int>(std::ceil((framesPerSecond * 60.0) / m_options.minBpm)));

    for (int lag = minLag; lag <= maxLag; ++lag) {
        const double bpm = (framesPerSecond * 60.0) / static_cast<double>(lag);
        const double onsetAuto = autocorrScore(features.onsetStrength, lag);
        const double lowAuto = autocorrScore(features.lowEnergy, lag);
        const double onsetComb = combScore(features.onsetStrength, lag);
        const double lowComb = combScore(features.lowSpectralFlux, lag);
        const double score = 0.38 * onsetAuto + 0.22 * lowAuto + 0.28 * onsetComb + 0.12 * lowComb;
        if (score > 0.01)
            raw.push_back({bpm, score, QStringLiteral("autocorr+comb")});
    }

    const auto peaks = pickOnsets(features.onsetStrength);
    std::map<int, double> ioiBins;
    for (size_t i = 0; i < peaks.size(); ++i) {
        for (size_t j = i + 1; j < std::min(peaks.size(), i + 9); ++j) {
            const double seconds = static_cast<double>(peaks[j] - peaks[i]) / framesPerSecond;
            if (seconds <= 0.0)
                continue;
            double bpm = 60.0 / seconds;
            bpm = foldToDjRange(bpm, m_options.preferredMinBpm, m_options.preferredMaxBpm);
            if (bpm < m_options.minBpm || bpm > m_options.maxBpm)
                continue;
            const int bin = static_cast<int>(std::round(bpm * 2.0));
            ioiBins[bin] += 1.0 / static_cast<double>(j - i);
        }
    }
    for (const auto& [bin, score] : ioiBins)
        raw.push_back({static_cast<double>(bin) * 0.5, score * 0.08, QStringLiteral("ioi")});

    std::vector<TempoCandidate> merged;
    for (const auto& candidate : raw) {
        const double variants[] = {
            candidate.bpm,
            candidate.bpm * 0.5,
            candidate.bpm * 2.0
        };
        const double weights[] = {1.0, 0.88, 0.82};
        for (int v = 0; v < 3; ++v) {
            double bpm = foldToDjRange(variants[v], m_options.preferredMinBpm, m_options.preferredMaxBpm);
            if (bpm < m_options.minBpm || bpm > m_options.maxBpm)
                continue;
            auto it = std::find_if(merged.begin(), merged.end(), [&](const TempoCandidate& existing) {
                return std::abs(existing.bpm - bpm) <= 0.75;
            });
            const double score = candidate.score * weights[v];
            if (it == merged.end()) {
                merged.push_back({bpm, score, candidate.source});
            } else {
                const double totalScore = it->score + score;
                it->bpm = (it->bpm * it->score + bpm * score) / totalScore;
                it->score = totalScore;
                if (!it->source.contains(candidate.source))
                    it->source += QStringLiteral("+") + candidate.source;
            }
        }
    }

    std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
        return a.score > b.score;
    });
    if (merged.size() > 8)
        merged.resize(8);

    result.candidates = merged;
    if (merged.empty())
        return result;

    result.bpm = merged.front().bpm;
    const double top = merged.front().score;
    const double second = merged.size() > 1 ? merged[1].score : 0.0;
    const double dominance = top > 1.0e-9 ? (top - second) / top : 0.0;
    const double absolute = std::min(1.0, top / 14.0);
    result.confidence = static_cast<float>(std::clamp(0.35 * absolute + 0.65 * dominance, 0.05, 1.0));
    return result;
}

} // namespace analysis
