#include "DDJFLX10Controller.h"

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
#include <libusb-1.0/libusb.h>
#endif

#include <QDebug>
#include <QThread>

#include <algorithm>

#include "Flx10ProtocolCommon.h"

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

    // 0x27 carries only the current jog-display state. Keeping an older one in
    // the queue wastes endpoint time and can make a busy display look jumpy.
    if (packetBytes.at(1) == char(0x27)) {
        const auto pending = std::find_if(m_hidWriteQueue.rbegin(), m_hidWriteQueue.rend(),
                                          [&packetBytes](const QByteArray& queued) {
            return queued.size() == kHidPacketSize
                && queued.at(0) == packetBytes.at(0)
                && queued.at(1) == char(0x27);
        });
        if (pending != m_hidWriteQueue.rend()) {
            *pending = packetBytes;
            return true;
        }
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
        m_hidWriterStopping = false;
        m_hidWriterRunning = true;
        m_hidWriteHealthy.store(true, std::memory_order_release);
        m_hidWriteFailurePending.store(false, std::memory_order_release);
        m_hidWriteError.store(0, std::memory_order_release);
        m_hidWriteTransferred.store(0, std::memory_order_release);
    }
    m_hidWriter = std::thread([this] { hidWriterLoop(); });
}

void DDJFLX10Controller::stopHidWriter() noexcept
{
    {
        std::lock_guard lock(m_hidWriteMutex);
        m_hidWriterStopping = true;
        m_hidWriteQueue.clear();
    }
    m_hidWriteCondition.notify_all();
    if (m_hidWriter.joinable())
        m_hidWriter.join();
    {
        std::lock_guard lock(m_hidWriteMutex);
        m_hidWriterRunning = false;
        m_hidWriterStopping = false;
    }
}

void DDJFLX10Controller::hidWriterLoop()
{
    while (true) {
        QByteArray packetBytes;
        {
            std::unique_lock lock(m_hidWriteMutex);
            m_hidWriteCondition.wait(lock, [this] {
                return m_hidWriterStopping || !m_hidWriteQueue.empty();
            });
            if (m_hidWriterStopping)
                return;
            packetBytes = std::move(m_hidWriteQueue.front());
            m_hidWriteQueue.pop_front();
        }

        int transferred = 0;
        const int result = libusb_interrupt_transfer(
            m_handle, m_outEndpoint,
            reinterpret_cast<unsigned char*>(packetBytes.data()), packetBytes.size(),
            &transferred, 1000);
        if (result == 0 && transferred == packetBytes.size())
            continue;

        m_hidWriteError.store(result, std::memory_order_release);
        m_hidWriteTransferred.store(transferred, std::memory_order_release);
        m_hidWriteHealthy.store(false, std::memory_order_release);
        m_hidWriteFailurePending.store(true, std::memory_order_release);
        std::lock_guard lock(m_hidWriteMutex);
        m_hidWriteQueue.clear();
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
