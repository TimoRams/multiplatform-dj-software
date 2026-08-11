#include "DDJFLX10Controller.h"

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
#include <libusb-1.0/libusb.h>
#endif

#include <QDebug>
#include <QThread>

#include <algorithm>
#include <chrono>

#include "Flx10Protocol.h"

using namespace flx10_protocol;

bool DDJFLX10Controller::initialiseUsb()
{
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    const int initResult = libusb_init(&m_usbContext);
    if (initResult != 0) {
        setStatus(QStringLiteral("DDJ-FLX10: libusb init failed (%1)").arg(initResult));
        return false;
    }

    libusb_device** devices = nullptr;
    const ssize_t deviceCount = libusb_get_device_list(m_usbContext, &devices);
    libusb_device* flx10Device = nullptr;
    for (ssize_t i = 0; i < deviceCount; ++i) {
        libusb_device_descriptor descriptor {};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0)
            continue;
        if (descriptor.idVendor == kVid && descriptor.idProduct == kPid) {
            flx10Device = devices[i];
            break;
        }
    }

    if (!flx10Device) {
        setStatus(QStringLiteral("DDJ-FLX10: USB device 2B73:0041 not found"));
        if (devices)
            libusb_free_device_list(devices, 1);
        libusb_exit(m_usbContext);
        m_usbContext = nullptr;
        return false;
    }

    const int openResult = libusb_open(flx10Device, &m_handle);
    libusb_free_device_list(devices, 1);
    if (openResult != 0 || !m_handle) {
        setStatus(QStringLiteral("DDJ-FLX10: USB device found, but access was denied (%1). Install the BrockDJ udev rule, then reconnect the controller.")
                      .arg(openResult));
        libusb_exit(m_usbContext);
        m_usbContext = nullptr;
        return false;
    }

    libusb_set_auto_detach_kernel_driver(m_handle, 1);
    libusb_set_configuration(m_handle, 1);
    return true;
#else
    return false;
#endif
}
bool DDJFLX10Controller::sendVendorUnlock()
{
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    if (!m_handle)
        return false;

    bool anyOk = false;
    for (const auto& command : kVendorUnlockCommands) {
        const int result = libusb_control_transfer(m_handle,
                                                   0x40,
                                                   3,
                                                   command.value,
                                                   command.index,
                                                   nullptr,
                                                   0,
                                                   500);
        anyOk = anyOk || result >= 0;
        QThread::msleep(5);
    }
    QThread::msleep(250);
    return anyOk;
#else
    return false;
#endif
}
bool DDJFLX10Controller::claimScreenInterface()
{
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    if (!m_handle)
        return false;

    const int claimResult = libusb_claim_interface(m_handle, kScreenInterface);
    if (claimResult != 0) {
        qWarning() << "[DDJ-FLX10] claim interface failed" << claimResult;
        return false;
    }
    m_interfaceClaimed = true;

    libusb_device* device = libusb_get_device(m_handle);
    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != 0 || config == nullptr)
        return false;

    bool found = false;
    for (int i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& usbInterface = config->interface[i];
        for (int j = 0; j < usbInterface.num_altsetting && !found; ++j) {
            const libusb_interface_descriptor& descriptor = usbInterface.altsetting[j];
            if (descriptor.bInterfaceNumber != kScreenInterface)
                continue;

            for (int e = 0; e < descriptor.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& endpoint = descriptor.endpoint[e];
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
                    m_outEndpoint = endpoint.bEndpointAddress;
                    found = true;
                    break;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);
    if (!found)
        qWarning() << "[DDJ-FLX10] no HID OUT endpoint found";
    return found;
#else
    return false;
#endif
}
bool DDJFLX10Controller::writePacket(const QByteArray& packetBytes)
{
#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
    if (!m_handle || !m_outEndpoint || packetBytes.size() != kHidPacketSize)
        return false;

    std::lock_guard lock(m_hidWriteMutex);
    if (!m_hidWriterRunning || m_hidWriterStopping
        || !m_hidWriteHealthy.load(std::memory_order_acquire)) {
        return false;
    }

    // 0x27 is mutable state, not an ordered upload. Keep one latest packet per
    // deck in priority slots so a new scratch cursor never waits behind cover,
    // beatgrid or full-waveform packets already in the static FIFO.
    if (packetBytes.at(1) == char(0x27)) {
        const auto sequence = m_displaySnapshotsPublished.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (m_latestHidDisplayPackets.publish(packetBytes, sequence))
            m_displayFramesCoalesced.fetch_add(1, std::memory_order_relaxed);
        const auto depth = static_cast<std::uint64_t>(
            m_hidWriteQueue.size() + m_latestHidDisplayPackets.size());
        auto maximum = m_maximumDisplayQueueDepth.load(std::memory_order_relaxed);
        while (depth > maximum
               && !m_maximumDisplayQueueDepth.compare_exchange_weak(
                   maximum, depth, std::memory_order_relaxed)) {}
        m_hidWriteCondition.notify_one();
        return true;
    }

    if (m_hidWriteQueue.size() >= kHidWriteQueueCapacity)
        return false;

    m_hidWriteQueue.push_back(packetBytes);
    m_hidWriteCondition.notify_one();
    return true;
#else
    Q_UNUSED(packetBytes)
    return false;
#endif
}

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)

void DDJFLX10Controller::startHidWriter()
{
    stopHidWriter();
    {
        std::lock_guard lock(m_hidWriteMutex);
        m_hidWriteQueue.clear();
        m_latestHidDisplayPackets.clear();
        m_hidWriterStopping = false;
        m_hidWriterRunning = true;
        m_hidWriteHealthy.store(true, std::memory_order_release);
        m_hidWriteFailurePending.store(false, std::memory_order_release);
        m_hidWriteError.store(0, std::memory_order_release);
        m_hidWriteTransferred.store(0, std::memory_order_release);
        m_displaySnapshotsPublished.store(0, std::memory_order_relaxed);
        m_displayFramesSent.store(0, std::memory_order_relaxed);
        m_displayFramesCoalesced.store(0, std::memory_order_relaxed);
        m_displayWriteFailures.store(0, std::memory_order_relaxed);
        m_displayWriteTimeouts.store(0, std::memory_order_relaxed);
        m_maximumDisplayQueueDepth.store(0, std::memory_order_relaxed);
        m_lastDisplaySequence.store(0, std::memory_order_relaxed);
        m_worstDisplayWriteUsec.store(0, std::memory_order_relaxed);
        m_hidDiagnosticsEnabled = qEnvironmentVariableIsSet("BROCKDJ_FLX10_DIAGNOSTICS");
    }
    m_hidWriter = std::thread([this] { hidWriterLoop(); });
}

void DDJFLX10Controller::stopHidWriter() noexcept
{
    {
        std::lock_guard lock(m_hidWriteMutex);
        m_hidWriterStopping = true;
        m_hidWriteQueue.clear();
        m_latestHidDisplayPackets.clear();
    }
    m_hidWriteCondition.notify_all();
    if (m_hidWriter.joinable())
        m_hidWriter.join();
    {
        std::lock_guard lock(m_hidWriteMutex);
        m_hidWriterRunning = false;
        m_hidWriterStopping = false;
    }
    if (m_hidDiagnosticsEnabled
        && m_displaySnapshotsPublished.load(std::memory_order_relaxed) > 0) {
        qInfo() << "[DDJ-FLX10 diagnostics] display snapshots="
                << m_displaySnapshotsPublished.load(std::memory_order_relaxed)
                << "sent=" << m_displayFramesSent.load(std::memory_order_relaxed)
                << "coalesced=" << m_displayFramesCoalesced.load(std::memory_order_relaxed)
                << "writeFailures=" << m_displayWriteFailures.load(std::memory_order_relaxed)
                << "timeouts=" << m_displayWriteTimeouts.load(std::memory_order_relaxed)
                << "maxQueueDepth=" << m_maximumDisplayQueueDepth.load(std::memory_order_relaxed)
                << "lastSequence=" << m_lastDisplaySequence.load(std::memory_order_relaxed)
                << "worstWriteUsec=" << m_worstDisplayWriteUsec.load(std::memory_order_relaxed);
    }
}

void DDJFLX10Controller::discardQueuedDeckPackets(int deck)
{
    const char targetDeck = static_cast<char>(deckByte(deck));
    std::lock_guard lock(m_hidWriteMutex);
    std::erase_if(m_hidWriteQueue, [targetDeck](const QByteArray& queued) {
        return queued.size() == kHidPacketSize && queued.at(0) == targetDeck;
    });
    m_latestHidDisplayPackets.clearDeck(deck);
}

void DDJFLX10Controller::hidWriterLoop()
{
    int consecutiveDroppedPackets = 0;
    while (true) {
        QByteArray packetBytes;
        std::uint64_t displaySequence = 0;
        {
            std::unique_lock lock(m_hidWriteMutex);
            m_hidWriteCondition.wait(lock, [this] {
                return m_hidWriterStopping || !m_latestHidDisplayPackets.empty()
                    || !m_hidWriteQueue.empty();
            });
            if (m_hidWriterStopping)
                return;
            if (auto display = m_latestHidDisplayPackets.takeNext()) {
                packetBytes = std::move(display->bytes);
                displaySequence = display->sequence;
            } else {
                packetBytes = std::move(m_hidWriteQueue.front());
                m_hidWriteQueue.pop_front();
            }
        }

        const auto writeStarted = std::chrono::steady_clock::now();
        int result = LIBUSB_ERROR_OTHER;
        int transferred = 0;
        for (int attempt = 0; attempt <= kHidTransientRetries; ++attempt) {
            transferred = 0;
            result = libusb_interrupt_transfer(
                m_handle, m_outEndpoint,
                reinterpret_cast<unsigned char*>(packetBytes.data()), packetBytes.size(),
                &transferred, kHidTransferTimeoutMs);
            if (result == 0 && transferred == packetBytes.size())
                break;

            const bool transient = result == LIBUSB_ERROR_TIMEOUT
                || result == LIBUSB_ERROR_INTERRUPTED
                || result == LIBUSB_ERROR_BUSY
                || result == LIBUSB_ERROR_IO
                || result == LIBUSB_ERROR_PIPE
                || (result == 0 && transferred != packetBytes.size());
            if (!transient || attempt == kHidTransientRetries)
                break;
            if (result == LIBUSB_ERROR_PIPE)
                libusb_clear_halt(m_handle, m_outEndpoint);
            std::this_thread::sleep_for(std::chrono::milliseconds(2 << attempt));
        }

        if (displaySequence != 0) {
            const auto elapsedUsec = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - writeStarted).count());
            auto worst = m_worstDisplayWriteUsec.load(std::memory_order_relaxed);
            while (elapsedUsec > worst
                   && !m_worstDisplayWriteUsec.compare_exchange_weak(
                       worst, elapsedUsec, std::memory_order_relaxed)) {}
        }

        if (result == 0 && transferred == packetBytes.size()) {
            consecutiveDroppedPackets = 0;
            if (displaySequence != 0) {
                m_displayFramesSent.fetch_add(1, std::memory_order_relaxed);
                m_lastDisplaySequence.store(displaySequence, std::memory_order_relaxed);
            }
            continue;
        }

        if (displaySequence != 0) {
            m_displayWriteFailures.fetch_add(1, std::memory_order_relaxed);
            if (result == LIBUSB_ERROR_TIMEOUT)
                m_displayWriteTimeouts.fetch_add(1, std::memory_order_relaxed);
        }

        const bool transient = result == LIBUSB_ERROR_TIMEOUT
            || result == LIBUSB_ERROR_INTERRUPTED
            || result == LIBUSB_ERROR_BUSY
            || result == LIBUSB_ERROR_IO
            || result == LIBUSB_ERROR_PIPE
            || (result == 0 && transferred != packetBytes.size());
        if (transient) {
            ++consecutiveDroppedPackets;
            m_hidStateRefreshPending.store(true, std::memory_order_release);
            if (consecutiveDroppedPackets == 1
                || (consecutiveDroppedPackets & (consecutiveDroppedPackets - 1)) == 0) {
                qWarning() << "[DDJ-FLX10] transient HID transfer dropped after retries"
                           << result << transferred << "consecutive" << consecutiveDroppedPackets;
            }
            continue;
        }

        m_hidWriteError.store(result, std::memory_order_release);
        m_hidWriteTransferred.store(transferred, std::memory_order_release);
        m_hidWriteHealthy.store(false, std::memory_order_release);
        m_hidWriteFailurePending.store(true, std::memory_order_release);
        std::lock_guard lock(m_hidWriteMutex);
        m_hidWriteQueue.clear();
        m_latestHidDisplayPackets.clear();
        return;
    }
}

void DDJFLX10Controller::reportHidWriteFailure()
{
    if (!m_hidWriteFailurePending.exchange(false, std::memory_order_acq_rel))
        return;

    const int result = m_hidWriteError.load(std::memory_order_acquire);
    const int transferred = m_hidWriteTransferred.load(std::memory_order_acquire);
    qWarning() << "[DDJ-FLX10] HID writer stopped after transfer failure" << result << transferred;
    setStatus(QStringLiteral("DDJ-FLX10: HID transfer failed (%1, %2 bytes); disable and re-enable display support to reconnect")
                  .arg(result)
                  .arg(transferred));
}

#endif
