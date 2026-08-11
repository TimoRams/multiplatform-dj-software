#pragma once

#include "ScratchController.h"

#include <QElapsedTimer>
#include <cmath>

namespace engine::audio { class RenderModeRouter; }

namespace engine::scratch {

struct ScratchLoopCtx {
    bool active = false;
    double inSec = 0.0;
    double outSec = 0.0;
};

// UI/MIDI scratch session state — physics live in RenderModeRouter + ScratchController.
class ScratchSession {
public:
    static constexpr double kEventSpikeClampSec = 0.06;
    static constexpr double kDirectionFlipThresholdRate = 0.08;

    [[nodiscard]] bool scrubbing() const noexcept {
        return m_phase == ScratchPhase::TouchTracking;
    }
    [[nodiscard]] bool releaseGlide() const noexcept {
        return m_phase == ScratchPhase::ReleasePending
            || m_phase == ScratchPhase::CoastToDeckRate
            || m_phase == ScratchPhase::CoastToStop
            || m_phase == ScratchPhase::HandoffPending;
    }
    [[nodiscard]] ScratchPhase phase() const noexcept { return m_phase; }
    [[nodiscard]] bool wasPlaying() const noexcept { return m_wasPlaying; }
    [[nodiscard]] bool loopLocked() const noexcept { return m_loopLocked; }
    [[nodiscard]] bool savedReverse() const noexcept { return m_savedReverse; }
    [[nodiscard]] QElapsedTimer& physicsClock() noexcept { return m_physicsClock; }

    void setScrubbing(bool v) noexcept;
    void setReleaseGlide(bool v) noexcept;
    void setPhase(ScratchPhase phase) noexcept { m_phase = phase; }
    void setWasPlaying(bool v) noexcept { m_wasPlaying = v; }
    void setLoopLocked(bool v) noexcept { m_loopLocked = v; }
    void setSavedReverse(bool v) noexcept { m_savedReverse = v; }

    void clear() noexcept;

    // Loop wrap used at grab and during absolute scrub targets.
    [[nodiscard]] static double wrapLoopPosition(double posSec,
                                                 double trackLenSec,
                                                 const ScratchLoopCtx& loop,
                                                 bool& loopLocked) noexcept;

    [[nodiscard]] double armGrab(double grabSec, double trackLenSec, const ScratchLoopCtx& loop) noexcept;

    bool submitRelative(engine::audio::RenderModeRouter* bridge,
                        double deltaSec,
                        double sampleRate) noexcept;
    bool submitRelativeAtInterval(engine::audio::RenderModeRouter* bridge,
                                  double deltaSec,
                                  double sampleRate,
                                  double eventIntervalSeconds) noexcept;

    bool submitReleaseRelative(engine::audio::RenderModeRouter* bridge,
                               double deltaSec) noexcept;

    bool submitAbsolute(engine::audio::RenderModeRouter* bridge,
                        double posSec,
                        double sampleRate,
                        double trackLenSec,
                        double scratchPreRollSec,
                        const ScratchLoopCtx& loop) noexcept;

    [[nodiscard]] double lastRawSec() const noexcept { return m_lastRawSec; }

    // Returns current scratch rate after control-thread tick.
    double tick(engine::audio::RenderModeRouter* bridge, double dtSec) noexcept;

private:
    ScratchPhase m_phase = ScratchPhase::Idle;
    bool m_wasPlaying = false;
    bool m_loopLocked = false;
    bool m_savedReverse = false;
    double m_lastRawSec = 0.0;
    QElapsedTimer m_physicsClock;
    QElapsedTimer m_lastMoveClock;
};

} // namespace engine::scratch
