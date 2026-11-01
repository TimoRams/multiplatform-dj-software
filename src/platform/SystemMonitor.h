#pragma once

#include <QObject>
#include "app/ControlClock.h"

// Lightweight system resource monitor for CPU and RAM usage.
// Reads /proc/stat and /proc/meminfo on Linux at ~2 Hz.
class SystemMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double cpuUsage READ cpuUsage NOTIFY cpuUsageChanged)
    Q_PROPERTY(double ramUsage READ ramUsage NOTIFY ramUsageChanged)
    Q_PROPERTY(QString networkTransport READ networkTransport NOTIFY connectivityChanged)
    Q_PROPERTY(bool networkOnline READ networkOnline NOTIFY connectivityChanged)
    Q_PROPERTY(bool networkKnown READ networkKnown NOTIFY connectivityChanged)
    Q_PROPERTY(bool bluetoothEnabled READ bluetoothEnabled NOTIFY connectivityChanged)
    Q_PROPERTY(bool bluetoothAvailable READ bluetoothAvailable NOTIFY connectivityChanged)

public:
    explicit SystemMonitor(ControlClock& controlClock, QObject* parent = nullptr);

    double cpuUsage() const { return m_cpuUsage; }
    double ramUsage() const { return m_ramUsage; }

    QString networkTransport() const;
    bool networkOnline() const;
    bool networkKnown() const;
    bool bluetoothEnabled() const;
    bool bluetoothAvailable() const;

signals:
    void connectivityChanged();
    void cpuUsageChanged();
    void ramUsageChanged();

private slots:
    void poll();

private:
    QObject* m_bluetooth = nullptr;
    ControlClock::Registration m_clockRegistration;
    double m_cpuUsage = 0.0;
    double m_ramUsage = 0.0;

    // Previous /proc/stat totals for delta calculation
    long long m_prevTotal = 0;
    long long m_prevIdle  = 0;
};
