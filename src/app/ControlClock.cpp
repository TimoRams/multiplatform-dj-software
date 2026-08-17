#include "ControlClock.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace {

double periodFor(int hertz) noexcept
{
    return 1.0 / static_cast<double>(std::max(1, hertz));
}

void addSample(ControlClockGroupStats& stats, double microseconds) noexcept
{
    ++stats.executions;
    stats.averageDurationMicros +=
        (microseconds - stats.averageDurationMicros) / static_cast<double>(stats.executions);
    stats.worstDurationMicros = std::max(stats.worstDurationMicros, microseconds);
}

} // namespace

ControlClock::Registration::~Registration()
{
    reset();
}

ControlClock::Registration::Registration(Registration&& other) noexcept
    : m_clock(std::exchange(other.m_clock, nullptr))
    , m_id(std::exchange(other.m_id, 0))
{
}

ControlClock::Registration& ControlClock::Registration::operator=(Registration&& other) noexcept
{
    if (this != &other) {
        reset();
        m_clock = std::exchange(other.m_clock, nullptr);
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}

void ControlClock::Registration::reset() noexcept
{
    if (m_clock)
        m_clock->unregister(m_id);
    m_clock = nullptr;
    m_id = 0;
}

ControlClock::ControlClock(QObject* parent)
    : ControlClock(Configuration {}, parent)
{
}

ControlClock::ControlClock(Configuration configuration, QObject* parent)
    : QObject(parent)
    , m_configuration(configuration)
{
    m_configuration.baseTickHz = std::clamp(m_configuration.baseTickHz, 50, 1000);
    m_configuration.maximumDeltaSeconds = std::clamp(
        m_configuration.maximumDeltaSeconds, 0.010, 1.0);
    m_transportDeadline.periodSeconds = periodFor(m_configuration.transportHz);
    m_syncDeadline.periodSeconds = periodFor(m_configuration.syncHz);
    m_waveformDeadline.periodSeconds = periodFor(m_configuration.waveformHz);
    m_feedbackDeadline.periodSeconds = periodFor(m_configuration.feedbackHz);
    m_displayDeadline.periodSeconds = periodFor(m_configuration.displayHz);
    m_meterDeadline.periodSeconds = periodFor(m_configuration.metersHz);
    m_statisticsDeadline.periodSeconds = periodFor(m_configuration.statisticsHz);
    m_housekeepingDeadline.periodSeconds = periodFor(m_configuration.housekeepingHz);
    m_linkDeadline.periodSeconds = periodFor(m_configuration.linkHz);

    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(std::max(1, 1000 / m_configuration.baseTickHz));
    connect(&m_timer, &QTimer::timeout, this, &ControlClock::onBaseTick);
}

ControlClock::~ControlClock()
{
    stop();
    for (auto& slot : m_slots)
        slot = {};
}

ControlClock::Registration ControlClock::registerCallbacks(Callbacks callbacks)
{
    const auto free = std::find_if(m_slots.begin(), m_slots.end(),
                                   [](const Slot& slot) { return slot.id == 0; });
    if (free == m_slots.end())
        return {};
    const std::uint64_t id = m_nextRegistrationId++;
    free->id = id;
    free->callbacks = std::move(callbacks);
    return {this, id};
}

void ControlClock::unregister(std::uint64_t id) noexcept
{
    const auto slot = std::find_if(m_slots.begin(), m_slots.end(),
                                   [id](const Slot& value) { return value.id == id; });
    if (slot != m_slots.end())
        *slot = {};
}

void ControlClock::start()
{
    if (m_timer.isActive())
        return;
    m_monotonicClock.start();
    m_lastTickSeconds = 0.0;
    for (RateDeadline* deadline : {&m_transportDeadline, &m_syncDeadline,
                                   &m_waveformDeadline, &m_feedbackDeadline,
                                   &m_displayDeadline, &m_meterDeadline,
                                   &m_statisticsDeadline, &m_housekeepingDeadline,
                                   &m_linkDeadline})
        deadline->nextSeconds = 0.0;
    m_timer.start();
}

void ControlClock::stop() noexcept
{
    m_timer.stop();
}

void ControlClock::setBackgroundMode(bool enabled) noexcept
{
    m_backgroundMode = enabled;
}

void ControlClock::onBaseTick()
{
    const double now = static_cast<double>(m_monotonicClock.nsecsElapsed()) * 1.0e-9;
    const double elapsed = m_lastTickSeconds > 0.0
        ? now - m_lastTickSeconds : periodFor(m_configuration.baseTickHz);
    m_lastTickSeconds = now;
    dispatch(now, elapsed);
}

void ControlClock::advanceForTesting(double elapsedSeconds)
{
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0)
        elapsedSeconds = periodFor(m_configuration.baseTickHz);
    m_testNowSeconds += elapsedSeconds;
    dispatch(m_testNowSeconds, elapsedSeconds);
}

bool ControlClock::due(RateDeadline& deadline, double nowSeconds) noexcept
{
    if (deadline.nextSeconds <= 0.0)
        deadline.nextSeconds = nowSeconds;
    if (nowSeconds + 1.0e-12 < deadline.nextSeconds)
        return false;
    // Advance the deadline without dispatching missed periods. This preserves the
    // long-term rate while coalescing a pause into one current-state update.
    const double missed = std::floor((nowSeconds - deadline.nextSeconds)
                                     / deadline.periodSeconds) + 1.0;
    deadline.nextSeconds += std::max(1.0, missed) * deadline.periodSeconds;
    return true;
}

void ControlClock::dispatch(double monotonicSeconds, double elapsedSeconds)
{
    const auto tickStart = std::chrono::steady_clock::now();
    const double basePeriod = periodFor(m_configuration.baseTickHz);
    const double finiteElapsed = std::isfinite(elapsedSeconds) && elapsedSeconds > 0.0
        ? elapsedSeconds : basePeriod;
    const double lateness = std::max(0.0, finiteElapsed - basePeriod);
    const bool late = finiteElapsed > basePeriod * 1.5;
    const bool severelyLate = finiteElapsed > basePeriod * 3.0;
    const ControlTickContext context {
        ++m_tickIndex,
        std::isfinite(monotonicSeconds) ? monotonicSeconds : 0.0,
        std::clamp(finiteElapsed, 0.0, m_configuration.maximumDeltaSeconds),
        late,
        lateness
    };

    ++m_stats.totalTicks;
    if (late) {
        ++m_stats.lateTicks;
        const double latenessMicros = lateness * 1.0e6;
        m_stats.averageLatenessMicros += (latenessMicros - m_stats.averageLatenessMicros)
            / static_cast<double>(m_stats.lateTicks);
        m_stats.worstLatenessMicros = std::max(m_stats.worstLatenessMicros, latenessMicros);
    }

    std::uint64_t callbacks = 0;
    auto run = [&](auto member, ControlClockGroupStats& group) {
        const auto begin = std::chrono::steady_clock::now();
        std::uint64_t count = 0;
        for (const auto& slot : m_slots) {
            if (slot.id == 0)
                continue;
            const auto& callback = slot.callbacks.*member;
            if (callback) {
                callback(context);
                ++count;
            }
        }
        addSample(group, std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - begin).count());
        callbacks += count;
    };

    run(&Callbacks::fast, m_stats.fast);

    const bool transportDue = due(m_transportDeadline, monotonicSeconds);
    const bool syncDue = due(m_syncDeadline, monotonicSeconds);
    if (transportDue)
        run(&Callbacks::transport, m_stats.transport);
    if (syncDue) {
        const auto syncStart = std::chrono::steady_clock::now();
        for (auto member : {&Callbacks::syncInput, &Callbacks::syncCoordinate,
                            &Callbacks::syncApply}) {
            for (const auto& slot : m_slots) {
                if (slot.id == 0)
                    continue;
                const auto& callback = slot.callbacks.*member;
                if (callback) {
                    callback(context);
                    ++callbacks;
                }
            }
        }
        addSample(m_stats.sync, std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - syncStart).count());
    }

    if (due(m_waveformDeadline, monotonicSeconds)) {
        if (m_backgroundMode) {
            ++m_stats.skippedWaveformTicks;
        } else if (severelyLate) {
            ++m_stats.skippedWaveformTicks;
        } else {
            run(&Callbacks::waveform, m_stats.waveform);
            emit waveformTick();
        }
    }
    if (due(m_linkDeadline, monotonicSeconds)) {
        if (severelyLate)
            ++m_stats.skippedLinkTicks;
        else
            emit linkTick();
    }
    if (due(m_feedbackDeadline, monotonicSeconds)) {
        if (m_backgroundMode) {
            ++m_stats.skippedFeedbackTicks;
        } else if (severelyLate) {
            ++m_stats.skippedFeedbackTicks;
        } else {
            run(&Callbacks::feedback, m_stats.feedback);
            emit feedbackTick();
        }
    }
    if (due(m_displayDeadline, monotonicSeconds)) {
        if (m_backgroundMode) {
            ++m_stats.skippedDisplayTicks;
        } else if (severelyLate) {
            ++m_stats.skippedDisplayTicks;
        } else {
            run(&Callbacks::display, m_stats.display);
            emit displayTick();
        }
    }
    if (due(m_meterDeadline, monotonicSeconds)) {
        if (m_backgroundMode || severelyLate)
            ++m_stats.skippedMeterTicks;
        else
            run(&Callbacks::meters, m_stats.meters);
    }
    if (due(m_statisticsDeadline, monotonicSeconds)) {
        if (severelyLate)
            ++m_stats.skippedStatisticsTicks;
        else {
            run(&Callbacks::statistics, m_stats.statistics);
            emit statisticsTick();
        }
    }
    if (due(m_housekeepingDeadline, monotonicSeconds)) {
        if (severelyLate)
            ++m_stats.skippedHousekeepingTicks;
        else {
            run(&Callbacks::housekeeping, m_stats.housekeeping);
            emit housekeepingTick();
        }
    }

    m_stats.maxCallbacksPerTick = std::max(m_stats.maxCallbacksPerTick, callbacks);
    const double tickMicros = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - tickStart).count();
    m_stats.averageTickDurationMicros += (tickMicros - m_stats.averageTickDurationMicros)
        / static_cast<double>(m_stats.totalTicks);
    m_stats.worstTickDurationMicros = std::max(m_stats.worstTickDurationMicros, tickMicros);
}
