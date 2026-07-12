#include "engine/deck/DeckTrackLoader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool writeWave(const QString& path, double sampleRate, int channels, double seconds)
{
    juce::WavAudioFormat format;
    auto fileStream = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!fileStream->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
    const auto options = juce::AudioFormatWriterOptions{}
        .withSampleRate(sampleRate)
        .withNumChannels(channels)
        .withBitsPerSample(16);
    auto writer = format.createWriterFor(stream, options);
    if (!writer) return false;
    const int samples = static_cast<int>(sampleRate * seconds);
    juce::AudioBuffer<float> buffer(channels, samples);
    for (int channel = 0; channel < channels; ++channel)
        for (int i = 0; i < samples; ++i)
            buffer.setSample(channel, i, i < 128 ? 0.0f
                : static_cast<float>(0.15 * std::sin(2.0 * juce::MathConstants<double>::pi
                                                     * 440.0 * i / sampleRate)));
    return writer->writeFromAudioSampleBuffer(buffer, 0, samples);
}

struct ResultWaiter {
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        std::optional<TrackLoadResult> result;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    DeckTrackLoader::CompletionCallback callback()
    {
        return [state = state](TrackLoadResult incoming) {
            {
                std::lock_guard lock(state->mutex);
                state->result.emplace(std::move(incoming));
            }
            state->condition.notify_one();
        };
    }

    bool wait(std::chrono::seconds timeout = std::chrono::seconds(10))
    {
        std::unique_lock lock(state->mutex);
        return state->condition.wait_for(lock, timeout, [this] { return state->result.has_value(); });
    }

    TrackLoadResult& value() { return *state->result; }
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    QTemporaryDir directory;
    ok &= require(directory.isValid(), "temporary directory must be available");
    const QString monoPath = directory.filePath(QStringLiteral("Artist - Mono.wav"));
    const QString stereoPath = directory.filePath(QStringLiteral("Stereo.wav"));
    ok &= require(writeWave(monoPath, 44100.0, 1, 0.2), "mono fixture must be generated");
    ok &= require(writeWave(stereoPath, 48000.0, 2, 0.3), "stereo fixture must be generated");

    DeckTrackLoader loader(100);
    {
        ResultWaiter waiter;
        const auto generation = loader.loadTrack(monoPath, waiter.callback());
        ok &= require(waiter.wait(), "valid mono load must complete");
        ok &= require(waiter.value().succeeded(), "valid mono load must succeed");
        ok &= require(waiter.value().generation == generation, "result generation must match request");
        ok &= require(waiter.value().metadata.channelCount == 1, "mono channel count must be retained");
        ok &= require(std::abs(waiter.value().metadata.sampleRate - 44100.0) < 0.1,
                      "mono sample rate must be retained");
        ok &= require(waiter.value().metadata.title == QStringLiteral("Mono"),
                      "filename metadata fallback must be retained");
        ok &= require(waiter.value().bufferedReader && waiter.value().directReader,
                      "two prepared playback readers must be returned");
    }
    {
        ResultWaiter waiter;
        loader.loadTrack(QString(), waiter.callback());
        ok &= require(waiter.wait(), "empty path must produce a result");
        ok &= require(waiter.value().error == TrackLoadError::EmptyPath,
                      "empty path must have a defined error");
    }
    {
        ResultWaiter waiter;
        loader.loadTrack(directory.filePath(QStringLiteral("missing.wav")), waiter.callback());
        ok &= require(waiter.wait(), "missing path must produce a result");
        ok &= require(waiter.value().error == TrackLoadError::FileNotFound,
                      "missing file must have a defined error");
    }
    {
        const QString badPath = directory.filePath(QStringLiteral("damaged.wav"));
        QFile bad(badPath);
        ok &= require(bad.open(QIODevice::WriteOnly), "damaged fixture must open");
        bad.write("not audio");
        bad.close();
        ResultWaiter waiter;
        loader.loadTrack(badPath, waiter.callback());
        ok &= require(waiter.wait(), "damaged file must produce a result");
        ok &= require(waiter.value().error == TrackLoadError::UnsupportedFormat,
                      "damaged file must fail decoder creation");
    }
    {
        struct CompletionState {
            std::mutex mutex;
            std::condition_variable condition;
            std::vector<std::uint64_t> completed;
        };
        auto state = std::make_shared<CompletionState>();
        const auto collect = [state](TrackLoadResult result) {
            {
                std::lock_guard lock(state->mutex);
                state->completed.push_back(result.generation);
            }
            state->condition.notify_one();
        };
        loader.loadTrack(monoPath, collect);
        loader.loadTrack(stereoPath, collect);
        loader.loadTrack(monoPath, collect);
        const auto current = loader.loadTrack(stereoPath, collect);
        std::unique_lock lock(state->mutex);
        ok &= require(state->condition.wait_for(lock, std::chrono::seconds(10), [&] {
                          return !state->completed.empty() && state->completed.back() == current;
                      }), "latest rapid load must complete");
        ok &= require(state->completed.size() == 1 && state->completed.front() == current,
                      "only the current generation may publish a result");
    }

    loader.requestCancel();
    ok &= require(loader.state() == TrackLoadState::CancelRequested
                      || loader.state() == TrackLoadState::Cancelled,
                  "explicit cancel has a defined state");
    loader.shutdownAndJoin();
    ok &= require(loader.state() == TrackLoadState::ShuttingDown,
                  "shutdown has a terminal state and joins the worker");

    {
        DeckTrackLoader destructorJoin(100);
        destructorJoin.loadTrack(stereoPath, [](TrackLoadResult) {});
    }

    return ok ? 0 : 1;
}
