#pragma once

#include "app/ControlClock.h"

#include <QObject>
#include <QString>

#include <cstdint>

class AudioDeviceService;

class RenderPressurePolicy final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString tier READ tier NOTIFY tierChanged)
    Q_PROPERTY(int waveformUpdateIntervalMs READ waveformUpdateIntervalMs
               NOTIFY updateIntervalsChanged)
    Q_PROPERTY(int interactiveWaveformUpdateIntervalMs
               READ interactiveWaveformUpdateIntervalMs NOTIFY updateIntervalsChanged)
    Q_PROPERTY(int overviewUpdateIntervalMs READ overviewUpdateIntervalMs
               NOTIFY updateIntervalsChanged)
    Q_PROPERTY(bool waveformRasterWorkEnabled READ waveformRasterWorkEnabled
               NOTIFY updateIntervalsChanged)

public:
    enum class Tier : std::uint8_t {
        Normal,
        Elevated,
        Critical,
        Suspended
    };

    struct Sample {
        bool applicationActive = true;
        bool windowMinimized = false;
        double callbackLoad = 0.0;
        bool callbackOverrun = false;
        bool hardwareXrun = false;
    };

    explicit RenderPressurePolicy(ControlClock& controlClock,
                                  AudioDeviceService& audioDeviceService,
                                  QObject* parent = nullptr);

    [[nodiscard]] QString tier() const;
    [[nodiscard]] int waveformUpdateIntervalMs() const noexcept;
    [[nodiscard]] int interactiveWaveformUpdateIntervalMs() const noexcept;
    [[nodiscard]] int overviewUpdateIntervalMs() const noexcept;
    [[nodiscard]] bool waveformRasterWorkEnabled() const noexcept
    {
        return rasterWorkAllowed(m_tier);
    }

    [[nodiscard]] static constexpr bool rasterWorkAllowed(Tier tier) noexcept
    {
        // Tile generation and texture publication are disposable visual work.
        // Stop them at the first measured pressure tier, before the callback is
        // close to an overrun; already-present scene-graph tiles keep moving.
        return tier == Tier::Normal;
    }

    void setApplicationActive(bool active);
    void setWindowMinimized(bool minimized);

    [[nodiscard]] static constexpr Tier targetTier(Sample sample) noexcept
    {
        if (sample.windowMinimized)
            return Tier::Suspended;
        if (sample.callbackOverrun || sample.hardwareXrun || sample.callbackLoad >= 0.85)
            return Tier::Critical;
        if (!sample.applicationActive || sample.callbackLoad >= 0.65)
            return Tier::Elevated;
        return Tier::Normal;
    }

signals:
    void tierChanged();
    void updateIntervalsChanged();

private:
    void sampleAudioLoad();
    void setTier(Tier tier);
    [[nodiscard]] Tier currentTargetTier() const noexcept;
    [[nodiscard]] static int severity(Tier tier) noexcept;

    AudioDeviceService& m_audioDeviceService;
    ControlClock::Registration m_clockRegistration;
    Tier m_tier = Tier::Normal;
    bool m_applicationActive = true;
    bool m_windowMinimized = false;
    std::uint64_t m_lastCallbackCount = 0;
    std::uint64_t m_lastCallbackUsec = 0;
    std::uint64_t m_lastCallbackOverruns = 0;
    std::uint64_t m_lastHardwareXruns = 0;
    unsigned int m_cleanSamples = 0;
};
