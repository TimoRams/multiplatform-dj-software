#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

#include <array>
#include <cstdint>
#include <functional>

struct ControlTickContext {
    std::uint64_t tickIndex = 0;
    double monotonicSeconds = 0.0;
    double deltaSeconds = 0.0;
    bool late = false;
    double latenessSeconds = 0.0;
};

struct ControlClockGroupStats {
    std::uint64_t executions = 0;
    double averageDurationMicros = 0.0;
    double worstDurationMicros = 0.0;
};

struct ControlClockStats {
    std::uint64_t totalTicks = 0;
    std::uint64_t lateTicks = 0;
    std::uint64_t skippedWaveformTicks = 0;
    std::uint64_t skippedFeedbackTicks = 0;
    std::uint64_t skippedDisplayTicks = 0;
    std::uint64_t skippedMeterTicks = 0;
    std::uint64_t skippedLinkTicks = 0;
    std::uint64_t skippedStatisticsTicks = 0;
    std::uint64_t skippedHousekeepingTicks = 0;
    std::uint64_t maxCallbacksPerTick = 0;
    double averageTickDurationMicros = 0.0;
    double worstTickDurationMicros = 0.0;
    double averageLatenessMicros = 0.0;
    double worstLatenessMicros = 0.0;
    ControlClockGroupStats fast;
    ControlClockGroupStats transport;
    ControlClockGroupStats sync;
    ControlClockGroupStats waveform;
    ControlClockGroupStats feedback;
    ControlClockGroupStats display;
    ControlClockGroupStats meters;
    ControlClockGroupStats statistics;
    ControlClockGroupStats housekeeping;
};

class ControlClock final : public QObject {
    Q_OBJECT

public:
    struct Configuration {
        int baseTickHz = 250;
        int transportHz = 125;
        int syncHz = 125;
        int waveformHz = 60;
        int feedbackHz = 30;
        int displayHz = 60;
        int metersHz = 30;
        int statisticsHz = 10;
        int housekeepingHz = 2;
        int linkHz = 20;
        double maximumDeltaSeconds = 0.100;
    };

    struct Callbacks {
        std::function<void(const ControlTickContext&)> fast;
        std::function<void(const ControlTickContext&)> transport;
        std::function<void(const ControlTickContext&)> syncInput;
        std::function<void(const ControlTickContext&)> syncCoordinate;
        std::function<void(const ControlTickContext&)> syncApply;
        std::function<void(const ControlTickContext&)> waveform;
        std::function<void(const ControlTickContext&)> feedback;
        std::function<void(const ControlTickContext&)> display;
        std::function<void(const ControlTickContext&)> meters;
        std::function<void(const ControlTickContext&)> statistics;
        std::function<void(const ControlTickContext&)> housekeeping;
    };

    class Registration final {
    public:
        Registration() = default;
        Registration(ControlClock* clock, std::uint64_t id) noexcept : m_clock(clock), m_id(id) {}
        ~Registration();
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& other) noexcept;
        Registration& operator=(Registration&& other) noexcept;
        void reset() noexcept;
        [[nodiscard]] bool valid() const noexcept { return m_clock != nullptr; }

    private:
        ControlClock* m_clock = nullptr;
        std::uint64_t m_id = 0;
    };

    explicit ControlClock(QObject* parent = nullptr);
    ControlClock(Configuration configuration, QObject* parent);
    ~ControlClock() override;

    [[nodiscard]] Registration registerCallbacks(Callbacks callbacks);
    void start();
    void stop() noexcept;
    [[nodiscard]] bool isRunning() const noexcept { return m_timer.isActive(); }
    [[nodiscard]] ControlClockStats stats() const noexcept { return m_stats; }
    [[nodiscard]] Configuration configuration() const noexcept { return m_configuration; }

    // Deterministic test hook. Dispatches once and never runs catch-up ticks.
    void advanceForTesting(double elapsedSeconds);

signals:
    void waveformTick();
    void feedbackTick();
    void displayTick();
    void statisticsTick();
    void housekeepingTick();
    void linkTick();

private:
    struct Slot {
        std::uint64_t id = 0;
        Callbacks callbacks;
    };
    struct RateDeadline {
        double periodSeconds = 1.0;
        double nextSeconds = 0.0;
    };

    void unregister(std::uint64_t id) noexcept;
    void onBaseTick();
    void dispatch(double monotonicSeconds, double elapsedSeconds);
    [[nodiscard]] bool due(RateDeadline& deadline, double nowSeconds) noexcept;
    friend class Registration;
    static constexpr std::size_t kMaximumRegistrations = 24;
    Configuration m_configuration;
    QTimer m_timer;
    QElapsedTimer m_monotonicClock;
    std::array<Slot, kMaximumRegistrations> m_slots {};
    std::uint64_t m_nextRegistrationId = 1;
    std::uint64_t m_tickIndex = 0;
    double m_lastTickSeconds = 0.0;
    double m_testNowSeconds = 0.0;
    RateDeadline m_transportDeadline;
    RateDeadline m_syncDeadline;
    RateDeadline m_waveformDeadline;
    RateDeadline m_feedbackDeadline;
    RateDeadline m_displayDeadline;
    RateDeadline m_meterDeadline;
    RateDeadline m_statisticsDeadline;
    RateDeadline m_housekeepingDeadline;
    RateDeadline m_linkDeadline;
    ControlClockStats m_stats;
};
