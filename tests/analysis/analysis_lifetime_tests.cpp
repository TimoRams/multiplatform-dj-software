#include "TrackData.h"
#include "WaveformAnalyzer.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool writeSilentWave(const QString& path)
{
    juce::WavAudioFormat format;
    auto stream = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!stream->openedOk())
        return false;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.release(), 44100.0, 2, 16, {}, 0));
    if (!writer)
        return false;

    juce::AudioBuffer<float> silence(2, 44100);
    silence.clear();
    for (int second = 0; second < 30; ++second) {
        if (!writer->writeFromAudioSampleBuffer(silence, 0, silence.getNumSamples()))
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    bool ok = true;
    {
        TrackData data;
        WaveformAnalyzer analyzer(&formats, 600);
        std::atomic<int> callbacks{0};
        std::atomic<WaveformAnalyzer::AnalysisGeneration> callbackGeneration{0};
        analyzer.setCompletionCallback(
            [&](bool completed, WaveformAnalyzer::AnalysisGeneration generation, const QString&,
                WaveformAnalyzer::ResultPtr) {
                ok &= require(!completed, "an unreadable file must finish as failed");
                callbackGeneration.store(generation, std::memory_order_release);
                callbacks.fetch_add(1, std::memory_order_release);
            });

        const auto first = analyzer.startAnalysis(QStringLiteral("/not/a/track.wav"));
        analyzer.shutdownAndJoin();
        ok &= require(callbacks.load(std::memory_order_acquire) == 1,
                      "failed analysis must notify exactly once");
        ok &= require(callbackGeneration.load(std::memory_order_acquire) == first,
                      "completion must carry its captured generation");
        ok &= require(analyzer.jobState() != WaveformAnalyzer::AnalysisJobState::Running,
                      "joined failed analysis must have a terminal state");

        const auto second = analyzer.startAnalysis(QStringLiteral("/also/not/a/track.wav"));
        analyzer.shutdownAndJoin();
        ok &= require(second > first, "repeated analysis of the same owner needs a new generation");
    }

    QTemporaryDir dir;
    const QString wavePath = dir.filePath(QStringLiteral("cancel.wav"));
    ok &= require(dir.isValid() && writeSilentWave(wavePath), "test wave must be writable");
    if (ok) {
        WaveformAnalyzer analyzer(&formats, 600);
        std::mutex chunkMutex;
        std::condition_variable chunkReady;
        int firstRgbBin = -1;
        analyzer.setChunkCallback(
            [&](WaveformAnalyzer::AnalysisGeneration, int firstBin, int,
                QVector<TrackData::WaveformBin>,
                QVector<TrackData::RgbWaveformFrame> rgb) {
                if (rgb.isEmpty())
                    return;
                {
                    std::lock_guard lock(chunkMutex);
                    if (firstRgbBin < 0)
                        firstRgbBin = firstBin;
                }
                chunkReady.notify_one();
            });
        analyzer.startAnalysis(wavePath, 15.0);
        {
            std::unique_lock lock(chunkMutex);
            ok &= require(chunkReady.wait_for(lock, std::chrono::seconds(5),
                                              [&] { return firstRgbBin >= 0; }),
                          "cursor-priority waveform chunk must be published promptly");
        }
        // 0.5 s of context at 600 pps starts 300 bins before the cursor.
        ok &= require(firstRgbBin == 15 * 600 - 300,
                      "first lazy-loaded chunk must start at the cursor context window");
        analyzer.shutdownAndJoin();
    }
    if (ok) {
        TrackData data;
        auto analyzer = std::make_unique<WaveformAnalyzer>(&formats, 600);
        std::atomic<int> acceptedCompletions{0};
        analyzer->setCompletionCallback(
            [&](bool completed, WaveformAnalyzer::AnalysisGeneration, const QString&,
                WaveformAnalyzer::ResultPtr) {
                if (completed)
                    acceptedCompletions.fetch_add(1, std::memory_order_relaxed);
            });
        analyzer->startAnalysis(wavePath);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        analyzer->requestCancel();
        analyzer.reset(); // destructor must join before TrackData leaves scope
        ok &= require(acceptedCompletions.load(std::memory_order_relaxed) == 0,
                      "cancelled/stale generation must not be accepted as complete");
        ok &= require(!data.isAnalyzing(), "cancelled analysis must clear active progress state");
    }

    return ok ? 0 : 1;
}
