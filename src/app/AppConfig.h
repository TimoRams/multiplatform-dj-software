#pragma once

#include <QObject>
#include <QString>

class AppConfig : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool firstRunCompleted READ firstRunCompleted NOTIFY firstRunCompletedChanged)

public:
    explicit AppConfig(QObject* parent = nullptr);

    void init(const QString& configDirectoryPath);

    bool firstRunCompleted() const { return m_firstRunCompleted; }

    // Called when the user clicks Start on the welcome screen.
    // persist=true → saves firstRunCompleted=true to disk (don't show again).
    // persist=false → only hides for this session.
    Q_INVOKABLE void completeFirstRun(bool persist);

signals:
    void firstRunCompletedChanged();

private:
    void load();
    void save();

    QString m_filePath;
    bool m_firstRunCompleted = false;
};
