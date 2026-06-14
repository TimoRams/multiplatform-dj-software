#pragma once

#include <QObject>
#include <QString>

#include "flx10/DDJFLX10Controller.h"

class DjEngine;

class ControllerIntegrationManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool flx10Enabled READ flx10Enabled WRITE setFlx10Enabled NOTIFY flx10EnabledChanged)
    Q_PROPERTY(bool flx10Connected READ flx10Connected NOTIFY flx10StatusChanged)
    Q_PROPERTY(QString flx10Status READ flx10Status NOTIFY flx10StatusChanged)

public:
    explicit ControllerIntegrationManager(QObject* parent = nullptr);
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
    bool m_flx10Enabled = false;
    DjEngine* m_deckA = nullptr;
    DjEngine* m_deckB = nullptr;
    DDJFLX10Controller m_flx10;
};

