#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include "flx10/DDJFLX10Controller.h"
#include "deck/DjEngine.h"

class ControllerIntegrationManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool flx10Enabled READ flx10Enabled WRITE setFlx10Enabled NOTIFY flx10EnabledChanged)
    Q_PROPERTY(bool flx10Connected READ flx10Connected NOTIFY flx10StatusChanged)
    Q_PROPERTY(QString flx10Status READ flx10Status NOTIFY flx10StatusChanged)

public:
    explicit ControllerIntegrationManager(ControlClock& controlClock, QObject* parent = nullptr);
    ~ControllerIntegrationManager() override;

    void setDecks(DjEngine* deckA, DjEngine* deckB);
    void prepareForShutdown() noexcept;

    bool flx10Enabled() const { return m_flx10Enabled; }
    bool flx10Connected() const { return m_flx10.isConnected(); }
    QString flx10Status() const { return m_flx10.status(); }

public slots:
    void setFlx10Enabled(bool enabled);
    void refreshFlx10();

signals:
    void flx10EnabledChanged();
    void flx10StatusChanged();

private:
    void retryFlx10Connection();

    bool m_flx10Enabled = false;
    bool m_shuttingDown = false;
    QPointer<DjEngine> m_deckA;
    QPointer<DjEngine> m_deckB;
    DDJFLX10Controller m_flx10;
    QTimer m_flx10HotplugTimer;
};
