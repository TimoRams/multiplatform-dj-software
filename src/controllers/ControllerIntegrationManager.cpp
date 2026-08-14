#include "ControllerIntegrationManager.h"

#include <QDebug>

ControllerIntegrationManager::ControllerIntegrationManager(ControlClock& controlClock, QObject* parent)
    : QObject(parent)
    , m_flx10(controlClock)
{
    m_flx10HotplugTimer.setInterval(5000);
    m_flx10HotplugTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_flx10HotplugTimer, &QTimer::timeout,
            this, &ControllerIntegrationManager::retryFlx10Connection);
    connect(&m_flx10, &DDJFLX10Controller::statusChanged,
            this, &ControllerIntegrationManager::flx10StatusChanged);
    connect(&m_flx10, &DDJFLX10Controller::connectedChanged,
            this, &ControllerIntegrationManager::flx10StatusChanged);
}

ControllerIntegrationManager::~ControllerIntegrationManager()
{
    m_flx10HotplugTimer.stop();
    m_flx10.stop();
}

void ControllerIntegrationManager::setDecks(DjEngine* deckA, DjEngine* deckB)
{
    m_deckA = deckA;
    m_deckB = deckB;
    m_flx10.setDecks(m_deckA, m_deckB);
}

void ControllerIntegrationManager::prepareForShutdown() noexcept
{
    m_shuttingDown = true;
    m_flx10HotplugTimer.stop();
    m_flx10.prepareForShutdown();
    m_deckA = nullptr;
    m_deckB = nullptr;
}

void ControllerIntegrationManager::setFlx10Enabled(bool enabled)
{
    if (m_flx10Enabled == enabled)
        return;

    m_flx10Enabled = enabled;
    emit flx10EnabledChanged();

    if (m_flx10Enabled) {
        qInfo() << "[Controller] DDJ-FLX10 support enabled";
        m_flx10.start();
        if (!m_shuttingDown)
            m_flx10HotplugTimer.start();
    } else {
        qInfo() << "[Controller] DDJ-FLX10 support disabled";
        m_flx10HotplugTimer.stop();
        m_flx10.stop();
    }

    emit flx10StatusChanged();
}

void ControllerIntegrationManager::refreshFlx10()
{
    if (!m_flx10Enabled)
        return;

    m_flx10.stop();
    m_flx10.start();
    emit flx10StatusChanged();
}

void ControllerIntegrationManager::retryFlx10Connection()
{
    if (m_shuttingDown || !m_flx10Enabled || m_flx10.isConnected())
        return;

    qInfo() << "[Controller] DDJ-FLX10 hotplug scan: reconnecting";
    m_flx10.start();
    emit flx10StatusChanged();
}
