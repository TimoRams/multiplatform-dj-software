#include "ElectronicAnalysis.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <numbers>
#include <set>
#include <span>

namespace analysis {
namespace detail {

int selectBeatPhase(std::span<const PhaseHit> hits, double period)
{
    if (hits.empty())
        return 0;

    constexpr double sigma = 0.02;
    constexpr double inverseVariance = 1.0 / (sigma * sigma);
    constexpr int halfPhases = kPhaseCount / 2;
    const double step = period / kPhaseCount;
    const double decay = std::exp(-step * step * inverseVariance);
    std::array<double, kPhaseCount> scores{};
    double totalWeight = 0.0;
    double largestTime = 0.0;

    // Gaussian ratios between neighbouring phases decay geometrically, so
    // each hit needs only one wrapped position and two exponentials.
    for (const auto hit : hits) {
        const double wrapped = std::remainder(hit.time, period);
        const int nearest = static_cast<int>(std::round(wrapped / step));
        const double residual = wrapped - nearest * step;
        const int centre = (nearest + kPhaseCount) % kPhaseCount;
        const double normalized = residual / sigma;
        const double initial = hit.weight * std::exp(-0.5 * normalized * normalized);
        double forward = initial;
        double backward = initial;
        double forwardRatio = std::exp((residual * step - 0.5 * step * step)
                                       * inverseVariance);
        double backwardRatio = decay / forwardRatio;

        scores[centre] += initial;
        for (int distance = 1; distance < halfPhases; ++distance) {
            forward *= forwardRatio;
            backward *= backwardRatio;
            forwardRatio *= decay;
            backwardRatio *= decay;
            scores[(centre + distance) % kPhaseCount] += forward;
            scores[(centre - distance + kPhaseCount) % kPhaseCount] += backward;
        }
        scores[(centre + halfPhases) % kPhaseCount] += residual >= 0.0
            ? forward * forwardRatio : backward * backwardRatio;
        totalWeight += hit.weight;
        largestTime = std::max(largestTime, std::abs(hit.time));
    }

    const auto maximum = std::max_element(scores.begin(), scores.end());
    const double roundingMargin = 64.0 * std::numeric_limits<double>::epsilon()
        * largestTime / sigma;
    const double margin = std::max(1.0e-9, roundingMargin) * std::max(1.0, totalWeight);
    int bestPhase = static_cast<int>(maximum - scores.begin());
    int contenders = 0;
    for (double score : scores)
        contenders += *maximum - score <= margin;
    if (contenders <= 1)
        return bestPhase;

    // Re-evaluate near-ties using the original expression and summation order.
    // The recurrence changes only floating-point rounding, not the phase model.
    double bestScore = -1.0;
    for (int phase = 0; phase < kPhaseCount; ++phase) {
        if (*maximum - scores[phase] > margin)
            continue;
        const double offset = phase * period / kPhaseCount;
        double score = 0.0;
        for (const auto hit : hits) {
            const double error = std::remainder(hit.time - offset, period);
            score += hit.weight * std::exp(-0.5 * std::pow(error / sigma, 2));
        }
        if (score > bestScore) {
            bestScore = score;
            bestPhase = phase;
        }
    }
    return bestPhase;
}

} // namespace detail

namespace {
constexpr double sr = 44100.0;
constexpr int hop = 256;
constexpr double pi = std::numbers::pi;

void normalize(std::vector<float>& values)
{
    std::vector<float> positive;
    for (float x : values) if (x > 1.0e-10f) positive.push_back(x);
    if (positive.empty()) return;
    const auto at = positive.begin() + static_cast<std::ptrdiff_t>(positive.size() * 9 / 10);
    std::nth_element(positive.begin(), at, positive.end());
    const float scale = std::max(*at, 1.0e-10f);
    for (float& x : values) x = std::clamp(x / scale, 0.0f, 1.0f);
}

void addChroma(const std::vector<float>& magnitudes, std::array<double, 36>& chroma)
{
    struct Peak { double hz, amplitude; };
    std::vector<Peak> peaks;
    const double binHz = sr / 4096;
    for (int bin = 3; bin * binHz <= 3500.0; ++bin) {
        const double a = magnitudes[bin - 1], b = magnitudes[bin], c = magnitudes[bin + 1];
        if (b <= 0.0001 || b < a || b <= c) continue;
        const double curvature = a - 2 * b + c;
        const double fraction = std::abs(curvature) > 1.0e-12
            ? std::clamp(0.5 * (a - c) / curvature, -0.5, 0.5) : 0.0;
        double envelope = 0.0;
        const int first = std::max(1, bin - 10), last = std::min(2047, bin + 10);
        for (int i = first; i <= last; ++i) envelope += magnitudes[i];
        envelope /= last - first + 1;
        const double hz = (bin + fraction) * binHz;
        peaks.push_back({hz, b / std::max(envelope, 1.0e-10) * std::pow(10.0, -5.0 * hz / 20000.0)});
    }
    std::sort(peaks.begin(), peaks.end(), [](auto a, auto b) { return a.amplitude > b.amplitude; });
    if (peaks.size() > 60) peaks.resize(60);
    for (const auto peak : peaks) {
        for (int harmonic = 1; harmonic <= 4; ++harmonic) {
            const double note = 69.0 + 12.0 * std::log2(peak.hz / harmonic / 440.0);
            const double position = note * 3.0;
            const double weight = peak.amplitude * peak.amplitude * std::pow(0.6, harmonic - 1);
            const int first = static_cast<int>(std::ceil(position - 1.5));
            const int last = static_cast<int>(std::floor(position + 1.5));
            for (int bin = first; bin <= last; ++bin)
                chroma[((bin % 36) + 36) % 36] += weight * std::cos(pi * (position - bin) / 3.0);
        }
    }
}

using Hit = detail::PhaseHit;
struct Model { double phase = 0, period = 0, quality = 0; };

std::vector<Hit> hitsOf(const ElectronicFeatures& features)
{
    const auto& attacks = features.attacks;
    std::vector<float> rises(attacks.size());
    for (std::size_t i = 1; i < attacks.size(); ++i)
        rises[i] = std::max(0.0f, attacks[i] - attacks[i - 1]);
    normalize(rises);
    std::vector<Hit> hits;
    const auto& rhythm = features.rhythm;
    for (std::size_t i = 1; i + 1 < rises.size(); ++i) {
        if (rises[i] < 0.12f || rises[i] < rises[i - 1] || rises[i] <= rises[i + 1]) continue;
        const double time = (i - 0.5) * features.attackStep;
        const int frame = static_cast<int>(rhythm.secondsToFrame(time));
        float bass = 0.0f;
        for (int j = std::max(0, frame - 5); j <= frame + 5 && j < static_cast<int>(rhythm.lowSpectralFlux.size()); ++j)
            bass = std::max(bass, rhythm.lowSpectralFlux[j]);
        hits.push_back({time, rises[i] * (0.25 + 2.0 * bass)});
    }
    return hits;
}

double signedPhase(double time, double period)
{
    return std::remainder(time, period);
}

std::vector<TempoCandidate> tempos(const AnalysisFeatures& features, double from, double to,
                                  AnalysisOptions options)
{
    const auto first = std::min(features.secondsToFrame(from), features.onsetStrength.size());
    const auto last = std::min(features.secondsToFrame(to), features.onsetStrength.size());
    if (last <= first + 32) return {};
    const int count = static_cast<int>(last - first);
    int order = 1;
    while ((1 << order) < count * 2) ++order;
    const int size = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<std::complex<float>> input(size), spectrum(size), correlation(size);
    for (int i = 0; i < count; ++i)
        input[i] = {features.onsetStrength[first + i], 0.0f};
    fft.perform(input.data(), spectrum.data(), false);
    for (int i = 0; i < size; ++i) spectrum[i] = {std::norm(spectrum[i]), 0.0f};
    fft.perform(spectrum.data(), correlation.data(), true);
    const double fps = features.sampleRate / features.hopSize;
    const int minLag = std::max(2, static_cast<int>(fps * 60 / options.maxBpm));
    const int maxLag = std::min(count / 2, static_cast<int>(std::ceil(fps * 60 / options.minBpm)));
    std::vector<TempoCandidate> candidates;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        const double value = correlation[lag].real();
        if (value <= 1.0e-10 || value < correlation[lag - 1].real() || value < correlation[lag + 1].real()) continue;
        const double curvature = correlation[lag - 1].real() - 2 * value + correlation[lag + 1].real();
        const double delta = std::abs(curvature) > 1.0e-12
            ? std::clamp(0.5 * (correlation[lag - 1].real() - correlation[lag + 1].real()) / curvature, -0.5, 0.5) : 0.0;
        const double bpm = fps * 60 / (lag + delta);
        if (bpm < options.minBpm || bpm > options.maxBpm) continue;
        const double omega = 2 * pi * bpm / (60 * fps);
        double fourier = 0.0;
        const int window = std::min(count, static_cast<int>(20 * fps));
        for (int start = 0; start < count; start += window) {
            double re = 0.0, im = 0.0, mass = 0.0;
            for (int i = start; i < std::min(start + window, count); ++i) {
                const double x = input[i].real();
                re += x * std::cos(i * omega); im += x * std::sin(i * omega); mass += x;
            }
            if (mass > 0) fourier += std::hypot(re, im) / mass;
        }
        fourier /= std::ceil(static_cast<double>(count) / window);
        const double prior = 0.85 + 0.15 * std::exp(-0.5 * std::pow(std::log2(bpm / 132.0), 2));
        const double score = value / std::max(1.0e-10f, correlation[0].real())
            * std::sqrt(std::max(0.0, fourier)) * prior;
        candidates.push_back({bpm, score, QStringLiteral("electronic-autocorrelation-fourier")});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (candidates.size() > 8) candidates.resize(8);
    return candidates;
}

Model fit(const std::vector<Hit>& hits, double bpm, double from, double to)
{
    // Restrict each local fit once, rather than scanning the entire track
    // for every phase and regression iteration (quadratic on long tracks).
    const auto first = std::lower_bound(hits.begin(), hits.end(), from,
        [](const Hit& hit, double time) { return hit.time < time; });
    const auto last = std::lower_bound(first, hits.end(), to,
        [](const Hit& hit, double time) { return hit.time < time; });
    const std::span<const Hit> localHits(first, last);
    Model model{0.0, 60.0 / bpm, 0.0};
    const int phase = detail::selectBeatPhase(localHits, model.period);
    model.phase = phase * model.period / detail::kPhaseCount;
    for (int iteration = 0; iteration < 3; ++iteration) {
        struct Point { double x, y, w, error; };
        std::vector<Point> points;
        for (const auto hit : localHits) {
            const double index = std::round((hit.time - model.phase) / model.period);
            const double error = std::abs(hit.time - model.phase - index * model.period);
            if (error <= std::min(0.05, model.period * 0.1)) points.push_back({index, hit.time, hit.weight, error});
        }
        if (points.size() < 8) return {};
        std::sort(points.begin(), points.end(), [](auto a, auto b) { return a.error < b.error; });
        if (iteration > 0) points.resize(std::max<std::size_t>(8, points.size() * 4 / 5));
        double w = 0, mx = 0, my = 0;
        for (auto p : points) { w += p.w; mx += p.w * p.x; my += p.w * p.y; }
        if (w <= 0) return {};
        mx /= w; my /= w;
        double xx = 0, xy = 0;
        for (auto p : points) { xx += p.w * (p.x - mx) * (p.x - mx); xy += p.w * (p.x - mx) * (p.y - my); }
        if (xx <= 0) return {};
        const double period = xy / xx;
        if (period <= 0 || std::abs(period / model.period - 1) > 0.08) return {};
        model.period = period; model.phase = my - period * mx;
        std::set<long long> covered;
        double meanError = 0;
        for (auto p : points) {
            covered.insert(static_cast<long long>(p.x));
            meanError += std::abs(p.y - model.phase - p.x * model.period);
        }
        model.quality = std::min(1.0, covered.size() * period / std::max(period, to - from))
            * std::exp(-meanError / points.size() / 0.01);
    }
    return model;
}

const Hit* nearest(const std::vector<Hit>& hits, double position, double radius)
{
    auto at = std::lower_bound(hits.begin(), hits.end(), position - radius,
        [](auto hit, double time) { return hit.time < time; });
    const Hit* selected = nullptr;
    double score = -1;
    for (; at != hits.end() && at->time <= position + radius; ++at) {
        const double candidate = at->weight * std::exp(-0.5 * std::pow((at->time - position) / std::max(0.001, radius * 0.5), 2));
        if (candidate > score) { score = candidate; selected = &*at; }
    }
    return selected;
}
}

ElectronicFeatures extractElectronicFeatures(juce::AudioFormatReader& reader,
    const AnalysisCooperate& cooperate, const std::function<void(double)>& progress)
{
    ElectronicFeatures out;
    auto& features = out.rhythm;
    features.sampleRate = sr; features.frameSize = 1024; features.hopSize = hop;
    if (reader.sampleRate <= 0 || reader.lengthInSamples <= 0) return out;
    features.durationSec = reader.lengthInSamples / reader.sampleRate;
    const int channels = std::max(1, static_cast<int>(reader.numChannels));
    juce::AudioFormatReaderSource source(&reader, false);
    juce::ResamplingAudioSource resampler(&source, false, channels);
    resampler.setResamplingRatio(reader.sampleRate / sr);
    resampler.prepareToPlay(1024, sr);
    juce::AudioBuffer<float> block(channels, 1024), ring(channels, 4096);
    ring.clear();
    const float* const* blockChannels = block.getArrayOfReadPointers();
    float* const* ringChannels = ring.getArrayOfWritePointers();
    juce::dsp::FFT onsetFft(10), keyFft(12);
    std::vector<float> fft(8192), magnitude(2049), previous(513), window(4096);
    for (int i = 0; i < 4096; ++i) window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * pi * i / 4096));
    std::array<int, 513> bandForBin;
    bandForBin.fill(-1);
    for (int bin = 1; bin <= 512; ++bin) {
        const double hz = bin * sr / 1024;
        if (hz >= 40 && hz <= 10000)
            bandForBin[bin] = std::clamp(
                static_cast<int>(std::log(hz / 40) / std::log(250.0) * 12), 0, 11);
    }
    using Filter = juce::dsp::IIR::Filter<float>;
    std::vector<Filter> high(channels), low(channels);
    for (int c = 0; c < channels; ++c) {
        high[c].coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(sr, 900.0f);
        low[c].coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass(sr, 9000.0f);
    }
    const auto length = static_cast<std::int64_t>(std::ceil(features.durationSec * sr));
    double attackEnergy = 0;
    for (std::int64_t frame = 0; frame < length;) {
        if (cooperate && !cooperate()) return {};
        const int count = static_cast<int>(std::min<std::int64_t>(1024, length - frame));
        resampler.getNextAudioBlock({&block, 0, count});
        for (int sample = 0; sample < count; ++sample, ++frame) {
            const auto ringPosition = static_cast<int>(frame & 4095);
            for (int channel = 0; channel < channels; ++channel) {
                const float value = blockChannels[channel][sample];
                ringChannels[channel][ringPosition] = value;
                const float filtered = low[channel].processSample(high[channel].processSample(value));
                attackEnergy += static_cast<double>(filtered) * filtered / channels;
            }
            if ((frame + 1) % 44 == 0) {
                out.attacks.push_back(static_cast<float>(std::sqrt(attackEnergy / 44)));
                attackEnergy = 0;
            }
            const auto spectrum = [&](int size, juce::dsp::FFT& transform) {
                std::fill_n(magnitude.begin(), size / 2 + 1, 0.0f);
                const auto ringStart = static_cast<int>((frame + 1 - size) & 4095);
                for (int channel = 0; channel < channels; ++channel) {
                    std::fill_n(fft.begin(), size * 2, 0.0f);
                    for (int i = 0; i < size; ++i)
                        fft[i] = ringChannels[channel][(ringStart + i) & 4095]
                            * window[i * (4096 / size)];
                    transform.performFrequencyOnlyForwardTransform(fft.data(), true);
                    for (int i = 0; i <= size / 2; ++i) magnitude[i] += fft[i] / channels;
                }
            };
            if ((frame + 1) % hop == 0) {
                const int featureIndex = static_cast<int>((frame + 1) / hop - 1);
                if (frame + 1 >= 1024) spectrum(1024, onsetFft);
                else std::fill(magnitude.begin(), magnitude.end(), 0.0f);
                float flux = 0, bassFlux = 0, bass = 0, mid = 0, treble = 0;
                std::array<float, 12> bands{};
                for (int bin = 1; bin <= 512; ++bin) {
                    const double hz = bin * sr / 1024;
                    const float rise = std::max(0.0f, magnitude[bin] - previous[bin]);
                    flux += rise;
                    if (hz <= 200) { bassFlux += rise; bass += magnitude[bin]; }
                    else if (hz <= 3000) mid += magnitude[bin];
                    else treble += magnitude[bin];
                    if (bandForBin[bin] >= 0)
                        bands[bandForBin[bin]] += magnitude[bin];
                    previous[bin] = magnitude[bin];
                }
                // Spectra are timed at the window centre, not its trailing edge.
                const int targetIndex = std::max(0, featureIndex - 1);
                if (features.onsetStrength.size() <= static_cast<std::size_t>(targetIndex)) {
                    features.onsetStrength.resize(targetIndex + 1); features.lowSpectralFlux.resize(targetIndex + 1);
                    features.lowEnergy.resize(targetIndex + 1); features.midEnergy.resize(targetIndex + 1);
                    features.highEnergy.resize(targetIndex + 1); out.bands.resize(targetIndex + 1);
                }
                features.onsetStrength[targetIndex] = flux; features.lowSpectralFlux[targetIndex] = bassFlux;
                features.lowEnergy[targetIndex] = bass; features.midEnergy[targetIndex] = mid;
                features.highEnergy[targetIndex] = treble;
                for (float& x : bands) x = std::log1p(x);
                out.bands[targetIndex] = bands;
            }
            if ((frame + 1) % 4096 == 0) { spectrum(4096, keyFft); addChroma(magnitude, out.chroma); }
        }
        if (progress) progress(static_cast<double>(frame) / length);
    }
    normalize(features.onsetStrength); normalize(features.lowSpectralFlux);
    normalize(features.lowEnergy); normalize(features.midEnergy); normalize(features.highEnergy);
    features.spectralFlux = features.onsetStrength;
    features.transientStrength = features.onsetStrength;
    features.energyNovelty = features.onsetStrength;
    features.rms = features.lowEnergy;
    return out;
}

ElectronicKey detectElectronicKey(const ElectronicFeatures& features)
{
    // EDMA reference profiles and parallel-minor preference: rbxport key.rs.
    // See docs/architecture/electronic-analysis.md for provenance and limits.
    constexpr std::array<double, 12> major {1,.29,.50,.40,.60,.56,.32,.80,.31,.45,.42,.39};
    constexpr std::array<double, 12> minor {1,.31,.44,.58,.33,.49,.29,.78,.43,.29,.53,.32};
    std::array<double, 3> tuning{};
    for (int i = 0; i < 36; ++i) tuning[i % 3] += features.chroma[i];
    const int offset = static_cast<int>(std::max_element(tuning.begin(), tuning.end()) - tuning.begin());
    const int shift = offset == 2 ? -1 : offset;
    std::array<double, 12> chroma{};
    for (int note = 0; note < 12; ++note) {
        const int centre = (note * 3 + shift + 36) % 36;
        chroma[note] = features.chroma[centre] + .25 * (features.chroma[(centre + 35) % 36] + features.chroma[(centre + 1) % 36]);
    }
    const double peak = *std::max_element(chroma.begin(), chroma.end());
    if (peak <= 1.0e-9) return {};
    const double average = std::accumulate(chroma.begin(), chroma.end(), 0.0) / 12;
    if (peak < average * 1.2) return {};
    for (double& x : chroma) x = x < .2 * peak ? 0.0 : x / peak;
    const auto correlate = [&](const auto& profile, int root) {
        const double mean = std::accumulate(chroma.begin(), chroma.end(), 0.0) / 12;
        const double reference = std::accumulate(profile.begin(), profile.end(), 0.0) / 12;
        double xy = 0, xx = 0, yy = 0;
        for (int note = 0; note < 12; ++note) {
            const double x = chroma[note] - mean, y = profile[(note - root + 12) % 12] - reference;
            xy += x * y; xx += x * x; yy += y * y;
        }
        return xx * yy > 1.0e-15 ? xy / std::sqrt(xx * yy) : 0.0;
    };
    int selectedRoot = 0;
    bool selectedMinor = false;
    double best = -1, runnerUp = -1;
    for (int root = 0; root < 12; ++root) {
        for (const bool isMinor : {false, true}) {
            const double score = correlate(isMinor ? minor : major, root);
            if (score > best) { runnerUp = best; best = score; selectedRoot = root; selectedMinor = isMinor; }
            else runnerUp = std::max(runnerUp, score);
        }
    }
    if (best < 0.45) return {};
    if (!selectedMinor && best - correlate(minor, selectedRoot) < 0.1) selectedMinor = true;
    const int anchor = selectedMinor ? 8 : 11;
    const int number = (((selectedRoot - anchor + 12) % 12) * 7) % 12 + 1;
    return {QString::number(number) + (selectedMinor ? QLatin1String("A") : QLatin1String("B")),
            static_cast<float>(std::clamp((best - runnerUp) / .25, 0.0, 1.0))};
}

BeatGridFitResult fitElectronicGrid(const ElectronicFeatures& features, AnalysisOptions options,
                                   const AnalysisCooperate& cooperate)
{
    BeatGridFitResult result;
    options = options.normalized();
    const auto hits = hitsOf(features);
    const double duration = features.rhythm.durationSec;
    if (hits.size() < 8 || duration <= 0) return result;
    const auto candidates = tempos(features.rhythm, 0, duration, options);
    const auto inRange = [&options](const Model& model) {
        return model.period > 0 && 60 / model.period >= options.minBpm - .001
            && 60 / model.period <= options.maxBpm + .001;
    };
    Model selected;
    double best = -1;
    for (const auto candidate : candidates) {
        if (cooperate && !cooperate()) return {};
        Model seed;
        for (double from = 0; from < duration; from += 16) {
            const auto local = fit(hits, candidate.bpm, from, std::min(duration, from + 16));
            if (local.quality > seed.quality) seed = local;
        }
        if (seed.period <= 0) continue;
        const auto refined = fit(hits, 60 / seed.period, 0, duration);
        const Model model = refined.quality > seed.quality * .4 ? refined : seed;
        if (!inRange(model)) continue;
        const double score = .7 * candidate.score + .3 * model.quality;
        if (score > best) { best = score; selected = model; }
    }
    if (selected.period <= 0 || selected.quality < .1) return result;
    struct Region { double from, to; Model model; };
    std::vector<Region> regions;
    for (double from = 0; from + 8 < duration; from += 8) {
        if (cooperate && !cooperate()) return {};
        Model local;
        const auto choices = tempos(features.rhythm, from, std::min(duration, from + 16), options);
        double localBest = -1;
        for (const auto candidate : choices) {
            const auto model = fit(hits, candidate.bpm, from, std::min(duration, from + 16));
            const double score = candidate.score * .7 + model.quality * .3;
            if (inRange(model) && score > localBest) { local = model; localBest = score; }
        }
        if (local.period > 0 && local.quality > .4) regions.push_back({from, std::min(duration, from + 16), local});
    }
    struct Segment { double from, to; Model model; };
    std::vector<Segment> segments;
    Model active = selected;
    double start = 0;
    if (!regions.empty()) active = regions.front().model;
    for (std::size_t i = 2; i < regions.size(); ++i) {
        const auto& a = regions[i - 2]; const auto& b = regions[i - 1]; const auto& c = regions[i];
        const double difference = std::abs(c.model.period / active.period - 1);
        const double phaseDifference = std::abs(signedPhase(c.model.phase - active.phase, active.period));
        const bool consistent = std::abs(a.model.period / c.model.period - 1) < .01
            && std::abs(b.model.period / c.model.period - 1) < .01;
        if (!consistent || (difference <= .02 && phaseDifference <= .02)) continue;
        const double ratio = c.model.period / active.period;
        bool rhythmAlias = false;
        for (double harmonic : {1.5, 2.0 / 3, 4.0 / 3, .75})
            rhythmAlias |= std::abs(ratio / harmonic - 1) < .01;
        if (rhythmAlias && difference > .02) continue;
        double boundary = std::max(start, a.from - 16);
        bool foundBoundary = false;
        for (const auto hit : hits) {
            if (hit.time < boundary || hit.time > a.from + 16) continue;
            if (std::abs(signedPhase(hit.time - c.model.phase, c.model.period)) < .008
                && std::abs(signedPhase(hit.time - active.phase, active.period)) > .015) {
                int support = 0;
                for (int beat = 1; beat < 8; ++beat)
                    support += nearest(hits, hit.time + beat * c.model.period, .008) != nullptr;
                if (support >= 6) { boundary = hit.time; foundBoundary = true; break; }
            }
        }
        if (!foundBoundary || boundary <= start + active.period) continue;
        segments.push_back({start, boundary, active});
        active = c.model; start = boundary;
    }
    segments.push_back({start, duration, active});
    // A stable track is fitted over its complete duration, never rounded to a nominal BPM.
    if (segments.size() == 1) {
        const auto refined = fit(hits, 60 / selected.period, 0, duration);
        segments[0].model = refined.period > 0 ? refined : selected;
    }
    const auto emitBeat = [&](double time, double period, double confidence) {
        if (time < 0 || time >= duration || (!result.beats.empty()
            && time <= result.beats.back().positionSec + period * .5)) return;
        if (result.grid.tempoNodes.empty() || std::abs(result.grid.tempoNodes.back().bpm - 60 / period) > .00001)
            result.grid.tempoNodes.push_back({time, 60 / period, static_cast<float>(confidence)});
        BeatMarker beat;
        beat.positionSec = time; beat.confidence = static_cast<float>(confidence);
        beat.isBeat = true; result.beats.push_back(beat);
    };
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const auto segment = segments[index];
        const auto model = segment.model;
        const double first = model.phase + std::ceil((segment.from - model.phase - .002) / model.period) * model.period;
        std::vector<double> walked;
        if (index + 1 < segments.size()
            && std::abs(segments[index + 1].model.period / model.period - 1) > .02) {
            const auto next = segments[index + 1];
            double position = model.phase + std::ceil((std::max(segment.from, segment.to - 48) - model.phase) / model.period) * model.period;
            if (const auto* anchor = nearest(hits, position, .015)) {
                position = anchor->time;
                walked.push_back(position);
                double period = model.period;
                int settled = 0;
                while (position < next.from + 16 && position < duration) {
                    if (cooperate && !cooperate()) return {};
                    const auto* hit = nearest(hits, position + period, std::max(.015, period * .05));
                    if (!hit || hit->time <= position) break;
                    const double measured = hit->time - position;
                    if (std::abs(measured / period - 1) > .05) break;
                    position = hit->time; period = measured;
                    walked.push_back(position);
                    if (std::abs(period / next.model.period - 1) < .01
                        && std::abs(signedPhase(position - next.model.phase, next.model.period)) <= .002) ++settled;
                    else settled = 0;
                    if (settled >= 4) break;
                }
                if (settled < 4) walked.clear();
                else {
                    std::size_t drift = 0;
                    while (drift + 1 < walked.size()
                        && std::abs((walked[drift + 1] - walked[drift]) / model.period - 1) < .005) ++drift;
                    if (drift > 2) walked.erase(walked.begin(), walked.begin() + static_cast<std::ptrdiff_t>(drift - 2));
                }
            }
        }
        const double staticEnd = walked.empty() ? segment.to : walked.front();
        for (double time = first; time < staticEnd - .0001; time += model.period)
            emitBeat(time, model.period, model.quality);
        for (std::size_t beat = 0; beat < walked.size(); ++beat) {
            const double period = beat + 1 < walked.size() ? walked[beat + 1] - walked[beat] : segments[index + 1].model.period;
            emitBeat(walked[beat], period, model.quality);
        }
    }
    if (result.beats.empty()) return {};
    std::vector<std::array<double, 12>> prefix(features.bands.size() + 1);
    for (std::size_t i = 0; i < features.bands.size(); ++i)
        for (int band = 0; band < 12; ++band) prefix[i + 1][band] = prefix[i][band] + features.bands[i][band];
    std::array<double, 8> phaseScores{};
    for (const int bars : {1, 2, 4, 8}) {
        std::vector<double> novelty(result.beats.size() * 2);
        for (std::size_t beat = 0; beat + 1 < result.beats.size(); ++beat) {
            const double period = result.beats[beat + 1].positionSec - result.beats[beat].positionSec;
            for (int half = 0; half < 2; ++half) {
                const double time = result.beats[beat].positionSec + half * period * .5;
                const auto at = features.rhythm.secondsToFrame(time);
                const auto width = static_cast<std::size_t>(std::max(1.0, bars * 4 * period * sr / hop));
                if (at < width || at + width >= prefix.size()) continue;
                for (int band = 0; band < 12; ++band)
                    novelty[beat * 2 + half] += std::abs(prefix[at + width][band] - 2 * prefix[at][band] + prefix[at - width][band]) / width;
            }
        }
        double total = 0;
        for (std::size_t i = 1; i + 1 < novelty.size(); ++i)
            if (novelty[i] >= novelty[i - 1] && novelty[i] > novelty[i + 1]) total += novelty[i];
        if (total <= 0) continue;
        for (std::size_t i = 1; i + 1 < novelty.size(); ++i)
            if (novelty[i] >= novelty[i - 1] && novelty[i] > novelty[i + 1]) phaseScores[i % 8] += novelty[i] / total;
    }
    int phase = static_cast<int>(std::max_element(phaseScores.begin(), phaseScores.end()) - phaseScores.begin());
    double alternative = 0;
    for (int i = 0; i < 8; ++i) if (i != phase) alternative = std::max(alternative, phaseScores[i]);
    result.downbeatConfidence = phaseScores[phase] > 0
        ? static_cast<float>(std::clamp((phaseScores[phase] - alternative) / phaseScores[phase], 0.0, 1.0)) : 0.0f;
    if (phase % 2 != 0) {
        double currentSupport = 0, halfSupport = 0;
        for (std::size_t i = 0; i + 1 < result.beats.size(); ++i) {
            const double time = result.beats[i].positionSec;
            const double halfTime = (time + result.beats[i + 1].positionSec) * .5;
            for (auto [at, support] : {std::pair{features.rhythm.secondsToFrame(time), &currentSupport},
                                     std::pair{features.rhythm.secondsToFrame(halfTime), &halfSupport}}) {
                // Frame quantisation must not make an otherwise supported
                // half-beat disappear (the fitted attack has sub-frame timing).
                float peak = 0;
                const auto& flux = features.rhythm.lowSpectralFlux;
                for (auto frame = at > 1 ? at - 2 : 0; frame < flux.size() && frame <= at + 2; ++frame)
                    peak = std::max(peak, flux[frame]);
                *support += peak;
            }
        }
        if (halfSupport >= currentSupport * .75) {
            for (std::size_t i = 0; i < result.beats.size(); ++i) {
                const double period = i + 1 < result.beats.size()
                    ? result.beats[i + 1].positionSec - result.beats[i].positionSec : segments.back().model.period;
                result.beats[i].positionSec += period * .5;
            }
            while (!result.beats.empty() && result.beats.back().positionSec >= duration) result.beats.pop_back();
            if (!result.beats.empty()) {
                const double period = segments.front().model.period;
                const double preceding = result.beats.front().positionSec - period;
                if (preceding >= 0) {
                    auto beat = result.beats.front();
                    beat.positionSec = preceding;
                    result.beats.insert(result.beats.begin(), beat);
                    phase = (phase + 2) % 8;
                }
                for (auto& node : result.grid.tempoNodes) {
                    const auto beat = std::min_element(result.beats.begin(), result.beats.end(), [&](const auto& a, const auto& b) {
                        return std::abs(a.positionSec - node.positionSec) < std::abs(b.positionSec - node.positionSec);
                    });
                    node.positionSec = beat->positionSec;
                }
            }
        } else phase -= 1;
    }
    const int downbeat = phase / 2;
    for (std::size_t i = 0; i < result.beats.size(); ++i) {
        auto& beat = result.beats[i]; beat.beatInBar = (static_cast<int>(i) - downbeat + 4) % 4 + 1;
        beat.isDownbeat = beat.beatInBar == 1;
        beat.barNumber = static_cast<int>(std::floor((static_cast<double>(i) - downbeat) / 4)) + 1;
    }
    result.bpm = 60 / selected.period;
    result.firstBeatSample = static_cast<qint64>(std::llround(result.beats.front().positionSec * features.rhythm.sampleRate));
    result.confidence = static_cast<float>(std::clamp(selected.quality, 0.0, 1.0));
    result.grid.type = segments.size() > 1 ? BeatGridType::DynamicTempo : BeatGridType::ConstantTempo;
    result.phaseScore = selected.quality;
    return result;
}
}
