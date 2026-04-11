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

void LinkManager::publishDeckState(double bpm, double absoluteBeat, double quantum)
{
    if (!m_link.isEnabled())
        return;
    if (!std::isfinite(bpm) || bpm <= 0.0)
        return;
    if (!std::isfinite(absoluteBeat))
        return;
    if (!std::isfinite(quantum) || quantum <= 0.0)
        quantum = 4.0;

    auto sessionState = m_link.captureAppSessionState();
    const auto now = m_link.clock().micros();

    const double linkTempoNow = sessionState.tempo();
    const double linkBeatNow = sessionState.beatAtTime(now, quantum);
    const double tempoErr = std::abs(bpm - linkTempoNow);
    const double beatErr = std::abs(absoluteBeat - linkBeatNow);
    const double tempoStep = std::abs(bpm - m_lastPublishedTempo);
    const double beatStep = std::abs(absoluteBeat - m_lastPublishedBeat);

    // While tempo is being adjusted (pitch fader/sync), force regular commits so
    // the Link session follows immediately and the LINK panel stays in phase.
    const bool tempoActivelyChanging = (tempoStep > 0.001);

    constexpr std::int64_t kMinCommitIntervalUs = 40'000;   // 25 Hz cap
    constexpr std::int64_t kHeartbeatIntervalUs = 220'000;  // keep-alive
    constexpr double kTempoDeadband = 0.02;                 // BPM
    constexpr double kBeatDeadband = 0.03;                  // beats

    const std::int64_t nowUs = now.count();
    const bool heartbeatDue = (m_lastPublishMicros == 0)
                           || ((nowUs - m_lastPublishMicros) >= kHeartbeatIntervalUs);

    // If Link is already aligned, skip frequent commits to avoid UI/control jitter.
    if (!heartbeatDue
        && !tempoActivelyChanging
        && tempoErr < kTempoDeadband
        && beatErr < kBeatDeadband
        && tempoStep < kTempoDeadband
        && beatStep < kBeatDeadband)
        return;

    // Never commit faster than the throttle interval unless this is the first publish.
    if (m_lastPublishMicros != 0 && (nowUs - m_lastPublishMicros) < kMinCommitIntervalUs)
        return;

    sessionState.setTempo(bpm, now);
    sessionState.requestBeatAtTime(absoluteBeat, now, quantum);
    m_link.commitAppSessionState(sessionState);

    m_lastPublishMicros = nowUs;
    m_lastPublishedTempo = bpm;
    m_lastPublishedBeat = absoluteBeat;

    // Keep exposed properties responsive between poll ticks.
    // Detailed property updates are emitted by pollLinkState() at 60 Hz.
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
