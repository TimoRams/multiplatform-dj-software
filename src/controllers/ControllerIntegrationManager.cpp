#include "ControllerIntegrationManager.h"

#include <QDebug>

ControllerIntegrationManager::ControllerIntegrationManager(ControlClock& controlClock, QObject* parent)
    : QObject(parent)
    , m_flx10(controlClock)
{
    m_retryClock.start();
    m_flx10HotplugTimer.setInterval(5000);
    m_flx10HotplugTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_flx10HotplugTimer, &QTimer::timeout,
            this, &ControllerIntegrationManager::retryFlx10Connection);
    connect(&m_flx10, &DDJFLX10Controller::statusChanged,
            this, &ControllerIntegrationManager::observeFlx10Status);
    connect(&m_flx10, &DDJFLX10Controller::connectedChanged,
            this, &ControllerIntegrationManager::observeFlx10Status);
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
    m_retryPolicy.disable();
    m_flx10HotplugTimer.stop();
    m_flx10.prepareForShutdown();
    m_deckA = nullptr;
    m_deckB = nullptr;
}

void ControllerIntegrationManager::setFlx10Enabled(bool enabled)
{
    if (m_shuttingDown || m_flx10Enabled == enabled)
        return;

    m_flx10Enabled = enabled;
    emit flx10EnabledChanged();

    if (m_flx10Enabled) {
        qInfo() << "[Controller] DDJ-FLX10 support enabled";
        m_retryPolicy.enable(DDJFLX10Controller::deviceSignature());
        attemptFlx10Connection(false);
        if (!m_shuttingDown && flx10Available())
            m_flx10HotplugTimer.start();
    } else {
        qInfo() << "[Controller] DDJ-FLX10 support disabled";
        m_flx10HotplugTimer.stop();
        m_retryPolicy.disable();
        m_wasConnected = false;
        m_flx10.stop();
    }

    emit flx10StatusChanged();
}

QString ControllerIntegrationManager::flx10ConnectionState() const
{
    if (!m_flx10Enabled || m_shuttingDown) return QStringLiteral("DISABLED");
    if (m_attempting) return QStringLiteral("CONNECTING");
    if (m_flx10.isConnected()) return QStringLiteral("CONNECTED");
    if (m_retryPolicy.failure() == flx10::ConnectionFailure::Absent) return QStringLiteral("WAITING_FOR_DEVICE");
    if (m_retryPolicy.retryPending()) return QStringLiteral("WAITING_TO_RETRY");
    return QStringLiteral("ACTION_REQUIRED");
}

void ControllerIntegrationManager::observeFlx10Status()
{
    if (!m_attempting && m_flx10Enabled && !m_shuttingDown
        && m_wasConnected && !m_flx10.isConnected()) {
        m_wasConnected = false;
        m_retryPolicy.failed(m_flx10.connectionFailure(), m_retryClock.elapsed());
    }
    emit flx10StatusChanged();
}

void ControllerIntegrationManager::attemptFlx10Connection(bool quiet)
{
    if (m_attempting || m_shuttingDown || !m_flx10Enabled) return;
    m_attempting = true;
    if (!quiet) emit flx10StatusChanged();
    const bool connected = m_flx10.start(quiet);
    if (connected) m_retryPolicy.connected();
    else m_retryPolicy.failed(m_flx10.connectionFailure(), m_retryClock.elapsed());
    m_wasConnected = connected;
    m_attempting = false;
    emit flx10StatusChanged();
}

void ControllerIntegrationManager::refreshFlx10()
{
    if (m_shuttingDown || !m_flx10Enabled || m_attempting) return;
    m_attempting = true;
    m_flx10.stop(false);
    m_wasConnected = false;
    m_retryPolicy.reset(DDJFLX10Controller::deviceSignature());
    m_attempting = false;
    attemptFlx10Connection(false);
}

void ControllerIntegrationManager::retryFlx10Connection()
{
    if (m_shuttingDown || !m_flx10Enabled || m_flx10.isConnected()) return;
    const auto signature = DDJFLX10Controller::deviceSignature();
    if (m_retryPolicy.shouldAttempt(signature, m_retryClock.elapsed()))
        attemptFlx10Connection(true);
    else if (signature.isEmpty() && m_retryPolicy.failure() == flx10::ConnectionFailure::None) {
        m_retryPolicy.failed(flx10::ConnectionFailure::Absent, m_retryClock.elapsed());
        emit flx10StatusChanged();
    }
}
