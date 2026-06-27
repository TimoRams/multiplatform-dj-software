#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>

namespace engine::audio {

// RT-safe Hermite scratch reader with true bidirectional playback.
// Window sizing follows the device buffer size from audio settings.
class ScratchResampler {
public:
    void prepare(int numChannels, int deviceBufferSize, double outputSampleRate);
    void reset(double readPositionSamples) noexcept;
    void setReadPositionSamples(double readPositionSamples) noexcept;
    void nudgeReadPositionSamples(double deltaSamples) noexcept;
    void snapSmoothedRate(double rate) noexcept;
    void primeTrackerVelocity(double ratePerOutputSample) noexcept;
    void invalidatePrefetch() noexcept { m_sourceSize = 0; }

    void setFormatReader(juce::AudioFormatReader* reader) noexcept { m_reader = reader; }

    void setTrackLengthSamples(double lengthSamples) noexcept {
        m_trackLengthSamples = std::max(0.0, lengthSamples);
    }

    void setLoopRange(double loopInSample, double loopOutSample, bool active) noexcept
    {
        m_loopInSample = loopInSample;
        m_loopOutSample = loopOutSample;
        m_loopActive = active;
    }

    // rate: track samples advanced per output sample (negative = reverse)
    void processBlock(juce::AudioSource& input,
                      double rate,
                      const juce::AudioSourceChannelInfo& output) noexcept;

    // Position-authoritative scratch step. A critically-damped tracker glides the
    // read head toward the absolute hand target (track samples). Slow/precise moves
    // track exactly with no overshoot; momentum carries playback smoothly across
    // sparse UI events. Returns the rate used (track samples per output sample).
    double processScratchTracking(juce::AudioSource& input,
                                  double targetPosSamples,
                                  double maxAbsRate,
                                  const juce::AudioSourceChannelInfo& output) noexcept;

    [[nodiscard]] double readPosition() const noexcept { return m_readPos; }
    [[nodiscard]] int deviceBufferSize() const noexcept { return m_deviceBufferSize; }

private:
    void windowMargins(double rate, int outputBlockSize, int& lookBehind, int& lookAhead) const noexcept;
    [[nodiscard]] bool needsWindowReload(double minAbsPos, double maxAbsPos) const noexcept;
    void ensureWindow(juce::AudioSource& input, double rate, int outputBlockSize) noexcept;
    bool reloadWindowFromDisk(double rate, int outputBlockSize) noexcept;
    void reloadWindowFromStream(juce::AudioSource& input, double rate, int outputBlockSize) noexcept;
    float readHermite(int channel, double position) const noexcept;
    double wrapPosition(double pos) const noexcept;

    juce::AudioFormatReader* m_reader = nullptr;
    juce::AudioBuffer<float> m_sourceBuffer;
    int m_channels = 2;
    int m_deviceBufferSize = 512;
    int m_blockSize = 512;
    double m_outputSampleRate = 44100.0;
    double m_trackLengthSamples = 0.0;

    int m_sourceSize = 0;
    double m_readPos = 0.0;
    double m_bufferOriginSample = 0.0;
    double m_lastRate = 0.0;
    double m_smoothedRate = 0.0;
    double m_trackVel = 0.0;   // tracker velocity, track samples / second

    bool m_loopActive = false;
    double m_loopInSample = 0.0;
    double m_loopOutSample = 0.0;

    static constexpr int kHermiteRadius = 2;
    static constexpr int kMinWindowSamples = 12;
};

} // namespace engine::audio
