#include "SystemMonitor.h"
#include <QFile>
#include <QDebug>
#include <QtGlobal>
#include <cmath>
#include <algorithm>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <juce_core/juce_core.h>
#endif

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
#if defined(Q_OS_LINUX)
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
#elif defined(Q_OS_WIN)
    // ── CPU usage from GetSystemTimes ─────────────────────────────────────
    {
        FILETIME idleTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
            ULARGE_INTEGER idle{};
            ULARGE_INTEGER kernel{};
            ULARGE_INTEGER user{};

            idle.LowPart = idleTime.dwLowDateTime;
            idle.HighPart = idleTime.dwHighDateTime;
            kernel.LowPart = kernelTime.dwLowDateTime;
            kernel.HighPart = kernelTime.dwHighDateTime;
            user.LowPart = userTime.dwLowDateTime;
            user.HighPart = userTime.dwHighDateTime;

            const unsigned long long total = kernel.QuadPart + user.QuadPart;
            const unsigned long long idleTicks = idle.QuadPart;

            const unsigned long long totalDelta = total - static_cast<unsigned long long>(m_prevTotal);
            const unsigned long long idleDelta = idleTicks - static_cast<unsigned long long>(m_prevIdle);

            if (totalDelta > 0) {
                double newCpu = 1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta);
                newCpu = std::clamp(newCpu, 0.0, 1.0);
                if (std::abs(newCpu - m_cpuUsage) > 0.005) {
                    m_cpuUsage = newCpu;
                    emit cpuUsageChanged();
                }
            }

            m_prevTotal = static_cast<long long>(total);
            m_prevIdle = static_cast<long long>(idleTicks);
        }
    }

    // ── RAM usage from GlobalMemoryStatusEx ───────────────────────────────
    {
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem) && mem.ullTotalPhys > 0) {
            double newRam = 1.0 - static_cast<double>(mem.ullAvailPhys) / static_cast<double>(mem.ullTotalPhys);
            newRam = std::clamp(newRam, 0.0, 1.0);
            if (std::abs(newRam - m_ramUsage) > 0.005) {
                m_ramUsage = newRam;
                emit ramUsageChanged();
            }
        }
    }
#elif defined(Q_OS_MACOS)
    // ── CPU usage from host_statistics(HOST_CPU_LOAD_INFO) ────────────────
    {
        host_cpu_load_info_data_t cpuInfo{};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

        if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                            reinterpret_cast<host_info_t>(&cpuInfo), &count) == KERN_SUCCESS) {
            const unsigned long long userTicks = static_cast<unsigned long long>(cpuInfo.cpu_ticks[CPU_STATE_USER])
                                               + static_cast<unsigned long long>(cpuInfo.cpu_ticks[CPU_STATE_NICE]);
            const unsigned long long systemTicks = static_cast<unsigned long long>(cpuInfo.cpu_ticks[CPU_STATE_SYSTEM]);
            const unsigned long long idleTicks = static_cast<unsigned long long>(cpuInfo.cpu_ticks[CPU_STATE_IDLE]);
            const unsigned long long total = userTicks + systemTicks + idleTicks;

            const unsigned long long totalDelta = total - static_cast<unsigned long long>(m_prevTotal);
            const unsigned long long idleDelta = idleTicks - static_cast<unsigned long long>(m_prevIdle);

            if (totalDelta > 0) {
                double newCpu = 1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta);
                newCpu = std::clamp(newCpu, 0.0, 1.0);
                if (std::abs(newCpu - m_cpuUsage) > 0.005) {
                    m_cpuUsage = newCpu;
                    emit cpuUsageChanged();
                }
            }

            m_prevTotal = static_cast<long long>(total);
            m_prevIdle = static_cast<long long>(idleTicks);
        }
    }

    // ── RAM usage from host_statistics64 ───────────────────────────────────
    {
        vm_statistics64_data_t vmStat{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        const kern_return_t vmOk = host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                                     reinterpret_cast<host_info64_t>(&vmStat), &count);

        const unsigned long long totalBytes =
            static_cast<unsigned long long>(juce::SystemStats::getMemorySizeInMegabytes()) * 1024ULL * 1024ULL;
        if (vmOk == KERN_SUCCESS && totalBytes > 0) {
            vm_size_t pageSize = 0;
            host_page_size(mach_host_self(), &pageSize);

            const unsigned long long freeBytes = static_cast<unsigned long long>(vmStat.free_count) * pageSize;
            const unsigned long long inactiveBytes = static_cast<unsigned long long>(vmStat.inactive_count) * pageSize;
            const unsigned long long availBytes = freeBytes + inactiveBytes;
            const unsigned long long clampedAvail = std::min(availBytes, totalBytes);

            double newRam = 1.0 - static_cast<double>(clampedAvail) / static_cast<double>(totalBytes);
            newRam = std::clamp(newRam, 0.0, 1.0);
            if (std::abs(newRam - m_ramUsage) > 0.005) {
                m_ramUsage = newRam;
                emit ramUsageChanged();
            }
        }
    }
#endif
}
