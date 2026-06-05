#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <cstdint>
#include <vector>
#include "../fx/BrickwallLimiter.h"

class DjEngine;

// ─────────────────────────────────────────────────────────────────────────────
// DjMasterBus  —  single pull-model AudioSource that sums all deck outputs,
// applies master gain + brickwall limiter on the summed signal, then routes
// the result to master, booth, and headphone output channel pairs.
//
// Only one DjMasterBus should exist per application.  Register its source
// player as the sole ADM audio callback via registerCallback().
//
// Signal flow (per block):
//   for each deck:
//       deck.source.getNextAudioBlock()        ← posts PFL into deck.pflBuffer
//   masterBuf = sum of all deck outputs (ch 0+1)
//   masterBuf *= s_masterVolume
//   masterBuf → BrickwallLimiter               ← correct position: summed signal
//   output routing:
//       masterFirstChannel  ← masterBuf
//       headphonesFirstChannel ← Σ cued-deck PFL × cueGain  [+ master × masterCueGain]
//       boothFirstChannel      ← Σ all-deck PFL
// ─────────────────────────────────────────────────────────────────────────────
class DjMasterBus : public juce::AudioSource
{
public:
    DjMasterBus();
    ~DjMasterBus() override;

    void addDeck(DjEngine* deck);
    void removeDeck(DjEngine* deck);

    // juce::AudioSource
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    // Register / unregister with the shared AudioDeviceManager.
    void registerCallback(juce::AudioDeviceManager& adm);
    void unregisterCallback(juce::AudioDeviceManager& adm);

    // ── Master volume + clip protection ──────────────────────────────────────
    static void  setMasterVolume(float v);
    static void  setAntiClipEnabled(bool enabled);
    static bool  antiClipEnabled();
    static float gainReduction();       // last block's gain-reduction factor
    static int   limiterLatencySamples();
    static double callbackAverageUsec();
    static double callbackWorstUsec();
    static uint64_t callbackCount();
    static uint64_t callbackOverrunCount();
    static void resetCallbackStats();

    // ── Output routing config ─────────────────────────────────────────────────
    // firstChannel is 1-based; -1 = disabled.
    static void setOutputRouting(int masterFirstCh, int boothFirstCh, int headphonesFirstCh);
    static int  masterFirstChannel();
    static int  boothFirstChannel();
    static int  headphonesFirstChannel();

    // ── Headphone monitoring ──────────────────────────────────────────────────
    static void  setMasterCueEnabled(bool enabled);
    static bool  masterCueEnabled();
    static void  setHeadphoneMix(float mix);    // 0 = full cue, 1 = full master
    static float headphoneMix();

    // ── Master VU (post-limiter, post-sum) ───────────────────────────────────
    float masterVuL()          const { return m_masterPeakL.load(std::memory_order_relaxed); }
    float masterVuR()          const { return m_masterPeakR.load(std::memory_order_relaxed); }
    bool  masterClipDetected() const { return s_masterClipDetected.load(std::memory_order_relaxed); }

    // Static accessor so DjEngine can delegate clipDetected() without holding a pointer.
    static bool masterClipDetected_s();

private:
    std::vector<DjEngine*>         m_decks;
    juce::AudioBuffer<float>       m_deckScratch;   // per-deck pull buffer (reused each call)
    juce::AudioBuffer<float>       m_masterBuf;     // summed 2-ch master signal
    BrickwallLimiter               m_limiter;
    juce::AudioSourcePlayer        m_sourcePlayer;
    double                         m_sampleRate    = 44100.0;
    int                            m_maxBlockSize  = 512;
    int                            m_bufferCapacity = 512;
    bool                           m_isPrepared    = false;

    std::atomic<float> m_masterPeakL       { 0.0f };
    std::atomic<float> m_masterPeakR       { 0.0f };
    // ── Shared statics ────────────────────────────────────────────────────────
    static std::atomic<float> s_masterVolume;
    static std::atomic<bool>  s_antiClipEnabled;
    static std::atomic<float> s_gainReduction;
    static std::atomic<int>   s_limiterLatencySamples;
    static std::atomic<uint64_t> s_callbackCount;
    static std::atomic<uint64_t> s_callbackTotalUsec;
    static std::atomic<uint64_t> s_callbackWorstUsec;
    static std::atomic<uint64_t> s_callbackOverruns;

    static std::atomic<int>   s_masterFirstChannel;
    static std::atomic<int>   s_boothFirstChannel;
    static std::atomic<int>   s_headphonesFirstChannel;

    static std::atomic<bool>  s_masterCueEnabled;
    static std::atomic<float> s_headphoneMix;
    static std::atomic<bool>  s_masterClipDetected;

    // ── Audio-thread helpers ──────────────────────────────────────────────────
    static void routeStereoToPair(juce::AudioBuffer<float>& buffer,
                                  const float* srcL, const float* srcR,
                                  int start, int n,
                                  int firstChannel,
                                  bool add   = false,
                                  float gain = 1.0f);
};
