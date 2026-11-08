#pragma once

#include "stems/StemInference.h"
#include "stems/StemMixState.h"
#include "stems/StemTrackData.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace brockdj::stems {

struct StemTrackRequest {
    std::string filePath;
    std::uint64_t trackId = 0;   // AudioCacheHandle::id() of the deck's track
    double sampleRate = 0.0;
    std::int64_t lengthSamples = 0;
};

// Single background worker that separates tracks for all decks with one
// shared model instance. Work is split into ~7.8 s windows, ordered from each
// deck's playhead forward, so the audio the DJ is about to hear is ready
// first. Playing decks are served before cued ones. Finished tracks are cached
// on disk for reuse.
class StemSeparationService final
{
public:
    struct Config {
        std::filesystem::path cacheDir;
        std::uint64_t cacheBudgetBytes = 8ull * 1024ull * 1024ull * 1024ull;
    };

    StemSeparationService(Config config, StemInferenceFactory factory);
    ~StemSeparationService();
    StemSeparationService(const StemSeparationService&) = delete;
    StemSeparationService& operator=(const StemSeparationService&) = delete;

    // Replaces any job of `deck`. Status and mix state are owned by the deck.
    void submit(int deck, StemTrackRequest request, std::shared_ptr<StemJobStatus> status,
                std::shared_ptr<StemMixState> mix, bool background = false);
    // One library job at a time, below every live deck. Metadata is decoded
    // by the worker, not the UI thread.
    void submitPreanalysis(const std::string& filePath, std::shared_ptr<StemJobStatus> status);
    void cancelPreanalysis();
    // After cancel() returns, the worker will not publish to that deck again.
    void cancel(int deck);
    // Loads the model in the background before a track requests separation.
    void preload();
    // Drops the loaded model; the next job recreates it (e.g. after a download).
    void resetInference();
    void shutdown();

    [[nodiscard]] std::string lastError() const;
    [[nodiscard]] std::string inferenceBackend() const;
    [[nodiscard]] std::string acceleratorWarning() const;
    [[nodiscard]] bool inferenceLoaded() const noexcept { return m_inferenceLoaded.load(); }
    [[nodiscard]] std::filesystem::path cacheFileFor(const std::string& filePath) const;
    [[nodiscard]] std::vector<std::string> cachedFileNames() const;
    [[nodiscard]] std::uint64_t cacheRevision() const noexcept { return m_cacheRevision.load(); }

    static constexpr std::uint32_t kCacheVersion = 3;

private:
    struct Job;
    void workerLoop();
    std::shared_ptr<Job> pickJob();
    void runStep(Job& job);
    bool startJob(Job& job);
    bool processChunk(Job& job, int chunk);
    void finishJob(Job& job);
    void failJob(Job& job, const std::string& error);
    bool ensureInference();
    bool loadFromCache(Job& job);
    bool publishCachedJob(Job& job);
    // One-time envelope upgrade for legacy audio-only caches.
    bool buildCachedEnvelope(Job& job);
    bool writeCache(const Job& job);
    void pruneCache();

    Config m_config;
    StemInferenceFactory m_factory;
    std::unique_ptr<StemInference> m_inference;
    std::atomic<bool> m_inferenceLoaded { false };
    std::atomic<std::uint64_t> m_cacheRevision { 0 };

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<std::shared_ptr<Job>> m_jobs;
    std::shared_ptr<Job> m_running;
    bool m_shutdown = false;
    bool m_preloadRequested = false;
    bool m_resetRequested = false;
    std::uint64_t m_serial = 0;
    std::string m_lastError;
    std::string m_inferenceBackend;
    std::string m_acceleratorWarning;
    std::thread m_worker;
};

} // namespace brockdj::stems
