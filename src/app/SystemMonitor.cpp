#include "SystemMonitor.h"
#include <QFile>
#include <QDebug>
#include <algorithm>

SystemMonitor::SystemMonitor(QObject* parent)
    : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &SystemMonitor::poll);
    m_timer.start(500);  // 2 Hz
    poll();

    // Force-emit initial values for QML
    emit cpuUsageChanged();
    emit ramUsageChanged();
    qDebug() << "[SystemMonitor] initial CPU:" << m_cpuUsage << "RAM:" << m_ramUsage;
}

void SystemMonitor::poll()
{
    // ── CPU usage from /proc/stat ─────────────────────────────────────────
    {
        QFile f("/proc/stat");
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray line = f.readLine();
            if (line.startsWith("cpu ")) {
                auto parts = line.simplified().split(' ');
                if (parts.size() >= 5) {
                    long long user   = parts[1].toLongLong();
                    long long nice   = parts[2].toLongLong();
                    long long system = parts[3].toLongLong();
                    long long idle   = parts[4].toLongLong();
                    long long iowait = parts.size() > 5 ? parts[5].toLongLong() : 0;

                    long long total = user + nice + system + idle + iowait;
                    for (int i = 6; i < parts.size(); ++i)
                        total += parts[i].toLongLong();

                    long long totalDelta = total - m_prevTotal;
                    long long idleDelta  = idle  - m_prevIdle;

                    if (totalDelta > 0) {
                        double newCpu = 1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta);
                        newCpu = std::clamp(newCpu, 0.0, 1.0);
                        if (std::abs(newCpu - m_cpuUsage) > 0.005) {
                            m_cpuUsage = newCpu;
                            emit cpuUsageChanged();
                        }
                    }
                    m_prevTotal = total;
                    m_prevIdle  = idle;
                }
            }
        }
    }

    // ── RAM usage from /proc/meminfo ──────────────────────────────────────
    {
        QFile f("/proc/meminfo");
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray data = f.readAll();
            long long memTotal = 0, memAvailable = 0;

            for (const QByteArray& line : data.split('\n')) {
                if (line.startsWith("MemTotal:")) {
                    auto parts = line.simplified().split(' ');
                    if (parts.size() >= 2) memTotal = parts[1].toLongLong();
                } else if (line.startsWith("MemAvailable:")) {
                    auto parts = line.simplified().split(' ');
                    if (parts.size() >= 2) memAvailable = parts[1].toLongLong();
                }
                if (memTotal > 0 && memAvailable > 0) break;
            }

            if (memTotal > 0 && memAvailable > 0) {
                double newRam = 1.0 - static_cast<double>(memAvailable) / static_cast<double>(memTotal);
                newRam = std::clamp(newRam, 0.0, 1.0);
                if (std::abs(newRam - m_ramUsage) > 0.005) {
                    m_ramUsage = newRam;
                    emit ramUsageChanged();
                }
            }
        }
    }
}
