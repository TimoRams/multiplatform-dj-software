#pragma once

#include <QObject>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <ableton/Link.hpp>

class LinkManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(double bpm READ bpm NOTIFY bpmChanged)
    Q_PROPERTY(double phase READ phase NOTIFY phaseChanged)
    Q_PROPERTY(double beat READ beat NOTIFY beatChanged)
    Q_PROPERTY(int numPeers READ numPeers NOTIFY numPeersChanged)


public:
    explicit LinkManager(QObject* parent = nullptr);
    ~LinkManager() override;

    void shutdown();

    bool   enabled()  const { return m_link.isEnabled(); }
    double bpm()      const { return m_bpm; }
    double phase()    const { return m_phase; }
    double beat()     const { return m_beat; }
    int    numPeers() const { return m_numPeers; }

    void setEnabled(bool on);

    Q_INVOKABLE void publishDeckState(double bpm, double absoluteBeat, double quantum = 4.0);

signals:
    void enabledChanged();
    void bpmChanged();
    void phaseChanged();
    void beatChanged();
    void numPeersChanged();

private slots:
    void pollLinkState();

private:
    ableton::Link m_link;
    QTimer        m_pollTimer;
    std::atomic<bool> m_shuttingDown { false };

    double m_bpm      = 120.0;
    double m_phase    = 0.0;
    double m_beat     = 0.0;
    int    m_numPeers = 0;

    std::int64_t m_lastPublishMicros = 0;
    double m_lastPublishedTempo = 120.0;
    double m_lastPublishedBeat = 0.0;
};
