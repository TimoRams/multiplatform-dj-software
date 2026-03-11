#pragma once

#include <QObject>
#include <QTimer>
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
    ~LinkManager() override = default;

    bool   enabled()  const { return m_link.isEnabled(); }
    double bpm()      const { return m_bpm; }
    double phase()    const { return m_phase; }
    double beat()     const { return m_beat; }
    int    numPeers() const { return m_numPeers; }

    void setEnabled(bool on);

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

    double m_bpm      = 120.0;
    double m_phase    = 0.0;
    double m_beat     = 0.0;
    int    m_numPeers = 0;
};
