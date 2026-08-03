#include "TimeStretchAudioSource.h"

#include <algorithm>
#include <cmath>

namespace { thread_local bool g_inTimeStretchAudioCallback = false; }

TimeStretchAudioSource::TimeStretchAudioSource(juce::AudioSource* inSource) : source(inSource) {}
TimeStretchAudioSource::~TimeStretchAudioSource() { releaseResources(); }

bool TimeStretchAudioSource::validConfiguration(const TimeStretchConfiguration& c) noexcept
{
    return std::isfinite(c.sampleRate) && c.sampleRate >= 8000.0 && c.sampleRate <= 384000.0
        && std::isfinite(c.tempoRatio) && c.tempoRatio >= 0.01 && c.tempoRatio <= 8.0
        && c.maximumBlockSize >= 64 && c.maximumBlockSize <= 8192
        && c.channelCount >= 1 && c.channelCount <= 2 && c.trackGeneration != 0;
}

void TimeStretchAudioSource::setTempoRatio(double ratio) noexcept
{
    const auto clamped = std::clamp(std::isfinite(ratio) ? ratio : 1.0, 0.01, 8.0);
    if (std::abs(m_targetTempoRatio.exchange(clamped) - clamped) >= 0.0005)
        publishDesiredConfiguration();
}

void TimeStretchAudioSource::setPitchLockEnabled(bool enabled) noexcept
{
    if (m_pitchLockEnabled.exchange(enabled) != enabled) publishDesiredConfiguration();
}

void TimeStretchAudioSource::setScratchBypass(bool enabled) noexcept
{
    if (m_scratchBypass.exchange(enabled) != enabled && !enabled) publishDesiredConfiguration();
}

void TimeStretchAudioSource::setTrackGeneration(std::uint64_t generation) noexcept
{
    generation = std::max<std::uint64_t>(1, generation);
    if (m_trackGeneration.exchange(generation, std::memory_order_acq_rel) != generation)
        publishDesiredConfiguration();
}

void TimeStretchAudioSource::enterScratchBypass() noexcept
{
    m_scratchBypass.store(true, std::memory_order_release);
    m_switchFadeRemaining.store(0, std::memory_order_relaxed);
    m_reportedLatencySamples.store(0, std::memory_order_relaxed);
}

void TimeStretchAudioSource::endScratchBypass() noexcept
{
    m_scratchBypass.store(false, std::memory_order_release);
    publishDesiredConfiguration();
}

TimeStretchConfiguration TimeStretchAudioSource::desiredConfiguration() const noexcept
{
    TimeStretchConfiguration c;
    c.sampleRate = m_sampleRate.load(std::memory_order_acquire);
    c.tempoRatio = m_targetTempoRatio.load(std::memory_order_acquire);
    c.maximumBlockSize = m_maximumBlockSize.load(std::memory_order_acquire);
    c.keylockEnabled = m_pitchLockEnabled.load(std::memory_order_acquire);
    c.trackGeneration = m_trackGeneration.load(std::memory_order_acquire);
    c.configurationGeneration = m_desiredGeneration.load(std::memory_order_acquire);
    return c;
}

void TimeStretchAudioSource::publishDesiredConfiguration() noexcept
{
    if (!m_accepting.load(std::memory_order_acquire)) return;
    m_desiredGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_workerWake.notify_one();
}

void TimeStretchAudioSource::prepareToPlay(int blockSize, double sr)
{
    stopWorker();
    if (source) source->prepareToPlay(blockSize, sr);
    m_sampleRate.store(sr, std::memory_order_release);
    m_maximumBlockSize.store(std::clamp(blockSize, 64, 8192), std::memory_order_release);
    m_trackGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_desiredGeneration.store(1, std::memory_order_release);
    m_activeGeneration.store(0, std::memory_order_release);
    m_activeSlot.store(-1, std::memory_order_release);
    m_accepting.store(true, std::memory_order_release);
    m_prepared.store(true, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);
    resizeBuffer(m_previousTail, 2, kSwitchFadeSamples);
    m_previousTail.clear();

    auto initial = desiredConfiguration();
    initial.configurationGeneration = 1;
    if (!preparePipeline(m_pipelines[0], initial)) {
        m_failures.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_pipelines[0].state.store(SlotState::Active, std::memory_order_release);
        m_activeSlot.store(0, std::memory_order_release);
        m_activeGeneration.store(1, std::memory_order_release);
    }
    m_pipelines[1].state.store(SlotState::Empty, std::memory_order_release);
    m_worker = std::thread([this] { workerLoop(); });
}

void TimeStretchAudioSource::stopWorker() noexcept
{
    m_accepting.store(false, std::memory_order_release);
    m_stopRequested.store(true, std::memory_order_release);
    m_workerWake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void TimeStretchAudioSource::releaseResources()
{
    if (!m_prepared.exchange(false, std::memory_order_acq_rel)) return;
    stopWorker();
    if (source) source->releaseResources();
    m_activeSlot.store(-1, std::memory_order_release);
    for (auto& p : m_pipelines) {
        p.state.store(SlotState::Empty, std::memory_order_release);
        p.stretcher.reset();
        p.fifo.reset();
        resizeBuffer(p.input, 0, 0);
        resizeBuffer(p.output, 0, 0);
        resizeBuffer(p.trim, 0, 0);
        resizeBuffer(p.zeros, 0, 0);
    }
    resizeBuffer(m_previousTail, 0, 0);
}

bool TimeStretchAudioSource::preparePipeline(Pipeline& p, const TimeStretchConfiguration& c)
{
    if (g_inTimeStretchAudioCallback) m_prepareFromAudio.fetch_add(1, std::memory_order_relaxed);
    if (!validConfiguration(c)) return false;
    p.stretcher = std::make_unique<RubberBand::RubberBandStretcher>(c.sampleRate, c.channelCount,
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionWindowShort |
        RubberBand::RubberBandStretcher::OptionPitchHighSpeed);
    p.stretcher->setMaxProcessSize(static_cast<size_t>(std::min(4096, std::max(512, c.maximumBlockSize))));
    p.stretcher->setTimeRatio(1.0);
    p.stretcher->setPitchScale(c.keylockEnabled ? 1.0 / c.tempoRatio : 1.0);
    resizeBuffer(p.input, 2, std::max(4096, c.maximumBlockSize));
    resizeBuffer(p.output, 2, kFifoCapacity);
    resizeBuffer(p.trim, 2, 4096);
    resizeBuffer(p.zeros, 2, 4096);
    p.zeros.clear();
    p.fifo = std::make_unique<juce::AbstractFifo>(kFifoCapacity);
    p.config = c;
    p.prefill = 0;
    prewarmPipeline(p);
    p.latency = static_cast<int>(p.stretcher->getLatency());
    return true;
}

void TimeStretchAudioSource::resizeBuffer(juce::AudioBuffer<float>& buffer, int channels, int samples)
{
    if (g_inTimeStretchAudioCallback) m_growthFromAudio.fetch_add(1, std::memory_order_relaxed);
    buffer.setSize(channels, samples);
}

void TimeStretchAudioSource::prewarmPipeline(Pipeline& p)
{
    if (g_inTimeStretchAudioCallback) m_prewarmFromAudio.fetch_add(1, std::memory_order_relaxed);
    if (!p.stretcher) return;
    int remaining = static_cast<int>(p.stretcher->getPreferredStartPad() + p.stretcher->getStartDelay());
    for (int guard = 256; remaining > 0 && guard-- > 0;) {
        const int chunk = std::min(remaining, p.zeros.getNumSamples());
        const float* in[2] { p.zeros.getReadPointer(0), p.zeros.getReadPointer(1) };
        p.stretcher->process(in, chunk, false);
        int available = p.stretcher->available();
        if (available > 0) {
            const int take = std::min({remaining, available, p.trim.getNumSamples()});
            float* out[2] { p.trim.getWritePointer(0), p.trim.getWritePointer(1) };
            p.stretcher->retrieve(out, take);
            remaining -= take;
        }
    }
}

void TimeStretchAudioSource::workerLoop()
{
    std::uint64_t observed = m_activeGeneration.load(std::memory_order_acquire);
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock(m_workerMutex);
            m_workerWake.wait(lock, [&] {
                return m_stopRequested.load(std::memory_order_acquire)
                    || m_desiredGeneration.load(std::memory_order_acquire) != observed;
            });
        }
        if (m_stopRequested.load(std::memory_order_acquire)) break;
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
                m_workerWake.notify_one();
            }
            continue;
        }
        if (!preparePipeline(p, config)) {
            p.state.store(SlotState::Empty, std::memory_order_release);
            m_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const auto latest = m_desiredGeneration.load(std::memory_order_acquire);
        const auto track = m_trackGeneration.load(std::memory_order_acquire);
        if (latest != config.configurationGeneration || track != config.trackGeneration) {
            p.state.store(SlotState::Empty, std::memory_order_release);
            m_stale.fetch_add(1, std::memory_order_relaxed);
            observed = m_activeGeneration.load(std::memory_order_acquire);
            m_workerWake.notify_one();
            continue;
        }
        p.state.store(SlotState::Ready, std::memory_order_release);
    }
}

void TimeStretchAudioSource::activatePreparedPipelineAtBlockBoundary() noexcept
{
    const auto wanted = m_desiredGeneration.load(std::memory_order_acquire);
    for (int i = 0; i < 2; ++i) {
        auto& next = m_pipelines[i];
        if (next.state.load(std::memory_order_acquire) != SlotState::Ready) continue;
        SlotState ready = SlotState::Ready;
        if (!next.state.compare_exchange_strong(ready, SlotState::Active, std::memory_order_acq_rel)) continue;
        if (next.config.configurationGeneration != wanted
            || next.config.trackGeneration != m_trackGeneration.load(std::memory_order_acquire)) {
            next.state.store(SlotState::Empty, std::memory_order_release);
            m_workerWake.notify_one();
            continue;
        }
        const int old = m_activeSlot.exchange(i, std::memory_order_acq_rel);
        m_activeGeneration.store(next.config.configurationGeneration, std::memory_order_release);
        m_reportedLatencySamples.store(next.config.keylockEnabled ? next.latency : 0, std::memory_order_relaxed);
        m_switchFadeRemaining.store(kSwitchFadeSamples, std::memory_order_relaxed);
        m_switches.fetch_add(1, std::memory_order_relaxed);
        if (old >= 0 && old != i) m_pipelines[old].state.store(SlotState::Empty, std::memory_order_release);
        m_workerWake.notify_one();
        break;
    }
}

void TimeStretchAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) noexcept
{
    struct Scope { Scope(){g_inTimeStretchAudioCallback=true;} ~Scope(){g_inTimeStretchAudioCallback=false;} } scope;
    if (!source || !info.buffer || info.numSamples <= 0) { info.clearActiveBufferRegion(); return; }
    activatePreparedPipelineAtBlockBoundary();
    const int active = m_activeSlot.load(std::memory_order_acquire);
    if (m_scratchBypass.load(std::memory_order_acquire) || active < 0
        || m_pipelines[active].config.trackGeneration != m_trackGeneration.load(std::memory_order_acquire)
        || !m_pipelines[active].config.keylockEnabled) {
        m_reportedLatencySamples.store(0, std::memory_order_relaxed);
        source->getNextAudioBlock(info);
        applySwitchFade(info);
        captureOutputTail(info);
        return;
    }
    processPipeline(m_pipelines[active], info);
    applySwitchFade(info);
    captureOutputTail(info);
}

void TimeStretchAudioSource::processPipeline(Pipeline& p, const juce::AudioSourceChannelInfo& info) noexcept
{
    const int needed = info.numSamples;
    int loops = kPullLoopLimit;
    while (p.fifo->getNumReady() < needed && loops-- > 0) {
        int pull = p.stretcher->getSamplesRequired();
        pull = std::clamp(pull > 0 ? pull : kMinPullSize, kMinPullSize,
                          std::min(kMaxPullSize, p.input.getNumSamples()));
        p.input.clear(0, pull);
        source->getNextAudioBlock({&p.input, 0, pull});
        const float* in[2] { p.input.getReadPointer(0), p.input.getReadPointer(1) };
        p.stretcher->process(in, pull, false);
        int available = std::min(p.stretcher->available(), p.fifo->getFreeSpace());
        int s1, n1, s2, n2;
        p.fifo->prepareToWrite(std::max(0, available), s1, n1, s2, n2);
        if (n1 > 0) { float* out[2] {p.output.getWritePointer(0,s1),p.output.getWritePointer(1,s1)}; p.stretcher->retrieve(out,n1); }
        if (n2 > 0) { float* out[2] {p.output.getWritePointer(0,s2),p.output.getWritePointer(1,s2)}; p.stretcher->retrieve(out,n2); }
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

void TimeStretchAudioSource::applySwitchFade(const juce::AudioSourceChannelInfo& info) noexcept
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

void TimeStretchAudioSource::captureOutputTail(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (m_previousTail.getNumSamples() != kSwitchFadeSamples) return;
    const int count = std::min(kSwitchFadeSamples, info.numSamples);
    const int sourceStart = info.startSample + info.numSamples - count;
    for (int ch=0; ch<std::min(2,info.buffer->getNumChannels()); ++ch) {
        if (count < kSwitchFadeSamples) m_previousTail.clear(ch,0,kSwitchFadeSamples-count);
        m_previousTail.copyFrom(ch,kSwitchFadeSamples-count,*info.buffer,ch,sourceStart,count);
    }
}

int TimeStretchAudioSource::getLatencySamples() const noexcept { return m_reportedLatencySamples.load(std::memory_order_relaxed); }
std::uint64_t TimeStretchAudioSource::activeConfigurationGeneration() const noexcept { return m_activeGeneration.load(std::memory_order_acquire); }
TimeStretchRealtimeStats TimeStretchAudioSource::realtimeStats() const noexcept
{
    return {m_prepareFromAudio.load(),m_resetFromAudio.load(),m_prewarmFromAudio.load(),m_growthFromAudio.load(),m_lockFromAudio.load(),m_switches.load(),m_stale.load(),m_failures.load()};
}
