#include "DjMasterBus.h"
#include "DjEngine.h"
#include <algorithm>
#include <cmath>

// ── Static storage ────────────────────────────────────────────────────────────
std::atomic<float> DjMasterBus::s_masterVolume       { 1.0f  };
std::atomic<bool>  DjMasterBus::s_antiClipEnabled    { false };
std::atomic<float> DjMasterBus::s_gainReduction      { 1.0f  };
// Instance-level clip state exposed via a static pointer to the singleton.
static DjMasterBus* s_instance = nullptr;  // set in ctor, cleared in dtor
std::atomic<int>   DjMasterBus::s_masterFirstChannel  { 1     };
std::atomic<int>   DjMasterBus::s_boothFirstChannel   { -1    };
std::atomic<int>   DjMasterBus::s_headphonesFirstChannel { -1 };
std::atomic<bool>  DjMasterBus::s_masterCueEnabled   { false };
std::atomic<float> DjMasterBus::s_headphoneMix       { 0.5f  };

// ── Construction ──────────────────────────────────────────────────────────────

DjMasterBus::DjMasterBus()  { s_instance = this; }
DjMasterBus::~DjMasterBus() { if (s_instance == this) s_instance = nullptr; }

// ── Deck management ───────────────────────────────────────────────────────────

void DjMasterBus::addDeck(DjEngine* deck)
{
    if (deck && std::find(m_decks.begin(), m_decks.end(), deck) == m_decks.end())
    {
        m_decks.push_back(deck);
        // Only prepare the deck if the bus has been prepared by the ADM.
        // Avoids calling prepareToPlay with stale defaults before the audio
        // device opens with its actual block size and sample rate.
        if (m_isPrepared)
            if (auto* src = deck->getAudioSource())
                src->prepareToPlay(m_maxBlockSize, m_sampleRate);
    }
}

void DjMasterBus::removeDeck(DjEngine* deck)
{
    m_decks.erase(std::remove(m_decks.begin(), m_decks.end(), deck), m_decks.end());
}

// ── ADM integration ───────────────────────────────────────────────────────────

void DjMasterBus::registerCallback(juce::AudioDeviceManager& adm)
{
    m_sourcePlayer.setSource(this);
    adm.addAudioCallback(&m_sourcePlayer);
}

void DjMasterBus::unregisterCallback(juce::AudioDeviceManager& adm)
{
    adm.removeAudioCallback(&m_sourcePlayer);
    m_sourcePlayer.setSource(nullptr);
}

// ── AudioSource ───────────────────────────────────────────────────────────────

void DjMasterBus::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    m_sampleRate   = sampleRate;
    m_maxBlockSize = samplesPerBlockExpected;
    m_isPrepared   = true;

    // Prepare summing buffers
    m_deckScratch.setSize(2, samplesPerBlockExpected, false, true, true);
    m_masterBuf  .setSize(2, samplesPerBlockExpected, false, true, true);

    // Prepare limiter
    m_limiter.prepare(sampleRate, samplesPerBlockExpected, 2);

    // Prepare all registered deck sources
    for (auto* deck : m_decks)
        if (auto* src = deck->getAudioSource())
            src->prepareToPlay(samplesPerBlockExpected, sampleRate);
}

void DjMasterBus::releaseResources()
{
    m_isPrepared = false;
    for (auto* deck : m_decks)
        if (auto* src = deck->getAudioSource())
            src->releaseResources();
}

void DjMasterBus::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    const int s = bufferToFill.startSample;
    const int n = bufferToFill.numSamples;
    auto* outBuf = bufferToFill.buffer;

    if (n == 0 || !outBuf)
        return;

    // Zero all output channels — we write only to routed pairs below.
    for (int ch = 0; ch < outBuf->getNumChannels(); ++ch)
        outBuf->clear(ch, s, n);

    // Ensure internal buffers are large enough.
    if (m_deckScratch.getNumSamples() < n)
        m_deckScratch.setSize(2, n, false, true, true);
    if (m_masterBuf.getNumSamples() < n)
        m_masterBuf.setSize(2, n, false, true, true);

    // ── 1. Pull from each deck and accumulate into master buffer ─────────────
    m_masterBuf.clear(0, 0, n);
    m_masterBuf.clear(1, 0, n);

    for (auto* deck : m_decks)
    {
        auto* src = deck->getAudioSource();
        if (!src) continue;

        m_deckScratch.clear(0, 0, n);
        m_deckScratch.clear(1, 0, n);

        juce::AudioSourceChannelInfo deckInfo(&m_deckScratch, 0, n);
        src->getNextAudioBlock(deckInfo);
        // m_deckScratch[0+1] now contains deck's post-Beat-FX, post-fader stereo signal.
        // deck->getPflBuffer() now contains deck's fresh pre-fader PFL capture.

        m_masterBuf.addFrom(0, 0, m_deckScratch, 0, 0, n);
        m_masterBuf.addFrom(1, 0, m_deckScratch, 1, 0, n);
    }

    // ── 2. Apply master volume ────────────────────────────────────────────────
    const float masterGain = s_masterVolume.load(std::memory_order_relaxed);
    if (std::abs(masterGain - 1.0f) > 0.001f)
    {
        m_masterBuf.applyGain(0, 0, n, masterGain);
        m_masterBuf.applyGain(1, 0, n, masterGain);
    }

    // ── 3. Master VU measurement (true peaks, before limiter) ─────────────────
    {
        constexpr float kClipThreshold = 1.001f;
        const float peakL = m_masterBuf.getMagnitude(0, 0, n);
        const float peakR = m_masterBuf.getMagnitude(1, 0, n);
        m_masterPeakL.store(peakL, std::memory_order_relaxed);
        m_masterPeakR.store(peakR, std::memory_order_relaxed);
        m_masterClipDetected.store((peakL > kClipThreshold) || (peakR > kClipThreshold),
                                   std::memory_order_relaxed);
    }

    // ── 4. Brickwall limiter on the summed signal ─────────────────────────────
    {
        m_limiter.setEnabled(s_antiClipEnabled.load(std::memory_order_relaxed));
        float* ptrs[2] = { m_masterBuf.getWritePointer(0),
                           m_masterBuf.getWritePointer(1) };
        const float gr = m_limiter.processBlock(ptrs, 2, 0, n);
        s_gainReduction.store(m_limiter.isEnabled() ? gr : 1.0f,
                              std::memory_order_relaxed);
    }

    // ── 5. Output routing ──────────────────────────────────────────────────────
    const float* masterL = m_masterBuf.getReadPointer(0);
    const float* masterR = m_masterBuf.getReadPointer(1);

    // Master output
    const int masterCh = s_masterFirstChannel.load(std::memory_order_relaxed);
    if (masterCh >= 1)
        routeStereoToPair(*outBuf, masterL, masterR, s, n, masterCh, false);

    // Headphone output: cued decks' PFL + optional master cue
    const int hpCh = s_headphonesFirstChannel.load(std::memory_order_relaxed);
    if (hpCh >= 1)
    {
        const float cueMix     = std::clamp(s_headphoneMix.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float cueGain    = 1.0f - cueMix;
        const bool  masterCue  = s_masterCueEnabled.load(std::memory_order_relaxed);
        const float masterCueGain = masterCue ? cueMix : 0.0f;

        bool phonesHaveSignal = false;
        for (auto* deck : m_decks)
        {
            if (!deck->cueEnabled() || cueGain < 0.0001f) continue;
            const auto& pflBuf = deck->getPflBuffer();
            if (pflBuf.getNumSamples() < n) continue;
            const float* pflL = pflBuf.getReadPointer(0);
            const float* pflR = pflBuf.getNumChannels() > 1
                                    ? pflBuf.getReadPointer(1) : pflL;
            routeStereoToPair(*outBuf, pflL, pflR, s, n, hpCh, phonesHaveSignal, cueGain);
            phonesHaveSignal = true;
        }
        if (masterCueGain > 0.0001f)
            routeStereoToPair(*outBuf, masterL, masterR, s, n, hpCh,
                              phonesHaveSignal, masterCueGain);
    }

    // Booth output: sum of all deck pre-fader signals
    const int boothCh = s_boothFirstChannel.load(std::memory_order_relaxed);
    if (boothCh >= 1)
    {
        bool first = true;
        for (auto* deck : m_decks)
        {
            const auto& pflBuf = deck->getPflBuffer();
            if (pflBuf.getNumSamples() < n) continue;
            const float* pflL = pflBuf.getReadPointer(0);
            const float* pflR = pflBuf.getNumChannels() > 1
                                    ? pflBuf.getReadPointer(1) : pflL;
            routeStereoToPair(*outBuf, pflL, pflR, s, n, boothCh, !first);
            first = false;
        }
    }
}

// ── Static setters / getters ──────────────────────────────────────────────────

void  DjMasterBus::setMasterVolume(float v)
{
    s_masterVolume.store(std::clamp(v, 0.0f, 1.5f), std::memory_order_relaxed);
}

void  DjMasterBus::setAntiClipEnabled(bool enabled)
{
    s_antiClipEnabled.store(enabled, std::memory_order_relaxed);
}

float DjMasterBus::gainReduction()
{
    return s_gainReduction.load(std::memory_order_relaxed);
}

void DjMasterBus::setOutputRouting(int masterCh, int boothCh, int hpCh)
{
    s_masterFirstChannel    .store(masterCh, std::memory_order_relaxed);
    s_boothFirstChannel     .store(boothCh,  std::memory_order_relaxed);
    s_headphonesFirstChannel.store(hpCh,     std::memory_order_relaxed);
}

int DjMasterBus::masterFirstChannel()      { return s_masterFirstChannel    .load(std::memory_order_relaxed); }
int DjMasterBus::boothFirstChannel()       { return s_boothFirstChannel     .load(std::memory_order_relaxed); }
int DjMasterBus::headphonesFirstChannel()  { return s_headphonesFirstChannel.load(std::memory_order_relaxed); }

void  DjMasterBus::setMasterCueEnabled(bool enabled)
{
    s_masterCueEnabled.store(enabled, std::memory_order_relaxed);
}

bool  DjMasterBus::masterCueEnabled()
{
    return s_masterCueEnabled.load(std::memory_order_relaxed);
}

void  DjMasterBus::setHeadphoneMix(float mix)
{
    s_headphoneMix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

float DjMasterBus::headphoneMix()
{
    return s_headphoneMix.load(std::memory_order_relaxed);
}

bool DjMasterBus::masterClipDetected_s()
{
    return s_instance ? s_instance->m_masterClipDetected.load(std::memory_order_relaxed) : false;
}

// ── Audio-thread routing helper ───────────────────────────────────────────────

void DjMasterBus::routeStereoToPair(juce::AudioBuffer<float>& buffer,
                                    const float* srcL, const float* srcR,
                                    int start, int n,
                                    int firstChannel, bool add, float gain)
{
    if (!srcL || !srcR || n <= 0 || std::abs(gain) <= 0.0001f || firstChannel < 1)
        return;

    const int leftCh  = firstChannel - 1;
    const int rightCh = leftCh + 1;
    if (rightCh >= buffer.getNumChannels())
        return;
    if (buffer.getNumSamples() < start + n)
        return;

    float* dstL = buffer.getWritePointer(leftCh,  start);
    float* dstR = buffer.getWritePointer(rightCh, start);
    if (!dstL || !dstR) return;

    for (int i = 0; i < n; ++i)
    {
        if (add) { dstL[i] += srcL[i] * gain; dstR[i] += srcR[i] * gain; }
        else     { dstL[i]  = srcL[i] * gain; dstR[i]  = srcR[i] * gain; }
    }
}
