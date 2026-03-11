#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>

// Lightweight system resource monitor for CPU and RAM usage.
// Reads /proc/stat and /proc/meminfo on Linux at ~2 Hz.
class SystemMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double cpuUsage READ cpuUsage NOTIFY cpuUsageChanged)
    Q_PROPERTY(double ramUsage READ ramUsage NOTIFY ramUsageChanged)

public:
    explicit SystemMonitor(QObject* parent = nullptr);

    double cpuUsage() const { return m_cpuUsage; }
    double ramUsage() const { return m_ramUsage; }

signals:
    void cpuUsageChanged();
    void ramUsageChanged();

private slots:
    void poll();

private:
    QTimer m_timer;
    double m_cpuUsage = 0.0;
    double m_ramUsage = 0.0;

    // Previous /proc/stat totals for delta calculation
    long long m_prevTotal = 0;
    long long m_prevIdle  = 0;
};
