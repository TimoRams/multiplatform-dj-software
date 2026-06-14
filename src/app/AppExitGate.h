#pragma once

#include <QObject>
#include <functional>

// QML entry point for ordered app teardown before Qt.quit().
class AppExitGate : public QObject
{
    Q_OBJECT

public:
    explicit AppExitGate(QObject* parent = nullptr) : QObject(parent) {}

    void setHandler(std::function<void(bool manualBackup)> handler)
    {
        m_handler = std::move(handler);
    }

    Q_INVOKABLE void finalizeExit(bool manualBackup)
    {
        if (m_handler)
            m_handler(manualBackup);
    }

private:
    std::function<void(bool manualBackup)> m_handler;
};
