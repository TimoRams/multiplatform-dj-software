#include "analysis/internal/ElectronicAnalysis.h"
#include "analysis/internal/BeatAnalysis.h"
#include <juce_dsp/juce_dsp.h>
#include <iostream>
#include <cmath>
#include <numbers>
#include <functional>
#include <random>

namespace {
bool expect(bool value, const char* message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
int referenceBeatPhase(std::span<const analysis::detail::PhaseHit> hits, double period)
{
    int bestPhase = 0;
    double bestScore = -1.0;
    for (int phase = 0; phase < analysis::detail::kPhaseCount; ++phase) {
        const double offset = phase * period / analysis::detail::kPhaseCount;
        double score = 0.0;
        for (const auto hit : hits) {
            const double error = std::remainder(hit.time - offset, period);
            score += hit.weight * std::exp(-0.5 * std::pow(error / 0.02, 2));
        }
        if (score > bestScore) {
            bestScore = score;
            bestPhase = phase;
        }
    }
    return bestPhase;
}
analysis::ElectronicFeatures pulseFeatures(double bpm, double duration, double offset = .173)
{
    analysis::ElectronicFeatures data;
    auto& f = data.rhythm;
    f.sampleRate = 44100; f.hopSize = 256; f.durationSec = duration;
    const auto frames = static_cast<std::size_t>(std::ceil(duration * 44100 / 256));
    f.onsetStrength.assign(frames, 0); f.lowSpectralFlux.assign(frames, 0);
    data.attacks.assign(static_cast<std::size_t>(std::ceil(duration / data.attackStep)), 0);
    data.bands.resize(frames);
    for (int i = 0;; ++i) {
        const double time = offset + i * 60.0 / bpm;
        if (time >= duration) break;
        const auto frame = std::min(frames - 1, static_cast<std::size_t>(std::llround(time * 44100 / 256)));
        f.onsetStrength[frame] = 1; f.lowSpectralFlux[frame] = 1;
        const auto attack = static_cast<std::size_t>(std::llround(time / data.attackStep + .5));
        if (attack < data.attacks.size()) data.attacks[attack] = 1;
        const auto offbeat = static_cast<std::size_t>((time + 30.0 / bpm) * 44100 / 256);
        if (offbeat < frames) f.onsetStrength[offbeat] = .15f;
    }
    return data;
}
class GeneratedReader final : public juce::AudioFormatReader {
public:
    GeneratedReader(double rate, double duration, std::function<float(double, int)> generator)
        : AudioFormatReader(nullptr, "generated"), signal(std::move(generator)) {
        sampleRate = rate; lengthInSamples = static_cast<juce::int64>(duration * rate);
        numChannels = 2; bitsPerSample = 32; usesFloatingPointData = true;
    }
    bool readSamples(int* const* output, int channels, int offset, juce::int64 start, int count) override {
        for (int c = 0; c < channels; ++c) if (output[c]) {
            auto* values = reinterpret_cast<float*>(output[c]);
            for (int i = 0; i < count; ++i)
                values[offset + i] = start + i >= 0 && start + i < lengthInSamples
                    ? signal((start + i) / sampleRate, c) : 0.0f;
        }
        return true;
    }
private:
    std::function<float(double, int)> signal;
};

analysis::ElectronicFeatures timelineFeatures(const std::vector<double>& times, double duration)
{
    auto data = pulseFeatures(120, duration);
    std::fill(data.rhythm.onsetStrength.begin(), data.rhythm.onsetStrength.end(), 0);
    std::fill(data.rhythm.lowSpectralFlux.begin(), data.rhythm.lowSpectralFlux.end(), 0);
    std::fill(data.attacks.begin(), data.attacks.end(), 0);
    for (double time : times) {
        const auto frame = static_cast<std::size_t>(std::llround(time * 44100 / 256));
        const auto hit = static_cast<std::size_t>(std::llround(time / data.attackStep + .5));
        if (frame < data.rhythm.onsetStrength.size()) {
            data.rhythm.onsetStrength[frame] = 1; data.rhythm.lowSpectralFlux[frame] = 1;
        }
        if (hit < data.attacks.size()) data.attacks[hit] = 1;
    }
    return data;
}
bool timelineMatches(const analysis::BeatGridFitResult& grid, const std::vector<double>& times,
                     double from, double to, double tolerance)
{
    std::vector<double> errors;
    for (double time : times) {
        if (time < from || time >= to) continue;
        double nearest = 1000;
        for (auto beat : grid.beats) nearest = std::min(nearest, std::abs(beat.positionSec - time));
        errors.push_back(nearest);
    }
    if (errors.empty()) return false;
    std::sort(errors.begin(), errors.end());
    std::cout << "timeline p95=" << errors[errors.size() * 95 / 100] << " nodes=" << grid.grid.tempoNodes.size() << '\n';
    return errors[errors.size() * 95 / 100] <= tolerance;
}
}

int main()
{
    bool ok = true;
    {
        std::mt19937_64 random(0xb10cULL);
        std::uniform_real_distribution<double> time(0.0, 3600.0);
        std::uniform_real_distribution<double> weight(0.01, 2.0);
        for (const double bpm : {30.0, 48.0, 96.0, 120.0, 128.1234, 255.0, 300.0}) {
            const double period = 60.0 / bpm;
            std::vector<analysis::detail::PhaseHit> hits;
            for (int trial = 0; trial < 16; ++trial) {
                hits.clear();
                const double timeOffset = trial % 2 == 0 ? 0.0 : 86400.0;
                for (int i = 0; i < 193; ++i)
                    hits.push_back({timeOffset + time(random), weight(random)});
                for (double phase : {0.0, 0.5, 63.5, 64.0, 127.5})
                    hits.push_back({600.0 * period
                                        + phase * period / analysis::detail::kPhaseCount, 1.0});
                ok &= expect(analysis::detail::selectBeatPhase(hits, period)
                                 == referenceBeatPhase(hits, period),
                             "optimized phase search matches the reference search");
            }

            hits.clear();
            for (int phase = 0; phase < analysis::detail::kPhaseCount; ++phase)
                hits.push_back({phase * period / analysis::detail::kPhaseCount, 1.0});
            ok &= expect(analysis::detail::selectBeatPhase(hits, period)
                             == referenceBeatPhase(hits, period),
                         "optimized phase search preserves ambiguous tie handling");

            for (double start : {0.0, 3600.0, 86400.0}) {
                hits = {{start + 64.5 * period / analysis::detail::kPhaseCount, 1.0}};
                ok &= expect(analysis::detail::selectBeatPhase(hits, period)
                                 == referenceBeatPhase(hits, period),
                             "optimized phase search preserves half-step ties");
            }
        }
    }
    {
        std::mt19937 random(0xb10c);
        std::uniform_real_distribution<float> sample(-1.0f, 1.0f);
        for (const int order : {10, 12}) {
            const int size = 1 << order;
            juce::dsp::FFT fft(order);
            std::vector<float> reference(size * 2), positive(size * 2);
            for (int i = 0; i < size; ++i)
                reference[i] = positive[i] = sample(random);
            fft.performFrequencyOnlyForwardTransform(reference.data(), false);
            fft.performFrequencyOnlyForwardTransform(positive.data(), true);
            float maxError = 0.0f;
            float maxMagnitude = 0.0f;
            for (int i = 0; i <= size / 2; ++i) {
                maxError = std::max(maxError, std::abs(reference[i] - positive[i]));
                maxMagnitude = std::max(maxMagnitude, std::abs(reference[i]));
            }
            ok &= expect(maxError <= 1.0e-5f * (1.0f + maxMagnitude),
                         "positive-frequency FFT preserves the original magnitudes");
        }
    }
    {
        analysis::AnalysisOptions options;
        options.minBpm = 64; options.maxBpm = 100;
        const auto grid = analysis::fitElectronicGrid(pulseFeatures(174, 120), options);
        ok &= expect(std::abs(grid.bpm - 87) < .02, "selected BPM range permits the half-time interpretation");
        options.minBpm = 200; options.maxBpm = 210;
        const auto excluded = analysis::fitElectronicGrid(pulseFeatures(250, 120), options);
        ok &= expect(excluded.bpm == 0 || (excluded.bpm >= 200 && excluded.bpm <= 210),
                     "attack regression cannot escape the selected BPM range");
    }
    for (const double bpm : {48.0, 60.0, 80.0, 96.0, 120.0, 127.97, 128.1234, 138.0, 150.0, 174.0, 200.0, 230.0, 250.0, 255.0}) {
        const auto data = pulseFeatures(bpm, 120);
        const auto grid = analysis::fitElectronicGrid(data);
        const auto quality = analysis::measureBeatGridQuality(grid, bpm, .173, 120);
        std::cout << "tempo " << bpm << " -> " << grid.bpm << " phase=" << quality.phaseErrorSec
                  << " drift=" << quality.maximumEndOfTrackDriftSec << " nodes=" << grid.grid.tempoNodes.size() << '\n';
        ok &= expect(std::abs(grid.bpm - bpm) < .02, "electronic tempo is correct without octave folding");
        ok &= expect(!grid.beats.empty() && quality.percentile95BeatErrorSec <= .005
                     && quality.maximumEndOfTrackDriftSec <= .005, "electronic grid stays within 5 ms");
        ok &= expect(grid.grid.type == TrackData::BeatGridType::ConstantTempo, "constant grid does not invent tempo changes");
        ok &= expect(grid.downbeatConfidence == 0, "a plain pulse train has no structural downbeat evidence");
    }
    {
        const auto data = pulseFeatures(128.1234, 600);
        const auto grid = analysis::fitElectronicGrid(data);
        const auto quality = analysis::measureBeatGridQuality(grid, 128.1234, .173, 600);
        ok &= expect(quality.maximumEndOfTrackDriftSec <= .005, "ten-minute fractional tempo does not drift or round");
    }
    {
        std::vector<double> beats;
        double time = .173;
        for (int i = 0; i < 128; ++i, time += 60.0 / 128) beats.push_back(time);
        const double cut = time;
        for (int i = 0; i < 256; ++i, time += 60.0 / 174) beats.push_back(time);
        const auto grid = analysis::fitElectronicGrid(timelineFeatures(beats, time));
        for (const auto node : grid.grid.tempoNodes)
            std::cout << "transition-node " << node.positionSec << " bpm=" << node.bpm << '\n';
        ok &= expect(grid.grid.type == analysis::BeatGridType::DynamicTempo
                     && timelineMatches(grid, beats, .173, time, .005), "128 to 174 transition keeps both settled grids");
        ok &= expect(timelineMatches(grid, beats, cut - 1, cut + 2, .015), "tempo cut is placed on supported beats");
    }
    {
        std::vector<double> beats;
        double time = .173;
        for (int i = 0; i < 128; ++i, time += .5) beats.push_back(time);
        const double rampStart = time;
        for (int i = 0; i < 64; ++i) { beats.push_back(time); time += .5 - .1 * (i + 1) / 64; }
        const double rampEnd = time;
        for (int i = 0; i < 128; ++i, time += .4) beats.push_back(time);
        const auto grid = analysis::fitElectronicGrid(timelineFeatures(beats, time));
        for (const auto node : grid.grid.tempoNodes)
            std::cout << "ramp-node " << node.positionSec << " bpm=" << node.bpm << '\n';
        ok &= expect(grid.grid.type == analysis::BeatGridType::DynamicTempo
                     && timelineMatches(grid, beats, rampStart, rampEnd, .010), "gradual tempo ramp preserves every measured interval");
    }
    for (bool rephase : {false, true}) {
        std::vector<double> beats;
        for (double time = .173; time < 32; time += .5) beats.push_back(time);
        for (double time = 48.173 + (rephase ? .11 : 0); time < 120; time += .5) beats.push_back(time);
        const auto grid = analysis::fitElectronicGrid(timelineFeatures(beats, 120));
        ok &= expect(timelineMatches(grid, beats, 48, 120, .005), "return after breakdown stays aligned");
        if (!rephase) ok &= expect(grid.grid.type == analysis::BeatGridType::ConstantTempo, "breakdown without phase change keeps a constant grid");
    }
    for (bool loudOffbeat : {false, true}) {
        auto data = pulseFeatures(128, 120);
        constexpr double period = 60.0 / 128;
        for (std::size_t frame = 0; frame < data.bands.size(); ++frame) {
            const double time = data.rhythm.frameToSeconds(frame);
            const int phrase = static_cast<int>(std::floor((time - .173 - 2 * period) / (16 * period)));
            for (int band = 0; band < 12; ++band)
                data.bands[frame][band] = phrase % 2 == 0 ? 1.0f : 4.0f;
        }
        if (loudOffbeat) {
            for (double time = .173 + period * .5; time < 120; time += period) {
                const auto hit = static_cast<std::size_t>(std::llround(time / data.attackStep + .5));
                data.attacks[hit] = 2;
                const auto frame = data.rhythm.secondsToFrame(time);
                data.rhythm.lowSpectralFlux[frame] = 1;
                data.rhythm.onsetStrength[frame] = 1.5f;
            }
        }
        const auto grid = analysis::fitElectronicGrid(data);
        std::cout << "structure offbeat=" << loudOffbeat << " bpm=" << grid.bpm << " first=" << (grid.beats.empty() ? -1 : grid.beats.front().positionSec)
                  << " bar-phase=" << (grid.beats.empty() ? -1 : grid.beats.front().beatInBar) << '\n';
        ok &= expect(std::abs(grid.bpm - 128) < .02, "loud offbeat does not change the metrical tempo");
        ok &= expect(!grid.beats.empty() && std::abs(grid.beats.front().positionSec - .173) < .005,
                     "structure puts the grid on the kick even with louder offbeat attacks");
        ok &= expect(grid.beats.size() > 2 && grid.beats[2].beatInBar == 1,
                     "arrangement changes identify the musical downbeat");
    }
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        GeneratedReader reader(rate, 24, [](double time, int c) {
            const double phase = std::fmod(time - .173 + 1000, .5);
            const double kick = phase < .04 ? std::sin(2 * std::numbers::pi * 1800 * phase) * std::exp(-phase * 220) : 0;
            return static_cast<float>((c == 0 ? 1 : -1) * kick);
        });
        const auto features = analysis::extractElectronicFeatures(reader);
        const auto grid = analysis::fitElectronicGrid(features);
        std::cout << "reader " << rate << " -> " << grid.bpm << '\n';
        ok &= expect(std::abs(grid.bpm - 120) < .03 && !grid.beats.empty(), "reader analysis survives antiphase stereo and resampling");
        ok &= expect(!grid.beats.empty() && std::abs(grid.beats.front().positionSec - .173) < .005,
                     "reader kick placement compensates filter and resampling timing");
    }
    for (int root = 0; root < 12; ++root)
    for (bool minor : {false, true}) {
        GeneratedReader reader(44100, 6, [root, minor](double time, int channel) {
            const double tonic = 130.81278265 * std::pow(2.0, root / 12.0);
            double value = 0;
            for (const int semitone : {0, minor ? 3 : 4, 7}) {
                const double hz = tonic * std::pow(2.0, semitone / 12.0);
                value += .18 * std::sin(2 * std::numbers::pi * hz * time)
                    + .03 * std::sin(4 * std::numbers::pi * hz * time);
            }
            return static_cast<float>(channel == 0 ? value : -value);
        });
        const auto key = analysis::detectElectronicKey(analysis::extractElectronicFeatures(reader));
        const int anchor = minor ? 8 : 11;
        const QString expected = QString::number((((root - anchor + 12) % 12) * 7) % 12 + 1)
            + (minor ? QLatin1String("A") : QLatin1String("B"));
        std::cout << "key " << expected.toStdString() << " -> " << key.camelot.toStdString() << '\n';
        ok &= expect(key.camelot == expected, "EDM key matches an unambiguous major/minor reference");
    }
    {
        for (double detune : {-0.30, 0.30}) {
            GeneratedReader reader(44100, 6, [detune](double time, int channel) {
                double value = 0;
                for (int note : {0, 3, 7}) {
                    const double hz = 130.81278265 * std::pow(2.0, (note + detune) / 12);
                    value += .2 * std::sin(2 * std::numbers::pi * hz * time);
                }
                return channel == 1 ? static_cast<float>(value) : 0.0f;
            });
            ok &= expect(analysis::detectElectronicKey(analysis::extractElectronicFeatures(reader)).camelot == QLatin1String("5A"),
                         "EDM key tolerates detuning and right-only harmonic content");
        }
        GeneratedReader lateHarmony(44100, 96, [](double time, int) {
            if (time < 90) return 0.0f;
            return static_cast<float>(.2 * std::sin(2 * std::numbers::pi * 130.81278265 * time)
                + .2 * std::sin(2 * std::numbers::pi * 155.56349186 * time)
                + .2 * std::sin(2 * std::numbers::pi * 195.99771799 * time));
        });
        ok &= expect(analysis::detectElectronicKey(analysis::extractElectronicFeatures(lateHarmony)).camelot == QLatin1String("5A"),
                     "key detection reads tonal content beyond the old ninety-second cutoff");
        GeneratedReader noise(44100, 6, [](double time, int channel) {
            std::uint32_t value = static_cast<std::uint32_t>(time * 44100) + channel * 19471u;
            value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15; value *= 0x846ca68bu; value ^= value >> 16;
            return static_cast<float>(static_cast<std::int32_t>(value)) / 2147483648.0f * .2f;
        });
        ok &= expect(analysis::detectElectronicKey(analysis::extractElectronicFeatures(noise)).camelot.isEmpty(),
                     "unpitched noise has no invented musical key");
    }
    {
        GeneratedReader silence(44100, 3, [](double, int) { return 0.0f; });
        const auto features = analysis::extractElectronicFeatures(silence);
        ok &= expect(analysis::detectElectronicKey(features).camelot.isEmpty()
                     && analysis::fitElectronicGrid(features).beats.empty(), "silence produces no invented grid or key");
        int checks = 0;
        GeneratedReader cancelled(44100, 600, [](double, int) { return .2f; });
        const auto aborted = analysis::extractElectronicFeatures(cancelled, [&] { return ++checks < 3; });
        ok &= expect(checks == 3 && aborted.attacks.empty(), "cancelled extraction returns no partial result");
    }
    return ok ? 0 : 1;
}
