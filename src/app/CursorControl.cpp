#include "CursorControl.h"
#include <QGuiApplication>
#include <QCursor>
#include <cmath>

CursorControl::CursorControl(QObject* parent) : QObject(parent) {}

void CursorControl::hideCursor()
{
    if (!m_hidden) {
        QGuiApplication::setOverrideCursor(Qt::BlankCursor);
        m_hidden = true;
    }
}

void CursorControl::restoreCursor()
{
    if (m_hidden) {
        QGuiApplication::restoreOverrideCursor();
        m_hidden = false;
    }
}

void CursorControl::moveCursor(double x, double y)
{
    QCursor::setPos(static_cast<int>(std::round(x)), static_cast<int>(std::round(y)));
}
