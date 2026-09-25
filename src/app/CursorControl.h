#pragma once

#include <QCursor>
#include <QGuiApplication>
#include <QObject>

#include <cmath>

// Exposes cursor hide/restore/teleport to QML.
// Register as "cursorControl" context property before loading QML.
class CursorControl final : public QObject {
    Q_OBJECT

public:
    explicit CursorControl(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    Q_INVOKABLE void hideCursor()
    {
        if (m_hidden)
            return;
        QGuiApplication::setOverrideCursor(Qt::BlankCursor);
        m_hidden = true;
    }

    Q_INVOKABLE void restoreCursor()
    {
        if (!m_hidden)
            return;
        QGuiApplication::restoreOverrideCursor();
        m_hidden = false;
    }

    Q_INVOKABLE void moveCursor(double x, double y)
    {
        QCursor::setPos(static_cast<int>(std::round(x)),
                        static_cast<int>(std::round(y)));
    }

private:
    bool m_hidden = false;
};
