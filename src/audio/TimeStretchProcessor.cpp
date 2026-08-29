#include "TimeStretchProcessor.h"

#include "../platform/AudioThreadScheduling.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
thread_local bool g_inTimeStretchAudioCallback = false;

void configureWorkerPriority() noexcept
{
#ifdef __linux__
    // This thread services keylock seeds, which gate live audio: if it is not
    // scheduled promptly the audio thread has to take the seed itself and blow
    // its budget. Ask for a real-time band just below the callback, and settle
    // for normal priority when the host does not allow it.
    sched_param parameters {};
    const int minimum = sched_get_priority_min(SCHED_FIFO);
    const int maximum = sched_get_priority_max(SCHED_FIFO);
    if (minimum >= 0 && maximum >= minimum) {
        parameters.sched_priority = std::clamp(
            platform::AudioThreadScheduling::kDefaultRealtimePriority - 4, minimum, maximum);
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters) == 0)
            return;
    }
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 0);
#endif
}
}

TimeStretchProcessor::TimeStretchProcessor(juce::AudioSource* inSource) : source(inSource) {}
TimeStretchProcessor::~TimeStretchProcessor() { releaseResources(); }

bool TimeStretchProcessor::validConfiguration(const TimeStretchConfiguration& c) noexcept
{
    return std::isfinite(c.sampleRate) && c.sampleRate >= 8000.0 && c.sampleRate <= 384000.0
        && std::isfinite(c.tempoRatio) && c.tempoRatio >= 0.01 && c.tempoRatio <= 8.0
        && c.maximumBlockSize >= 64 && c.maximumBlockSize <= 8192
        && c.channelCount >= 1 && c.channelCount <= 2 && c.trackGeneration != 0;
}

void TimeStretchProcessor::setTempoRatio(double ratio) noexcept
{
    const auto clamped = std::clamp(std::isfinite(ratio) ? ratio : 1.0, 0.01, 8.0);
    // Rubber Band accepts real-time ratio and pitch changes from the same
    // callback as process(). Rebuilding a worker pipeline for every jog tick
    // causes the very stalls this path is meant to avoid.
    m_targetTempoRatio.store(clamped, std::memory_order_release);
}

void TimeStretchProcessor::setPitchLockEnabled(bool enabled) noexcept
{
    // Keylock is a render-time decision, not part of the pipeline identity: the
    // stretcher is configured identically whether or not the pitch is locked,
    // and only the transpose factor differs. Toggling therefore no longer waits
    // for a worker rebuild — that wait is what used to leave the deck playing at
    // the unlocked (tempo-shifted) pitch until the new pipeline arrived, and
    // then snap back audibly.
    m_pitchLockEnabled.store(enabled, std::memory_order_release);
}

void TimeStretchProcessor::setKeySemitoneOffset(double semitones) noexcept
{
    m_keySemitoneOffset.store(std::isfinite(semitones) ? semitones : 0.0,
                               std::memory_order_release);
}

void TimeStretchProcessor::setInputPlaybackActive(bool active) noexcept
{
    if (m_inputPlaybackActive.exchange(active, std::memory_order_acq_rel) != active)
        m_keylockSeedPending.store(true, std::memory_order_release);
}

void TimeStretchProcessor::setBackend(TimeStretchBackend backend) noexcept
{
    if (m_backend.exchange(backend, std::memory_order_acq_rel) != backend)
        publishDesiredConfiguration();
}

void TimeStretchProcessor::setScratchBypass(bool enabled) noexcept
{
    m_scratchBypass.store(enabled, std::memory_order_release);
}

void TimeStretchProcessor::setTrackGeneration(std::uint64_t generation) noexcept
{
    generation = std::max<std::uint64_t>(1, generation);
    // A new track does not change how the stretcher is built, so it must not
    // cost a rebuild: the deck has to be playable the instant it is loaded.
    // Re-seeding clears the previous track's spectral state on the next keylock
    // block instead.
    if (m_trackGeneration.exchange(generation, std::memory_order_acq_rel) != generation)
        m_keylockSeedPending.store(true, std::memory_order_release);
}

void TimeStretchProcessor::enterScratchBypass() noexcept
{
    m_scratchBypass.store(true, std::memory_order_release);
    m_reportedLatencySamples.store(0, std::memory_order_relaxed);
}

void TimeStretchProcessor::endScratchBypass() noexcept
{
    // The keylock pipeline survived the scratch, so normal playback resumes on
    // the very next block. Only the stretcher's continuity has to be restored,
    // which the render path does by seeding it with the audio just heard.
    m_keylockSeedPending.store(true, std::memory_order_release);
    m_scratchBypass.store(false, std::memory_order_release);
}

TimeStretchConfiguration TimeStretchProcessor::desiredConfiguration() const noexcept
{
    TimeStretchConfiguration c;
    c.sampleRate = m_sampleRate.load(std::memory_order_acquire);
    c.tempoRatio = m_targetTempoRatio.load(std::memory_order_acquire);
    c.maximumBlockSize = m_maximumBlockSize.load(std::memory_order_acquire);
    // Always build a keylock-capable pipeline. Whether the pitch is actually
    // locked is decided per block, so this keeps a standby stretcher ready and
    // makes every keylock/scratch transition a same-block handover.
    c.keylockEnabled = true;
    c.backend = m_backend.load(std::memory_order_acquire);
    c.trackGeneration = m_trackGeneration.load(std::memory_order_acquire);
    c.configurationGeneration = m_desiredGeneration.load(std::memory_order_acquire);
    return c;
}

void TimeStretchProcessor::publishDesiredConfiguration() noexcept
{
    if (!m_accepting.load(std::memory_order_acquire)) return;
    m_desiredGeneration.fetch_add(1, std::memory_order_acq_rel);
    wakeWorker();
}

void TimeStretchProcessor::prepareToPlay(int blockSize, double sr)
{
    stopWorker();
    if (source) source->prepareToPlay(blockSize, sr);
    m_sampleRate.store(sr, std::memory_order_release);
    m_maximumBlockSize.store(std::clamp(blockSize, 64, 8192), std::memory_order_release);
    m_trackGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_desiredGeneration.store(1, std::memory_order_release);
    m_activeGeneration.store(0, std::memory_order_release);
    m_activeSlot.store(-1, std::memory_order_release);
    m_scratchBypass.store(false, std::memory_order_release);
    m_keylockSeedPending.store(true, std::memory_order_release);
    m_keylockRenderActive = false;
    m_accepting.store(true, std::memory_order_release);
    m_prepared.store(true, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);
    resizeBuffer(m_previousTail, 2, kSwitchFadeSamples);
    m_previousTail.clear();
    resizeBuffer(m_outputHistory, 2, kOutputHistorySamples);
    resizeBuffer(m_historyScratch, 2, kOutputHistorySamples);
    resizeBuffer(m_seedSnapshot, 2, kOutputHistorySamples);
    m_outputHistory.clear();
    m_historyScratch.clear();
    m_seedSnapshot.clear();
    m_seedState.store(SeedState::Idle, std::memory_order_release);
    m_seedSlot.store(-1, std::memory_order_release);
    m_seedSnapshotLength = 0;
    m_seedBridgeSamples = 0;
    m_historyWrite = 0;

    auto initial = desiredConfiguration();
    initial.configurationGeneration = 1;
    if (!preparePipeline(m_pipelines[0], initial)) {
        m_failures.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_pipelines[0].state.store(SlotState::Active, std::memory_order_release);
        m_activeSlot.store(0, std::memory_order_release);
        m_activeGeneration.store(1, std::memory_order_release);
        m_activeBackend.store(initial.backend, std::memory_order_release);
    }
    m_pipelines[1].state.store(SlotState::Empty, std::memory_order_release);
    m_worker = std::thread([this] {
        configureWorkerPriority();
        workerLoop();
    });
}

void TimeStretchProcessor::stopWorker() noexcept
{
    m_accepting.store(false, std::memory_order_release);
    m_stopRequested.store(true, std::memory_order_release);
    wakeWorker();
    if (m_worker.joinable()) m_worker.join();
}

void TimeStretchProcessor::releaseResources()
{
    if (!m_prepared.exchange(false, std::memory_order_acq_rel)) return;
    stopWorker();
    if (source) source->releaseResources();
    m_activeSlot.store(-1, std::memory_order_release);
    for (auto& p : m_pipelines) {
        p.state.store(SlotState::Empty, std::memory_order_release);
        p.rubberBand.reset();
        p.signalsmith.reset();
        p.fifo.reset();
        resizeBuffer(p.input, 0, 0);
        resizeBuffer(p.output, 0, 0);
        resizeBuffer(p.trim, 0, 0);
        resizeBuffer(p.zeros, 0, 0);
    }
    resizeBuffer(m_previousTail, 0, 0);
}

bool TimeStretchProcessor::preparePipeline(Pipeline& p, const TimeStretchConfiguration& c)
{
    if (g_inTimeStretchAudioCallback) m_prepareFromAudio.fetch_add(1, std::memory_order_relaxed);
    if (!validConfiguration(c)) return false;
    p.appliedPitchScale = c.keylockEnabled ? 1.0 / c.tempoRatio : 1.0;
    p.rubberBand.reset();
    p.signalsmith.reset();
    if (c.backend == TimeStretchBackend::RubberBand) {
        p.rubberBand = std::make_unique<RubberBand::RubberBandStretcher>(c.sampleRate, c.channelCount,
            RubberBand::RubberBandStretcher::OptionProcessRealTime |
            RubberBand::RubberBandStretcher::OptionWindowShort |
            RubberBand::RubberBandStretcher::OptionPitchHighSpeed);
        p.rubberBand->setMaxProcessSize(static_cast<size_t>(std::min(4096, std::max(512, c.maximumBlockSize))));
        p.rubberBand->setTimeRatio(1.0);
        p.rubberBand->setPitchScale(p.appliedPitchScale);
    } else {
        p.signalsmith = std::make_unique<signalsmith::stretch::SignalsmithStretch<float>>();
        // A shorter window than the library default keeps keylock responsive
        // enough for live use; see kKeylockWindowSeconds for the trade-off.
        const int blockSamples = std::clamp(
            static_cast<int>(std::lround(c.sampleRate * kKeylockWindowSeconds)), 512, 8192);
        const int intervalSamples = std::clamp(blockSamples / kKeylockOverlap, 64, 2048);
        // A single FFT burst can consume most of a 64/128-sample callback on a
        // laptop CPU. Signalsmith can spread the analysis/synthesis steps across
        // one hop; the ~8 ms added latency is preferable to a deadline miss and
        // an audible keylock crackle.
        p.signalsmith->configure(c.channelCount, blockSamples, intervalSamples, true);
        p.tonalityLimit = kKeylockTonalityLimitHz / c.sampleRate;
        p.signalsmith->setTransposeFactor(static_cast<float>(p.appliedPitchScale),
                                          static_cast<float>(p.tonalityLimit));
    }
    resizeBuffer(p.input, 2, std::max(4096, c.maximumBlockSize));
    resizeBuffer(p.output, 2, kFifoCapacity);
    resizeBuffer(p.trim, 2, 4096);
    resizeBuffer(p.zeros, 2, p.signalsmith
                                 ? std::max(4096, p.signalsmith->outputSeekLength(1.0f))
                                 : 4096);
    p.zeros.clear();
    p.fifo = std::make_unique<juce::AbstractFifo>(kFifoCapacity);
    p.config = c;
    p.prefill = 0;
    prewarmPipeline(p);
    p.latency = p.rubberBand
        ? static_cast<int>(p.rubberBand->getLatency())
        : p.signalsmith->inputLatency() + p.signalsmith->outputLatency();
    return true;
}

void TimeStretchProcessor::resizeBuffer(juce::AudioBuffer<float>& buffer, int channels, int samples)
{
    if (g_inTimeStretchAudioCallback) m_growthFromAudio.fetch_add(1, std::memory_order_relaxed);
    buffer.setSize(channels, samples);
}

void TimeStretchProcessor::prewarmPipeline(Pipeline& p)
{
    if (g_inTimeStretchAudioCallback) m_prewarmFromAudio.fetch_add(1, std::memory_order_relaxed);
    if (p.signalsmith) {
        // Running silence through the stretcher does not shorten its startup
        // gap by a single sample — the delay is inherent, so all it would do is
        // burn input. What this pass is actually for is the allocations inside
        // outputSeek(): doing one here, on the worker thread, leaves the vectors
        // at full capacity so the real seed at activation time can run on the
        // audio thread without touching the allocator.
        const int seedLength = std::min(p.zeros.getNumSamples(),
                                        p.signalsmith->outputSeekLength(1.0f));
        if (seedLength > 0)
            p.signalsmith->outputSeek(p.zeros.getArrayOfReadPointers(), seedLength);
        return;
    }
    if (!p.rubberBand) return;
    int remaining = static_cast<int>(p.rubberBand->getPreferredStartPad() + p.rubberBand->getStartDelay());
    for (int guard = 256; remaining > 0 && guard-- > 0;) {
        const int chunk = std::min(remaining, p.zeros.getNumSamples());
        const float* in[2] { p.zeros.getReadPointer(0), p.zeros.getReadPointer(1) };
        p.rubberBand->process(in, chunk, false);
        int available = p.rubberBand->available();
        if (available > 0) {
            const int take = std::min({remaining, available, p.trim.getNumSamples()});
            float* out[2] { p.trim.getWritePointer(0), p.trim.getWritePointer(1) };
            p.rubberBand->retrieve(out, take);
            remaining -= take;
        }
    }
}

void TimeStretchProcessor::workerLoop()
{
    std::uint64_t observed = m_activeGeneration.load(std::memory_order_acquire);
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // Read the ticket before testing for work: any producer that bumps it
        // afterwards makes the wait return immediately, so no wakeup is lost.
        const auto ticket = m_workerTicket.load(std::memory_order_acquire);
        if (m_seedState.load(std::memory_order_acquire) != SeedState::Requested
            && m_desiredGeneration.load(std::memory_order_acquire) == observed
            && !m_stopRequested.load(std::memory_order_acquire)) {
            m_workerTicket.wait(ticket, std::memory_order_acquire);
        }
        if (m_stopRequested.load(std::memory_order_acquire)) break;
        // Seeds gate live audio, so they always win over a rebuild.
        if (serviceSeedRequest()) continue;
        if (m_desiredGeneration.load(std::memory_order_acquire) == observed) continue;
        auto config = desiredConfiguration();
        observed = config.configurationGeneration;
        const int active = m_activeSlot.load(std::memory_order_acquire);
        const int candidate = active == 0 ? 1 : 0;
        auto& p = m_pipelines[candidate];
        SlotState expected = SlotState::Empty;
        if (!p.state.compare_exchange_strong(expected, SlotState::Preparing, std::memory_order_acq_rel)) {
            if (expected == SlotState::Ready && p.config.configurationGeneration != config.configurationGeneration) {
                p.state.store(SlotState::Empty, std::memory_order_release);
                m_stale.fetch_add(1, std::memory_order_relaxed);
                observed = m_activeGeneration.load(std::memory_order_acquire);
                wakeWorker();
            }
            continue;
        }
        if (!preparePipeline(p, config)) {
            p.state.store(SlotState::Empty, std::memory_order_release);
            m_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const auto latest = m_desiredGeneration.load(std::memory_order_acquire);
        if (latest != config.configurationGeneration) {
            p.state.store(SlotState::Empty, std::memory_order_release);
            m_stale.fetch_add(1, std::memory_order_relaxed);
            observed = m_activeGeneration.load(std::memory_order_acquire);
            wakeWorker();
            continue;
        }
        p.state.store(SlotState::Ready, std::memory_order_release);
    }
}

void TimeStretchProcessor::activatePreparedPipelineAtBlockBoundary() noexcept
{
    const auto wanted = m_desiredGeneration.load(std::memory_order_acquire);
    for (int i = 0; i < 2; ++i) {
        auto& next = m_pipelines[i];
        if (next.state.load(std::memory_order_acquire) != SlotState::Ready) continue;
        SlotState ready = SlotState::Ready;
        if (!next.state.compare_exchange_strong(ready, SlotState::Active, std::memory_order_acq_rel)) continue;
        if (next.config.configurationGeneration != wanted) {
            next.state.store(SlotState::Empty, std::memory_order_release);
            wakeWorker();
            continue;
        }
        const int old = m_activeSlot.exchange(i, std::memory_order_acq_rel);
        m_activeGeneration.store(next.config.configurationGeneration, std::memory_order_release);
        m_activeBackend.store(next.config.backend, std::memory_order_release);
        // The freshly built stretcher has no history. Seeding is deferred to the
        // render path so it only costs anything when keylock is actually in use.
        m_keylockSeedPending.store(true, std::memory_order_release);
        m_switches.fetch_add(1, std::memory_order_relaxed);
        if (old >= 0 && old != i) m_pipelines[old].state.store(SlotState::Empty, std::memory_order_release);
        wakeWorker();
        break;
    }
}

void TimeStretchProcessor::beginTransitionFade() noexcept
{
    readOutputHistory(m_previousTail, kSwitchFadeSamples);
    m_switchFadeRemaining.store(kSwitchFadeSamples, std::memory_order_relaxed);
}

void TimeStretchProcessor::prepareKeylockTransition(Pipeline& p) noexcept
{
    // Entering keylock from a discontinuity (toggle, scratch release, new
    // track) must not replay whatever the stretcher still held internally.
    if (p.fifo) p.fifo->reset();
    if (!p.signalsmith) {
        // Rubber Band keeps its internal state deliberately: a reset() here
        // would re-arm its start pad, and refilling that inside the callback
        // costs far more than the small spectral discontinuity the crossfade
        // already covers.
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    seedPipelineFromHistory(p);
    recordSeedDuration(started);
}

void TimeStretchProcessor::recordSeedDuration(std::chrono::steady_clock::time_point started) noexcept
{
    const auto micros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    m_keylockSeeds.fetch_add(1, std::memory_order_relaxed);
    auto worst = m_worstSeedMicros.load(std::memory_order_relaxed);
    while (worst < micros
           && !m_worstSeedMicros.compare_exchange_weak(worst, micros,
                                                       std::memory_order_relaxed)) {
    }
}

bool TimeStretchProcessor::requestKeylockSeed(int slot) noexcept
{
    if (slot < 0 || slot >= static_cast<int>(m_pipelines.size())) return false;
    if (!m_accepting.load(std::memory_order_acquire)) return false;
    auto& p = m_pipelines[slot];
    if (!p.signalsmith) return false;
    const int length = std::min({ p.signalsmith->outputSeekLength(1.0f),
                                  m_seedSnapshot.getNumSamples(),
                                  m_outputHistory.getNumSamples() });
    if (length <= 0) return false;
    // Hand the worker a private copy so the audio thread stays free to keep
    // writing history while the seed runs.
    readOutputHistory(m_seedSnapshot, length);
    m_seedSnapshotLength = length;
    m_seedSlot.store(slot, std::memory_order_relaxed);
    m_seedState.store(SeedState::Requested, std::memory_order_release);
    wakeWorker();
    return true;
}

bool TimeStretchProcessor::serviceSeedRequest() noexcept
{
    auto expected = SeedState::Requested;
    if (!m_seedState.compare_exchange_strong(expected, SeedState::Seeding,
                                             std::memory_order_acq_rel))
        return false;
    const int slot = m_seedSlot.load(std::memory_order_relaxed);
    if (slot >= 0 && slot < static_cast<int>(m_pipelines.size()) && m_seedSnapshotLength > 0) {
        auto& p = m_pipelines[slot];
        if (p.signalsmith) {
            const auto started = std::chrono::steady_clock::now();
            if (p.fifo) p.fifo->reset();
            // Seed with the pitch the pipeline will actually render at, not
            // whatever was live when it was last built.
            syncPipelinePitchScale(p);
            p.signalsmith->outputSeek(m_seedSnapshot.getArrayOfReadPointers(),
                                      m_seedSnapshotLength);
            recordSeedDuration(started);
        }
    }
    m_seedState.store(SeedState::Ready, std::memory_order_release);
    return true;
}

void TimeStretchProcessor::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept
{
    struct Scope { Scope(){g_inTimeStretchAudioCallback=true;} ~Scope(){g_inTimeStretchAudioCallback=false;} } scope;
    if (!source || !info.buffer || info.numSamples <= 0) { info.clearActiveBufferRegion(); return; }
    activatePreparedPipelineAtBlockBoundary();

    const int active = m_activeSlot.load(std::memory_order_acquire);
    // A manual semitone offset (Key Shift) needs the stretcher active even
    // when the keylock toggle itself is off, since it is the only path that
    // can move pitch independently of tempo.
    const bool keySemitoneActive = std::abs(m_keySemitoneOffset.load(std::memory_order_acquire)) > 1.0e-9;
    const bool keylockRequested = active >= 0
        && (m_pitchLockEnabled.load(std::memory_order_acquire) || keySemitoneActive)
        && m_inputPlaybackActive.load(std::memory_order_acquire)
        && !m_scratchBypass.load(std::memory_order_acquire);

    const auto renderDirect = [&] {
        m_reportedLatencySamples.store(0, std::memory_order_relaxed);
        source->getNextAudioBlock(info);
        applySwitchFade(info);
        appendOutputHistory(info);
    };

    if (!keylockRequested) {
        if (m_keylockRenderActive) {
            m_keylockRenderActive = false;
            beginTransitionFade();
        }
        m_seedBridgeSamples = 0;
        // A seed that finished after keylock was switched off describes audio
        // that is no longer the tail of the output; using it later would splice
        // in stale material.
        auto stale = SeedState::Ready;
        m_seedState.compare_exchange_strong(stale, SeedState::Idle, std::memory_order_acq_rel);
        renderDirect();
        return;
    }

    auto& pipeline = m_pipelines[active];
    if (m_keylockSeedPending.exchange(false, std::memory_order_acq_rel))
        m_keylockRenderActive = false;

    if (!m_keylockRenderActive) {
        // Seeding costs several FFT frames. On a roomy buffer that fits inside
        // the callback and switching in place is seamless; on a small one it
        // overruns, so bridge on the direct path until the worker has seeded.
        // Bridging is continuous audio at an unlocked pitch, which is far less
        // audible than a dropout, but it is still wrong, so keep it short.
        const double rate = m_sampleRate.load(std::memory_order_relaxed);
        const double blockMicros = rate > 0.0 ? 1.0e6 * info.numSamples / rate : 0.0;
        const auto measuredSeedMicros = m_worstSeedMicros.load(std::memory_order_relaxed);
        // Until a seed has ever been measured, assume it does not fit.
        const bool inlineAffordable = info.numSamples >= kMinimumInlineSeedBlockSamples
            && measuredSeedMicros > 0
            && static_cast<double>(measuredSeedMicros) * kSeedBudgetHeadroom <= blockMicros;
        const int bridgeLimit = static_cast<int>(rate * kMaximumSeedBridgeSeconds);
        const bool bridgeAffordable = !inlineAffordable
            && m_seedBridgeSamples + info.numSamples <= bridgeLimit;
        bool seeded = false;
        bool bridge = false;
        switch (m_seedState.load(std::memory_order_acquire)) {
        case SeedState::Idle:
            // requestKeylockSeed fails when there is nothing to seek (Rubber
            // Band) or the worker is gone; either way the audio thread still
            // owns the pipeline and takes the transition inline.
            bridge = requestKeylockSeed(active);
            break;
        case SeedState::Ready:
            // A seed for a slot that has since been swapped out is useless.
            seeded = m_seedSlot.load(std::memory_order_relaxed) == active;
            m_seedState.store(SeedState::Idle, std::memory_order_release);
            if (!seeded)
                bridge = requestKeylockSeed(active);
            break;
        case SeedState::Requested: {
            // Not picked up yet, so ownership can still be taken back once
            // bridging has gone on longer than one late callback is worth.
            auto expected = SeedState::Requested;
            bridge = bridgeAffordable
                  || !m_seedState.compare_exchange_strong(expected, SeedState::Idle,
                                                          std::memory_order_acq_rel);
            break;
        }
        case SeedState::Seeding:
            // The worker owns the stretcher; touching it here would race.
            bridge = true;
            break;
        }

        if (bridge) {
            m_seedBridgeSamples += info.numSamples;
            m_seedBridgeBlocks.fetch_add(1, std::memory_order_relaxed);
            renderDirect();
            return;
        }

        if (!seeded && !inlineAffordable) {
            // Never run a Signalsmith seed in small realtime buffers: occasional
            // scheduler jitter can still turn an "average-safe" micro-burst into
            // an audible crackle. Keep bridging and wake the worker again.
            m_seedBridgeSamples += info.numSamples;
            m_seedBridgeBlocks.fetch_add(1, std::memory_order_relaxed);
            wakeWorker();
            renderDirect();
            return;
        }

        m_seedBridgeSamples = 0;
        // Capture the crossfade tail before the block is overwritten.
        beginTransitionFade();
        if (seeded) {
            if (pipeline.fifo) pipeline.fifo->reset();
        } else {
            m_inlineSeeds.fetch_add(1, std::memory_order_relaxed);
            prepareKeylockTransition(pipeline);
        }
        m_keylockRenderActive = true;
    }
    m_reportedLatencySamples.store(pipeline.latency, std::memory_order_relaxed);
    processPipeline(pipeline, info);
    applySwitchFade(info);
    appendOutputHistory(info);
}

void TimeStretchProcessor::syncPipelinePitchScale(Pipeline& p) noexcept
{
    const double effectiveRate = m_targetTempoRatio.load(std::memory_order_acquire);
    const double semitones = m_keySemitoneOffset.load(std::memory_order_acquire);
    const double semitoneRatio = std::pow(2.0, semitones / 12.0);
    // The keylock correction (1/rate) only applies while the toggle is on; a
    // Key Shift offset stacks on top of whatever pitch is otherwise playing.
    const bool pitchLocked = m_pitchLockEnabled.load(std::memory_order_acquire);
    const double pitchScale = (pitchLocked ? 1.0 / effectiveRate : 1.0) * semitoneRatio;
    if (std::abs(p.appliedPitchScale - pitchScale) <= 1.0e-7)
        return;
    if (p.rubberBand)
        p.rubberBand->setPitchScale(pitchScale);
    else if (p.signalsmith)
        // The limit has to be repeated on every update: passing a factor on
        // its own resets the pitch map back to transposing the full band.
        p.signalsmith->setTransposeFactor(static_cast<float>(pitchScale),
                                          static_cast<float>(p.tonalityLimit));
    p.appliedPitchScale = pitchScale;
}

void TimeStretchProcessor::processPipeline(Pipeline& p, const juce::AudioSourceChannelInfo& info) noexcept
{
    syncPipelinePitchScale(p);
    if (p.signalsmith) {
        processSignalsmithPipeline(p, info);
        return;
    }

    const int needed = info.numSamples;
    int loops = kPullLoopLimit;
    while (p.fifo->getNumReady() < needed && loops-- > 0) {
        const int required = p.rubberBand->getSamplesRequired();
        const int pull = std::clamp(required > 0 ? required : kMinPullSize, kMinPullSize,
                                    std::min(kMaxPullSize, p.input.getNumSamples()));
        p.input.clear(0, pull);
        source->getNextAudioBlock({&p.input, 0, pull});
        const float* in[2] { p.input.getReadPointer(0), p.input.getReadPointer(1) };
        p.rubberBand->process(in, pull, false);
        const int available = std::min(p.rubberBand->available(), p.fifo->getFreeSpace());
        int s1, n1, s2, n2;
        p.fifo->prepareToWrite(std::max(0, available), s1, n1, s2, n2);
        if (n1 > 0) { float* out[2] {p.output.getWritePointer(0,s1),p.output.getWritePointer(1,s1)}; p.rubberBand->retrieve(out,n1); }
        if (n2 > 0) { float* out[2] {p.output.getWritePointer(0,s2),p.output.getWritePointer(1,s2)}; p.rubberBand->retrieve(out,n2); }
        p.fifo->finishedWrite(n1+n2);
    }
    const int count = std::min(needed, p.fifo->getNumReady());
    int s1,n1,s2,n2; p.fifo->prepareToRead(count,s1,n1,s2,n2);
    const int channels = std::min(2, info.buffer->getNumChannels());
    for(int ch=0;ch<channels;++ch){
        if(n1>0) info.buffer->copyFrom(ch,info.startSample,p.output,ch,s1,n1);
        if(n2>0) info.buffer->copyFrom(ch,info.startSample+n1,p.output,ch,s2,n2);
        if(count<needed) info.buffer->clear(ch,info.startSample+count,needed-count);
    }
    p.fifo->finishedRead(count);
}

void TimeStretchProcessor::processSignalsmithPipeline(
    Pipeline& p, const juce::AudioSourceChannelInfo& info) noexcept
{
    auto* stretch = p.signalsmith.get();
    const int channels = std::min(2, info.buffer->getNumChannels());
    int remaining = info.numSamples;
    int destination = info.startSample;
    int loops = kPullLoopLimit;

    while (remaining > 0 && loops-- > 0) {
        const int pull = std::min({remaining, kMaxPullSize,
                                   p.input.getNumSamples(), p.trim.getNumSamples()});
        if (pull <= 0)
            break;

        p.input.clear(0, pull);
        source->getNextAudioBlock({&p.input, 0, pull});
        const float* input[2] {
            p.input.getReadPointer(0),
            p.input.getReadPointer(1)
        };

        if (channels == 2) {
            float* output[2] {
                info.buffer->getWritePointer(0, destination),
                info.buffer->getWritePointer(1, destination)
            };
            stretch->process(input, pull, output, pull);
        } else {
            stretch->process(input, pull, p.trim.getArrayOfWritePointers(), pull);
            if (channels == 1)
                info.buffer->copyFrom(0, destination, p.trim, 0, 0, pull);
        }

        destination += pull;
        remaining -= pull;
    }

    if (remaining > 0) {
        for (int channel = 0; channel < channels; ++channel)
            info.buffer->clear(channel, destination, remaining);
    }
}

void TimeStretchProcessor::applySwitchFade(const juce::AudioSourceChannelInfo& info) noexcept
{
    int remaining = m_switchFadeRemaining.load(std::memory_order_relaxed);
    const int count = std::min(remaining, info.numSamples);
    for (int i=0;i<count;++i) {
        const int fadeIndex = kSwitchFadeSamples-remaining+i;
        const float gain = static_cast<float>(fadeIndex+1)/kSwitchFadeSamples;
        for(int ch=0;ch<info.buffer->getNumChannels();++ch) {
            const float oldSample = ch < m_previousTail.getNumChannels() ? m_previousTail.getSample(ch,fadeIndex) : 0.0f;
            const float newSample = info.buffer->getSample(ch,info.startSample+i);
            info.buffer->setSample(ch,info.startSample+i,oldSample*(1.0f-gain)+newSample*gain);
        }
    }
    m_switchFadeRemaining.store(remaining-count,std::memory_order_relaxed);
}

void TimeStretchProcessor::appendOutputHistory(const juce::AudioSourceChannelInfo& info) noexcept
{
    const int capacity = m_outputHistory.getNumSamples();
    if (capacity <= 0) return;
    const int count = std::min(info.numSamples, capacity);
    const int sourceStart = info.startSample + info.numSamples - count;
    const int first = std::min(count, capacity - m_historyWrite);
    const int second = count - first;
    for (int ch = 0; ch < 2; ++ch) {
        const int sourceChannel = std::min(ch, info.buffer->getNumChannels() - 1);
        if (sourceChannel < 0) break;
        if (first > 0)
            m_outputHistory.copyFrom(ch, m_historyWrite, *info.buffer, sourceChannel, sourceStart, first);
        if (second > 0)
            m_outputHistory.copyFrom(ch, 0, *info.buffer, sourceChannel, sourceStart + first, second);
    }
    m_historyWrite = (m_historyWrite + count) % capacity;
}

void TimeStretchProcessor::readOutputHistory(juce::AudioBuffer<float>& destination, int count) const noexcept
{
    const int capacity = m_outputHistory.getNumSamples();
    if (capacity <= 0 || count <= 0 || destination.getNumSamples() < count) return;
    // Unwrap the newest `count` samples so they end up chronological, oldest
    // first, which is the order both the crossfade and the stretcher expect.
    const int start = ((m_historyWrite - count) % capacity + capacity) % capacity;
    const int first = std::min(count, capacity - start);
    const int second = count - first;
    for (int ch = 0; ch < std::min(2, destination.getNumChannels()); ++ch) {
        destination.copyFrom(ch, 0, m_outputHistory, ch, start, first);
        if (second > 0)
            destination.copyFrom(ch, first, m_outputHistory, ch, 0, second);
    }
}

void TimeStretchProcessor::seedPipelineFromHistory(Pipeline& p) noexcept
{
    if (!p.signalsmith || !p.config.keylockEnabled) return;
    // Seed with the pitch this pipeline will actually render at. Without this,
    // an inline seed taken with a tempo offset dialed in bakes pre-roll phase
    // state for the wrong pitch, and the transpose update a few lines later in
    // processPipeline() invalidates it immediately — audible as a digital
    // artifact right at the resume point, not a plain amplitude click.
    syncPipelinePitchScale(p);
    // A stretcher that has just been built outputs its own latency worth of
    // silence before the first real sample arrives — at this window that is
    // over 30 ms of dropout every time keylock is switched on, which is exactly
    // the click. Handing it the audio the listener just heard as pre-roll lets
    // it carry that on seamlessly while the new input works its way through.
    const int seedLength = std::min({ p.signalsmith->outputSeekLength(1.0f),
                                      m_historyScratch.getNumSamples(),
                                      m_outputHistory.getNumSamples() });
    if (seedLength <= 0) return;
    readOutputHistory(m_historyScratch, seedLength);
    p.signalsmith->outputSeek(m_historyScratch.getArrayOfReadPointers(), seedLength);
    p.fifo->reset();
}

int TimeStretchProcessor::getLatencySamples() const noexcept { return m_reportedLatencySamples.load(std::memory_order_relaxed); }
TimeStretchBackend TimeStretchProcessor::activeBackend() const noexcept
{
    return m_activeBackend.load(std::memory_order_acquire);
}
std::uint64_t TimeStretchProcessor::activeConfigurationGeneration() const noexcept { return m_activeGeneration.load(std::memory_order_acquire); }
TimeStretchRealtimeStats TimeStretchProcessor::realtimeStats() const noexcept
{
    return {m_prepareFromAudio.load(),m_resetFromAudio.load(),m_prewarmFromAudio.load(),m_growthFromAudio.load(),m_lockFromAudio.load(),m_switches.load(),m_stale.load(),m_failures.load(),m_keylockSeeds.load(),m_worstSeedMicros.load(),m_seedBridgeBlocks.load(),m_inlineSeeds.load()};
}
