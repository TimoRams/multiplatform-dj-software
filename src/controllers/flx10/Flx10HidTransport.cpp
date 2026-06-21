#include "DDJFLX10Controller.h"

#if defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX)
#include <libusb-1.0/libusb.h>
#endif

#include <QDebug>
#include <QThread>

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

    int transferred = 0;
    const int result = libusb_interrupt_transfer(m_handle,
                                                 m_outEndpoint,
                                                 reinterpret_cast<unsigned char*>(const_cast<char*>(packetBytes.constData())),
                                                 packetBytes.size(),
                                                 &transferred,
                                                 1000);
    if (result != 0 || transferred != packetBytes.size()) {
        qWarning() << "[DDJ-FLX10] HID write failed" << result << transferred;
        return false;
    }
    return true;
#else
    Q_UNUSED(packetBytes)
    return false;
#endif
}

