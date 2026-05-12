#pragma once
#include <QObject>

// Exposes cursor hide/restore/teleport to QML.
// Register as "cursorControl" context property before loading QML.
class CursorControl : public QObject {
    Q_OBJECT
public:
    explicit CursorControl(QObject* parent = nullptr);

    // Hide OS cursor (call once per drag start).
    Q_INVOKABLE void hideCursor();

    // Restore OS cursor (call once per drag end).
    Q_INVOKABLE void restoreCursor();

    // Teleport the cursor to global screen coordinates.
    Q_INVOKABLE void moveCursor(double x, double y);

private:
    bool m_hidden = false;
};
