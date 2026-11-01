#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <utility>

// Owner-thread only. Run the first request immediately, coalesce a burst into
// one trailing refresh, and never lose the final cache/analysis publication.
class Flx10WaveformRefresh final {
public:
    void setCallback(std::function<void()> callback)
    {
        m_callback = std::move(callback);
        m_timer.setSingleShot(true);
        m_timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { refresh(); });
    }

    void request()
    {
        const auto elapsed = m_lastRefresh.isValid() ? m_lastRefresh.elapsed() : kIntervalMs;
        if (elapsed >= kIntervalMs) {
            m_timer.stop();
            refresh();
        } else if (!m_timer.isActive()) {
            // Do not restart on every publication: continuous analysis must
            // still advance the controller at a bounded rate.
            m_timer.start(static_cast<int>(kIntervalMs - elapsed));
        }
    }

    void reset()
    {
        m_timer.stop();
        m_lastRefresh.invalidate();
    }

private:
    void refresh()
    {
        m_lastRefresh.restart();
        if (m_callback)
            m_callback();
    }

    static constexpr int kIntervalMs = 250;
    QTimer m_timer;
    QElapsedTimer m_lastRefresh;
    std::function<void()> m_callback;
};
