#pragma once

#include "stems/StemWaveformOverlay.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <functional>
#include <utility>

class StemWaveformAnimator final : public QObject {
public:
    using Overlay = brockdj::stems::StemWaveformOverlay;

    StemWaveformAnimator(QObject* parent, std::function<void()> repaint,
                        std::function<double()> timeSourceMilliseconds = {})
        : QObject(parent), m_repaint(std::move(repaint)),
          m_timeSourceMilliseconds(std::move(timeSourceMilliseconds))
    {
        m_clock.start();
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(16);
        connect(&m_timer, &QTimer::timeout, this, [this] { advance(); });
    }

    void setTarget(std::shared_ptr<const Overlay> next)
    {
        if ((m_target ? m_target->revision : 0) == (next ? next->revision : 0))
            return;
        if (!next || !m_target || next->data != m_target->data)
            m_transition = {};
        m_target = std::move(next);
        if (m_target)
            m_transition.retarget(m_target->gains, now());
        advance();
    }

    [[nodiscard]] const std::shared_ptr<const Overlay>& frame() const noexcept { return m_frame; }

private:
    [[nodiscard]] double now() const
    {
        return m_timeSourceMilliseconds ? m_timeSourceMilliseconds() : m_clock.nsecsElapsed() / 1'000'000.0;
    }

    void advance()
    {
        const double time = now();
        if (m_target) {
            auto frame = std::make_shared<Overlay>(*m_target);
            frame->gains = m_transition.value(time);
            frame->revision = brockdj::stems::stemWaveformOverlayRevision(
                frame->data->trackId(), frame->gains, frame->data->envelopeEpoch());
            m_frame = std::move(frame);
        } else {
            m_frame.reset();
        }
        if (m_target && m_transition.active(time))
            m_timer.start();
        else
            m_timer.stop();
        m_repaint();
    }

    QElapsedTimer m_clock;
    QTimer m_timer;
    brockdj::stems::StemWaveformTransition m_transition;
    std::shared_ptr<const Overlay> m_target;
    std::shared_ptr<const Overlay> m_frame;
    std::function<void()> m_repaint;
    std::function<double()> m_timeSourceMilliseconds;
};
