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
#include <vector>

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

class TestReader final : public juce::AudioFormatReader
{
public:
    explicit TestReader(std::unique_ptr<juce::AudioFormatReader> source)
        : juce::AudioFormatReader(nullptr, "test"), m_source(std::move(source))
    {
        sampleRate = m_source->sampleRate;
        lengthInSamples = m_source->lengthInSamples;
        numChannels = m_source->numChannels;
        bitsPerSample = m_source->bitsPerSample;
        usesFloatingPointData = m_source->usesFloatingPointData;
    }

    bool readSamples(int* const* channels, int count, int offset,
                     juce::int64 start, int samples) override
    {
        if (failReads)
            return false;
        std::vector<int*> destinations(static_cast<std::size_t>(count));
        for (int ch = 0; ch < count; ++ch)
            destinations[static_cast<std::size_t>(ch)] =
                channels[ch] ? channels[ch] + offset : nullptr;
        return m_source->read(destinations.data(), count, start, samples, false);
    }

    bool failReads = false;

private:
    std::unique_ptr<juce::AudioFormatReader> m_source;
};

enum class StopAt { None, Before, Priority, Gate, FullTrack, Paused, ReadFailure };

struct Analysis {
    QVector<TrackData::WaveformBin> geometry;
    QVector<TrackData::SpectralWaveformPoint> spectral;
    QVector<TrackData::SpectralWaveformPoint> overview;
    QVector<TrackData::WaveformBin> progressiveGeometry;
    QVector<TrackData::SpectralWaveformPoint> progressiveSpectral;
    QVector<TrackData::PeakFrame> peaks;
    QVector<int> priorityChunks;
    QVector<int> sequentialChunks;
    float globalMaxPeak = 0.0f;
    bool completed = false;
    bool acquiredSlot = false;
    bool validChunks = true;
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

Analysis analyze(const QString& path, double seekHint = 0.0,
                 StopAt stopAt = StopAt::None,
                 waveform::WaveformDemand demand = {})
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(
        format.createReaderFor(
            new juce::FileInputStream(juce::File(path.toStdString())), true));
    if (!reader)
        return {};
    auto controlledReader = std::make_unique<TestReader>(std::move(reader));
    reader = std::move(controlledReader);
    auto& testReader = static_cast<TestReader&>(*reader);

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
    Analysis result;
    if (stopAt == StopAt::Before)
        thread.signalThreadShouldExit();
    const waveform_internal::EnvelopePassInput input{
        *reader,
        &working,
        thread,
        kPointsPerSecond,
        0.0,
        [seekHint] { return seekHint; },
        [demand] { return demand; },
        [demand] { return demand.scratching; },
        [&] {
            if (result.acquiredSlot && stopAt == StopAt::Paused) {
                thread.signalThreadShouldExit();
                return true;
            }
            return false;
        },
        [&] {
            if (stopAt == StopAt::Gate)
                return false;
            result.acquiredSlot = true;
            testReader.failReads = stopAt == StopAt::ReadFailure;
            return true;
        },
        reader->lengthInSamples,
        reader->sampleRate,
        totalPoints,
        true,
        [&](int firstGeometry, int geometryTotal, QVector<TrackData::WaveformBin> geometry,
            int firstSpectral, int spectralTotal,
            QVector<TrackData::SpectralWaveformPoint> spectral,
            WaveformNormalizationState normalization) {
            constexpr int chunkSize = static_cast<int>(WaveformLineStore::kChunkSize);
            constexpr int spectralRatio = kPointsPerSecond
                / TrackData::SPECTRAL_POINTS_PER_SECOND;
            result.validChunks &= geometryTotal == totalPoints
                && spectralTotal == totalSpectralPoints
                && firstGeometry % chunkSize == 0
                && geometry.size() == std::min(chunkSize, totalPoints - firstGeometry)
                && firstSpectral == firstGeometry / spectralRatio
                && spectral.size() == (geometry.size() + spectralRatio - 1) / spectralRatio
                && normalization == WaveformNormalizationState::Final;
            (result.acquiredSlot ? result.sequentialChunks : result.priorityChunks)
                .push_back(firstGeometry);
            std::copy(geometry.cbegin(), geometry.cend(),
                      progressiveGeometry.begin() + firstGeometry);
            std::copy(spectral.cbegin(), spectral.cend(),
                      progressiveSpectral.begin() + firstSpectral);
            if ((!result.acquiredSlot && stopAt == StopAt::Priority)
                || (result.acquiredSlot && stopAt == StopAt::FullTrack))
                thread.signalThreadShouldExit();
        }
    };
    result.completed = waveform_internal::runEnvelopePass(input);
    result.geometry = working.getWaveformData();
    result.spectral = working.getSpectralWaveformData();
    result.overview = working.getOverviewWaveformData();
    result.progressiveGeometry = std::move(progressiveGeometry);
    result.progressiveSpectral = std::move(progressiveSpectral);
    result.peaks = working.getPeakMipData();
    result.globalMaxPeak = working.getGlobalMaxPeak();
    return result;
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

bool constantNumerics(const QString& path, const Analysis& analysis)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(
        new juce::FileInputStream(juce::File(path.toStdString())), true));
    juce::AudioBuffer<float> sample(2, 1);
    if (!reader || !reader->read(&sample, 0, 1, 0, true, true))
        return false;
    const float value = sample.getSample(0, 0);
    const auto coefficient = [](float frequency) {
        const float w = 2.0f * juce::MathConstants<float>::pi * frequency
            / static_cast<float>(kSampleRate);
        return w / (w + 1.0f);
    };
    const float low = coefficient(400.0f);
    const float high = coefficient(4000.0f);
    const auto shape = [](float value) {
        return std::pow(std::clamp(value, 0.0f, 1.0f), 0.72f);
    };
    const auto close = [](float actual, float expected) {
        return std::abs(actual - expected) < 1.0e-6f;
    };
    constexpr int spectralRatio = kPointsPerSecond
        / TrackData::SPECTRAL_POINTS_PER_SECOND;
    constexpr int samplesPerBin = static_cast<int>(kSampleRate) / kPointsPerSecond;
    float low1 = 0.0f, low2 = 0.0f, mid1 = 0.0f, mid2 = 0.0f;
    float high1 = 0.0f, high2 = 0.0f;
    float globalMax = 0.001f;
    double spectralBass = 0.0, spectralMid = 0.0, spectralTreble = 0.0;
    for (int bin = 0; bin < analysis.geometry.size(); ++bin) {
        double bassSquares = 0.0, midSquares = 0.0, trebleSquares = 0.0;
        for (int s = 0; s < samplesPerBin; ++s) {
            low1 += low * (value - low1);
            low2 += low * (low1 - low2);
            mid1 += high * (value - mid1);
            mid2 += high * (mid1 - mid2);
            high1 += high * (value - high1);
            const float highPass = value - high1;
            high2 += high * (highPass - high2);
            const float bass = std::abs(low2);
            const float mid = std::abs(mid2 - low2);
            const float treble = std::abs(highPass - high2);
            bassSquares += static_cast<double>(bass) * bass;
            midSquares += static_cast<double>(mid) * mid;
            trebleSquares += static_cast<double>(treble) * treble;
        }
        const float bass = std::sqrt(static_cast<float>(bassSquares) / samplesPerBin);
        const float mid = std::sqrt(static_cast<float>(midSquares) / samplesPerBin);
        const float treble = std::sqrt(static_cast<float>(trebleSquares) / samplesPerBin);
        globalMax = std::max({globalMax, bass, mid, treble});
        const auto& geometry = analysis.geometry[bin];
        if (!close(geometry.minimum, 0.0f)
            || !close(geometry.maximum, value / 0.05f)
            || !close(geometry.peak, shape(value / 0.05f))
            || !close(geometry.rms, shape(value / 0.02f)))
            return false;
        const float shapedBass = shape(bass / 0.02f);
        const float shapedMid = shape(mid / 0.02f);
        const float shapedTreble = shape(treble / 0.02f);
        spectralBass += shapedBass * shapedBass;
        spectralMid += shapedMid * shapedMid;
        spectralTreble += shapedTreble * shapedTreble;
        if ((bin + 1) % spectralRatio == 0) {
            const auto& spectral = analysis.spectral[bin / spectralRatio];
            if (!close(spectral.peak, shape(value / 0.05f))
                || !close(spectral.rms, shape(value / 0.02f))
                || !close(spectral.bass, std::sqrt(static_cast<float>(spectralBass) / spectralRatio))
                || !close(spectral.mid, std::sqrt(static_cast<float>(spectralMid) / spectralRatio))
                || !close(spectral.treble, std::sqrt(static_cast<float>(spectralTreble) / spectralRatio)))
                return false;
            spectralBass = spectralMid = spectralTreble = 0.0;
        }
    }
    const auto expectedPeak = static_cast<qint8>(value * (127.0f / value));
    return close(analysis.globalMaxPeak, globalMax)
        && analysis.peaks.size() == kSeconds * TrackData::PEAK_POINTS_PER_SECOND
        && std::all_of(analysis.peaks.cbegin(), analysis.peaks.cend(),
            [expectedPeak](const auto& peak) {
                return peak.minSample == expectedPeak && peak.maxSample == expectedPeak;
            });
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir(QStringLiteral("./neutral-waveform-analysis-XXXXXX"));
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
    const QString constantPath = dir.filePath("constant.wav");

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
    ok &= writeFixture(constantPath, [](int, double) { return 0.01f; });
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
    const Analysis constant = analyze(constantPath, 1.0);
    const Analysis soughtBass = analyze(bassPath, 1.0);
    waveform::WaveformDemand reverseDemand;
    reverseDemand.reverse = true;
    const Analysis reverseBass = analyze(bassPath, 1.0, StopAt::None, reverseDemand);
    waveform::WaveformDemand scratchDemand;
    scratchDemand.scratching = true;
    const Analysis scratchBass = analyze(bassPath, 1.0, StopAt::None, scratchDemand);

    constexpr int chunkSize = static_cast<int>(WaveformLineStore::kChunkSize);
    QVector<int> expectedChunks;
    for (int first = 0; first < kSeconds * kPointsPerSecond; first += chunkSize)
        expectedChunks.push_back(first);
    ok &= require(bass.completed && bass.acquiredSlot && bass.validChunks
                      && bass.priorityChunks == expectedChunks
                      && bass.sequentialChunks == expectedChunks,
                  "priority/sequential chunk boundaries or publication changed");
    ok &= require(soughtBass.completed && soughtBass.validChunks
                      && !soughtBass.priorityChunks.isEmpty()
                      && soughtBass.priorityChunks.front()
                          == (kPointsPerSecond / chunkSize) * chunkSize
                      && soughtBass.sequentialChunks == expectedChunks
                      && soughtBass.geometry == bass.geometry
                      && soughtBass.spectral == bass.spectral
                      && soughtBass.peaks.size() == bass.peaks.size()
                      && std::equal(soughtBass.peaks.cbegin(), soughtBass.peaks.cend(),
                          bass.peaks.cbegin(), [](const auto& left, const auto& right) {
                              return left.minSample == right.minSample
                                  && left.maxSample == right.maxSample;
                          })
                      && soughtBass.globalMaxPeak == bass.globalMaxPeak,
                  "seek prologue changed full-track numerics or priority order");
    ok &= require(constant.completed && constantNumerics(constantPath, constant),
                  "continuous filter state, normalization or peak numerics changed");
    const QVector<int> reverseChunks{chunkSize, 0, 2 * chunkSize};
    ok &= require(reverseBass.completed && scratchBass.completed
                      && reverseBass.validChunks && scratchBass.validChunks
                      && reverseBass.priorityChunks == reverseChunks
                      && scratchBass.priorityChunks == reverseChunks
                      && reverseBass.geometry == bass.geometry
                      && reverseBass.spectral == bass.spectral
                      && scratchBass.geometry == bass.geometry
                      && scratchBass.spectral == bass.spectral,
                  "reverse/scratch demand changed priority order or final numerics");
    for (const auto stopAt : {StopAt::Before, StopAt::Priority, StopAt::Gate,
                             StopAt::FullTrack, StopAt::Paused, StopAt::ReadFailure}) {
        const Analysis stopped = analyze(bassPath, 0.0, stopAt);
        ok &= require(!stopped.completed && stopped.peaks.isEmpty()
                          && stopped.validChunks,
                      "cancelled/failed pass finalized peaks or reported success");
        if (stopAt == StopAt::Before)
            ok &= require(stopped.priorityChunks.isEmpty() && !stopped.acquiredSlot,
                          "pre-cancelled pass published chunks or acquired the gate");
        if (stopAt == StopAt::Priority)
            ok &= require(stopped.priorityChunks.size() == 1 && !stopped.acquiredSlot,
                          "prologue cancellation continued publishing or acquired the gate");
        if (stopAt == StopAt::FullTrack)
            ok &= require(stopped.sequentialChunks.size() == 1,
                          "full-track cancellation continued publishing chunks");
        else
            ok &= require(stopped.sequentialChunks.isEmpty(),
                          "blocked/cancelled/failed pass published sequential chunks");
    }

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
