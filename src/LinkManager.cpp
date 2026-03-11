#include "LinkManager.h"
#include <QDebug>
#include <cmath>

LinkManager::LinkManager(QObject* parent)
    : QObject(parent)
    , m_link(120.0)      // initial BPM
{
    // Register peer count callback (called on Link-managed thread)
    m_link.setNumPeersCallback([this](std::size_t peers) {
        int p = static_cast<int>(peers);
        if (m_numPeers != p) {
            m_numPeers = p;
            // Signal must be emitted on the main thread
            QMetaObject::invokeMethod(this, "numPeersChanged", Qt::QueuedConnection);
        }
    });

    // 60 Hz UI poll timer — reads Link state lock-free via captureAppSessionState()
    connect(&m_pollTimer, &QTimer::timeout, this, &LinkManager::pollLinkState);
    m_pollTimer.start(16);   // ~60 fps

    qDebug() << "[LinkManager] initialised (120 BPM, disabled)";
}

void LinkManager::setEnabled(bool on)
{
    if (m_link.isEnabled() == on) return;
    m_link.enable(on);
    qDebug() << "[LinkManager] Link" << (on ? "enabled" : "disabled");
    emit enabledChanged();
}

void LinkManager::pollLinkState()
{
    if (!m_link.isEnabled()) return;

    // captureAppSessionState() is thread-safe and non-blocking for UI threads
    const auto state = m_link.captureAppSessionState();
    const auto now   = m_link.clock().micros();

    const double quantum = 4.0;   // 4 beats per bar

    double newBpm   = state.tempo();
    double newBeat  = state.beatAtTime(now, quantum);
    double newPhase = state.phaseAtTime(now, quantum);

    // Only emit signals when values actually change (avoids unnecessary QML updates)
    constexpr double eps = 0.001;

    if (std::abs(newBpm - m_bpm) > eps) {
        m_bpm = newBpm;
        emit bpmChanged();
    }

    // Phase and beat change every frame — always update
    m_phase = newPhase;
    m_beat  = newBeat;
    emit phaseChanged();
    emit beatChanged();
}
