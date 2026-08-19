#include "RenderPressurePolicy.h"

#include "audio/AudioEngine.h"
#include "audio/device/AudioDeviceService.h"

#include <algorithm>
#include <utility>

namespace {

constexpr unsigned int kCleanSamplesBeforeRelaxing = 12;

const char* tierName(RenderPressurePolicy::Tier tier) noexcept
{
    switch (tier) {
    case RenderPressurePolicy::Tier::Normal:
        return "normal";
    case RenderPressurePolicy::Tier::Elevated:
        return "reduced";
    case RenderPressurePolicy::Tier::Critical:
        return "audio-first";
    case RenderPressurePolicy::Tier::Suspended:
        return "suspended";
    }
    return "unknown";
}

} // namespace

RenderPressurePolicy::RenderPressurePolicy(ControlClock& controlClock,
                                           AudioDeviceService& audioDeviceService,
                                           QObject* parent)
    : QObject(parent)
    , m_audioDeviceService(audioDeviceService)
{
    ControlClock::Callbacks callbacks;
    callbacks.statistics = [this](const ControlTickContext&) { sampleAudioLoad(); };
    m_clockRegistration = controlClock.registerCallbacks(std::move(callbacks));
}

QString RenderPressurePolicy::tier() const
{
    return QString::fromLatin1(tierName(m_tier));
}

int RenderPressurePolicy::waveformUpdateIntervalMs() const noexcept
{
    switch (m_tier) {
    case Tier::Normal:
        return 16;
    case Tier::Elevated:
        return 33;
    case Tier::Critical:
        return 66;
    case Tier::Suspended:
        return 250;
    }
    return 66;
}

int RenderPressurePolicy::interactiveWaveformUpdateIntervalMs() const noexcept
{
    switch (m_tier) {
    case Tier::Normal:
        return 16;
    case Tier::Elevated:
    case Tier::Critical:
        return 33;
    case Tier::Suspended:
        return 250;
    }
    return 33;
}

int RenderPressurePolicy::overviewUpdateIntervalMs() const noexcept
{
    switch (m_tier) {
    case Tier::Normal:
        return 100;
    case Tier::Elevated:
        return 200;
    case Tier::Critical:
        return 500;
    case Tier::Suspended:
        return 1000;
    }
    return 500;
}

void RenderPressurePolicy::setApplicationActive(bool active)
{
    if (m_applicationActive == active)
        return;
    m_applicationActive = active;
    m_cleanSamples = 0;
    setTier(currentTargetTier());
}

void RenderPressurePolicy::setWindowMinimized(bool minimized)
{
    if (m_windowMinimized == minimized)
        return;
    m_windowMinimized = minimized;
    m_cleanSamples = 0;
    if (minimized)
        setTier(Tier::Suspended);
    else
        setTier(currentTargetTier());
}

void RenderPressurePolicy::sampleAudioLoad()
{
    const auto callbacks = AudioEngine::callbackCount();
    const auto callbackUsec = AudioEngine::callbackTotalUsec();
    const auto overruns = AudioEngine::callbackOverrunCount();
    const auto xruns = m_audioDeviceService.hardwareXRunCount();

    double callbackLoad = 0.0;
    if (callbacks >= m_lastCallbackCount && callbackUsec >= m_lastCallbackUsec) {
        const auto callbackDelta = callbacks - m_lastCallbackCount;
        const auto usecDelta = callbackUsec - m_lastCallbackUsec;
        const int sampleRate = m_audioDeviceService.currentSampleRate();
        const int bufferSize = m_audioDeviceService.currentBufferSize();
        if (callbackDelta > 0 && sampleRate > 0 && bufferSize > 0) {
            const double averageUsec = static_cast<double>(usecDelta)
                / static_cast<double>(callbackDelta);
            const double budgetUsec = static_cast<double>(bufferSize) * 1.0e6
                / static_cast<double>(sampleRate);
            callbackLoad = budgetUsec > 0.0 ? averageUsec / budgetUsec : 0.0;
        }
    }

    const Sample sample{
        .applicationActive = m_applicationActive,
        .windowMinimized = m_windowMinimized,
        .callbackLoad = std::max(0.0, callbackLoad),
        .callbackOverrun = overruns > m_lastCallbackOverruns,
        .hardwareXrun = xruns > m_lastHardwareXruns
    };
    m_lastCallbackCount = callbacks;
    m_lastCallbackUsec = callbackUsec;
    m_lastCallbackOverruns = overruns;
    m_lastHardwareXruns = xruns;

    const Tier target = targetTier(sample);
    if (severity(target) >= severity(m_tier)) {
        m_cleanSamples = 0;
        setTier(target);
        return;
    }

    if (++m_cleanSamples >= kCleanSamplesBeforeRelaxing) {
        m_cleanSamples = 0;
        setTier(target);
    }
}

void RenderPressurePolicy::setTier(Tier tier)
{
    if (m_tier == tier)
        return;
    m_tier = tier;
    emit tierChanged();
    emit updateIntervalsChanged();
}

RenderPressurePolicy::Tier RenderPressurePolicy::currentTargetTier() const noexcept
{
    return targetTier({
        .applicationActive = m_applicationActive,
        .windowMinimized = m_windowMinimized
    });
}

int RenderPressurePolicy::severity(Tier tier) noexcept
{
    switch (tier) {
    case Tier::Normal:
        return 0;
    case Tier::Elevated:
        return 1;
    case Tier::Critical:
        return 2;
    case Tier::Suspended:
        return 3;
    }
    return 0;
}
