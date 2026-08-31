#include "analysis/internal/AnalysisWorkingData.h"
#include "waveform/WaveformAnalyzer.h"
#include "waveform/internal/WaveformEnvelopePass.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kPointsPerSecond = 1200;
constexpr int kSeconds = 2;

class TestThread final : public juce::Thread
{
public:
    TestThread() : juce::Thread("neutral-waveform-test") {}
    void run() override {}
};

struct Analysis {
    QVector<TrackData::WaveformBin> geometry;
    QVector<TrackData::SpectralWaveformPoint> spectral;
    QVector<TrackData::SpectralWaveformPoint> overview;
    QVector<TrackData::WaveformBin> progressiveGeometry;
    QVector<TrackData::SpectralWaveformPoint> progressiveSpectral;
};

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool writeFixture(const QString& path,
                  const std::function<float(int, double)>& sample,
                  float rightPolarity = 1.0f)
{
    juce::WavAudioFormat format;
    auto stream = std::make_unique<juce::FileOutputStream>(
        juce::File(path.toStdString()));
    if (!stream->openedOk())
        return false;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.release(), kSampleRate, 2, 24, {}, 0));
    if (!writer)
        return false;

    constexpr int blockSize = 1024;
    juce::AudioBuffer<float> block(2, blockSize);
    const int totalSamples = static_cast<int>(kSampleRate) * kSeconds;
    for (int first = 0; first < totalSamples; first += blockSize) {
        const int count = std::min(blockSize, totalSamples - first);
        for (int local = 0; local < count; ++local) {
            const int index = first + local;
            const float value = sample(
                index, static_cast<double>(index) / kSampleRate);
            block.setSample(0, local, value);
            block.setSample(1, local, value * rightPolarity);
        }
        if (!writer->writeFromAudioSampleBuffer(block, 0, count))
            return false;
    }
    return true;
}

Analysis analyze(const QString& path)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(
        format.createReaderFor(
            new juce::FileInputStream(juce::File(path.toStdString())), true));
    if (!reader)
        return {};

    analysis::AnalysisWorkingData working;
    working.setOverviewWaveformData(
        WaveformAnalyzer::buildInstantOverview(
            reader.get(), TrackData::kOverviewBins));
    const int totalPoints = static_cast<int>(
        (reader->lengthInSamples / reader->sampleRate) * kPointsPerSecond);
    working.setTotalExpected(totalPoints);
    working.reserve(totalPoints);
    const int totalSpectralPoints = kSeconds
        * TrackData::SPECTRAL_POINTS_PER_SECOND;
    QVector<TrackData::WaveformBin> progressiveGeometry(totalPoints);
    QVector<TrackData::SpectralWaveformPoint> progressiveSpectral(
        totalSpectralPoints);
    TestThread thread;
    const waveform_internal::EnvelopePassInput input{
        *reader,
        {},
        &working,
        thread,
        kPointsPerSecond,
        0.0,
        {},
        {},
        {},
        [] { return true; },
        reader->lengthInSamples,
        reader->sampleRate,
        totalPoints,
        true,
        [&](int firstGeometry, int, QVector<TrackData::WaveformBin> geometry,
            int firstSpectral, int,
            QVector<TrackData::SpectralWaveformPoint> spectral,
            WaveformNormalizationState) {
            std::copy(geometry.cbegin(), geometry.cend(),
                      progressiveGeometry.begin() + firstGeometry);
            std::copy(spectral.cbegin(), spectral.cend(),
                      progressiveSpectral.begin() + firstSpectral);
        }
    };
    if (!waveform_internal::runEnvelopePass(input))
        return {};
    return {
        working.getWaveformData(),
        working.getSpectralWaveformData(),
        working.getOverviewWaveformData(),
        std::move(progressiveGeometry),
        std::move(progressiveSpectral)
    };
}

TrackData::SpectralWaveformPoint meanSpectrum(
    const QVector<TrackData::SpectralWaveformPoint>& points)
{
    TrackData::SpectralWaveformPoint result;
    if (points.isEmpty())
        return result;
    for (const auto& point : points) {
        result.peak += point.peak;
        result.rms += point.rms;
        result.bass += point.bass;
        result.mid += point.mid;
        result.treble += point.treble;
    }
    const float inverse = 1.0f / static_cast<float>(points.size());
    result.peak *= inverse;
    result.rms *= inverse;
    result.bass *= inverse;
    result.mid *= inverse;
    result.treble *= inverse;
    return result;
}

bool finite(const Analysis& analysis)
{
    for (const auto& point : analysis.geometry) {
        if (!std::isfinite(point.minimum) || !std::isfinite(point.maximum)
            || !std::isfinite(point.peak) || !std::isfinite(point.rms))
            return false;
    }
    for (const auto& point : analysis.spectral) {
        if (!std::isfinite(point.peak) || !std::isfinite(point.rms)
            || !std::isfinite(point.bass) || !std::isfinite(point.mid)
            || !std::isfinite(point.treble))
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!require(dir.isValid(), "temporary fixture directory unavailable"))
        return 1;

    const auto sine = [](double frequency, float amplitude) {
        return [frequency, amplitude](int, double time) {
            return amplitude * static_cast<float>(
                std::sin(2.0 * juce::MathConstants<double>::pi
                         * frequency * time));
        };
    };

    const QString silencePath = dir.filePath("silence.wav");
    const QString bassPath = dir.filePath("bass.wav");
    const QString midPath = dir.filePath("mid.wav");
    const QString highPath = dir.filePath("high.wav");
    const QString sweepPath = dir.filePath("sweep.wav");
    const QString dynamicsPath = dir.filePath("dynamics.wav");
    const QString transientPath = dir.filePath("transient.wav");
    const QString weakHighPath = dir.filePath("weak-high.wav");
    const QString antiphasePath = dir.filePath("antiphase.wav");

    bool ok = true;
    ok &= writeFixture(silencePath, [](int, double) { return 0.0f; });
    ok &= writeFixture(bassPath, sine(80.0, 0.65f));
    ok &= writeFixture(midPath, sine(1000.0, 0.65f));
    ok &= writeFixture(highPath, sine(9000.0, 0.65f));
    ok &= writeFixture(sweepPath, [](int, double time) {
        const double frequency = 50.0 + 9950.0 * (time / kSeconds);
        return 0.6f * static_cast<float>(std::sin(
            2.0 * juce::MathConstants<double>::pi * frequency * time));
    });
    ok &= writeFixture(dynamicsPath, [](int, double time) {
        const float amplitude = time < 1.0 ? 0.08f : 0.75f;
        return amplitude * static_cast<float>(std::sin(
            2.0 * juce::MathConstants<double>::pi * 440.0 * time));
    });
    ok &= writeFixture(transientPath, [](int index, double time) {
        if (index == static_cast<int>(kSampleRate))
            return 1.0f;
        return 0.2f * static_cast<float>(std::sin(
            2.0 * juce::MathConstants<double>::pi * 440.0 * time));
    });
    ok &= writeFixture(weakHighPath, [](int, double time) {
        return 0.65f * static_cast<float>(std::sin(
                   2.0 * juce::MathConstants<double>::pi * 80.0 * time))
            + 0.025f * static_cast<float>(std::sin(
                   2.0 * juce::MathConstants<double>::pi * 9000.0 * time));
    });
    ok &= writeFixture(antiphasePath, sine(440.0, 0.65f), -1.0f);
    if (!ok)
        return 1;

    const Analysis silence = analyze(silencePath);
    const Analysis bass = analyze(bassPath);
    const Analysis mid = analyze(midPath);
    const Analysis high = analyze(highPath);
    const Analysis sweep = analyze(sweepPath);
    const Analysis dynamics = analyze(dynamicsPath);
    const Analysis transient = analyze(transientPath);
    const Analysis weakHigh = analyze(weakHighPath);
    const Analysis antiphase = analyze(antiphasePath);

    ok &= require(!bass.geometry.isEmpty() && !bass.spectral.isEmpty(),
                  "neutral analysis produced no data");
    ok &= require(bass.geometry.size() == kSeconds * kPointsPerSecond,
                  "geometry resolution changed");
    ok &= require(bass.spectral.size()
                      == kSeconds * TrackData::SPECTRAL_POINTS_PER_SECOND,
                  "spectral data is not stored at 150 points/second");
    ok &= require(bass.overview.size() == TrackData::kOverviewBins,
                  "fixed full-track overview is not 1200 points");
    ok &= require(std::all_of(
                     bass.overview.cbegin(), bass.overview.cend(),
                     [](const auto& point) { return point.peak > 0.01f; }),
                  "short-track overview contains unpopulated holes");
    ok &= require(bass.progressiveGeometry == bass.geometry
                      && bass.progressiveSpectral == bass.spectral,
                  "progressive and final neutral analysis did not converge");
    ok &= require(finite(silence) && finite(bass) && finite(mid)
                      && finite(high) && finite(sweep)
                      && finite(dynamics) && finite(transient)
                      && finite(antiphase),
                  "analysis emitted NaN or infinity");
    ok &= require(std::any_of(
                     antiphase.geometry.cbegin(), antiphase.geometry.cend(),
                     [](const auto& point) { return point.peak > 0.5f; }),
                  "opposite-phase stereo erased waveform geometry");

    const auto silenceMean = meanSpectrum(silence.spectral);
    const auto bassMean = meanSpectrum(bass.spectral);
    const auto midMean = meanSpectrum(mid.spectral);
    const auto highMean = meanSpectrum(high.spectral);
    const auto weakHighMean = meanSpectrum(weakHigh.spectral);
    ok &= require(silenceMean.rms < 1.0e-5f
                      && silenceMean.bass < 1.0e-5f
                      && silenceMean.mid < 1.0e-5f
                      && silenceMean.treble < 1.0e-5f,
                  "silence has non-zero neutral energy");
    ok &= require(bassMean.bass > bassMean.mid * 1.5f
                      && bassMean.bass > bassMean.treble * 2.0f,
                  "bass fixture is not bass-dominant");
    ok &= require(midMean.mid > midMean.bass * 1.5f
                      && midMean.mid > midMean.treble * 1.5f,
                  "mid fixture is not mid-dominant");
    ok &= require(highMean.treble > highMean.bass * 2.0f
                      && highMean.treble > highMean.mid * 1.5f,
                  "high fixture is not treble-dominant");
    ok &= require(weakHighMean.treble < weakHighMean.bass * 0.35f,
                  "weak treble was promoted by per-band normalization");

    const int halfGeometry = dynamics.geometry.size() / 2;
    double quiet = 0.0;
    double loud = 0.0;
    for (int index = 0; index < halfGeometry; ++index)
        quiet += dynamics.geometry[index].rms;
    for (int index = halfGeometry; index < dynamics.geometry.size(); ++index)
        loud += dynamics.geometry[index].rms;
    ok &= require(loud > quiet * 3.0,
                  "quiet intro and loud drop lost relative dynamics");

    std::vector<float> ordinary;
    ordinary.reserve(static_cast<std::size_t>(transient.geometry.size()));
    for (int index = 0; index < transient.geometry.size(); ++index) {
        if (std::abs(index - kPointsPerSecond) > 4)
            ordinary.push_back(transient.geometry[index].rms);
    }
    std::nth_element(ordinary.begin(),
                     ordinary.begin() + ordinary.size() / 2,
                     ordinary.end());
    ok &= require(ordinary[ordinary.size() / 2] > 0.25f,
                  "one clipping transient flattened ordinary waveform dynamics");
    ok &= require(!sweep.spectral.isEmpty(),
                  "frequency sweep analysis produced no spectral data");

    return ok ? 0 : 1;
}
