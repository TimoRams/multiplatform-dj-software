#include "Flx10MacHidTransport.h"

#include "Flx10HidReport.h"
#include "Flx10Protocol.h"

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/USBSpec.h>

#include <chrono>
#include <vector>

namespace {

int number(CFTypeRef property)
{
    int value = -1;
    if (property && CFGetTypeID(property) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberIntType, &value);
    return value;
}

int interfaceNumber(IOHIDDeviceRef device)
{
    const int direct = number(IOHIDDeviceGetProperty(device, CFSTR(kUSBInterfaceNumber)));
    if (direct >= 0)
        return direct;

    // Since macOS 13.3 the interface number may live only on the USB parent.
    io_registry_entry_t entry = IOHIDDeviceGetService(device);
    if (!entry)
        return -1;
    IOObjectRetain(entry);
    int result = -1;
    for (int depth = 0; depth < 8; ++depth) {
        auto property = IORegistryEntryCreateCFProperty(
            entry, CFSTR(kUSBInterfaceNumber), kCFAllocatorDefault, 0);
        result = number(property);
        if (property)
            CFRelease(property);
        if (result >= 0 || IOObjectConformsTo(entry, "IOUSBHostDevice")
            || IOObjectConformsTo(entry, "IOUSBDevice"))
            break;
        io_registry_entry_t parent = IO_OBJECT_NULL;
        if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) != KERN_SUCCESS)
            break;
        IOObjectRelease(entry);
        entry = parent;
    }
    IOObjectRelease(entry);
    return result;
}

bool screenInterface(IOHIDDeviceRef device)
{
    auto property = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDReportDescriptorKey));
    if (!property || CFGetTypeID(property) != CFDataGetTypeID())
        return false;
    auto data = static_cast<CFDataRef>(property);
    return flx10_protocol::isScreenHidInterface(interfaceNumber(device), QByteArrayView(
        reinterpret_cast<const char*>(CFDataGetBytePtr(data)), CFDataGetLength(data)));
}

} // namespace

struct Flx10MacHidTransport::Impl {
    IOHIDDeviceRef device = nullptr;
    CFRunLoopRef runLoop = nullptr;
    // Kept until close even if a driver fails to deliver its completion.
    QByteArray inFlight;
    bool completed = false;
    bool removed = false;
    bool expired = false;
    bool opened = false;
    IOReturn result = kIOReturnSuccess;

    static void completion(void* context, IOReturn result, void*, IOHIDReportType,
                           uint32_t, uint8_t*, CFIndex)
    {
        auto& self = *static_cast<Impl*>(context);
        self.result = result;
        self.completed = true;
    }

    static void removal(void* context, IOReturn, void*)
    {
        static_cast<Impl*>(context)->removed = true;
    }
};

Flx10MacHidTransport::Flx10MacHidTransport() : m_impl(std::make_unique<Impl>()) {}
Flx10MacHidTransport::~Flx10MacHidTransport() { close(); }

bool Flx10MacHidTransport::open(QString& error)
{
    close();
    auto manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) {
        error = QStringLiteral("DDJ-FLX10: cannot create macOS IOHID manager");
        return false;
    }
    int vendor = flx10_protocol::kVid;
    int product = flx10_protocol::kPid;
    auto vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
    auto pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product);
    const void* keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)};
    const void* values[] = {vid, pid};
    auto matching = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                       &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(matching);
    CFRelease(vid);
    CFRelease(pid);
    // Do not open the manager: that would open every matching HID interface.
    auto devices = IOHIDManagerCopyDevices(manager);
    int matches = 0;
    if (devices) {
        std::vector<const void*> entries(static_cast<std::size_t>(CFSetGetCount(devices)));
        CFSetGetValues(devices, entries.data());
        for (auto entry : entries) {
            auto device = static_cast<IOHIDDeviceRef>(const_cast<void*>(entry));
            if (!screenInterface(device))
                continue;
            ++matches;
            if (!m_impl->device) {
                m_impl->device = device;
                CFRetain(device);
            }
        }
        CFRelease(devices);
    }
    CFRelease(manager);
    if (matches != 1) {
        error = matches > 1
            ? QStringLiteral("DDJ-FLX10: multiple screen interfaces found; connect only one FLX10 and Refresh")
            : QStringLiteral("DDJ-FLX10: no compatible macOS HID screen (2B73:0041, interface 5, unnumbered 128-byte output). Reconnect the controller, close other DJ apps, then Refresh. If the vendor driver hides HID, this driver configuration is unsupported; audio/MIDI are left untouched.");
        close();
        return false;
    }
    const IOReturn result = IOHIDDeviceOpen(m_impl->device, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        error = QStringLiteral("DDJ-FLX10: shared HID open failed (0x%1). Close other DJ apps, reconnect and Refresh; check macOS Privacy & Security permissions if access is denied. The audio/MIDI driver will not be detached.")
                    .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
        close();
        return false;
    }
    m_impl->opened = true;
    m_impl->removed = false;
    m_impl->expired = false;
    return true;
}

void Flx10MacHidTransport::close() noexcept
{
    // The owner must join the writer (and hence unschedule) before closing.
    if (m_impl->device) {
        IOHIDDeviceRegisterRemovalCallback(m_impl->device, nullptr, nullptr);
        if (m_impl->opened)
            IOHIDDeviceClose(m_impl->device, kIOHIDOptionsTypeNone);
        CFRelease(m_impl->device);
        m_impl->device = nullptr;
    }
    m_impl->opened = false;
    m_impl->inFlight.clear();
}

void Flx10MacHidTransport::beginWrites()
{
    if (!m_impl->device || !m_impl->opened)
        return;
    m_impl->runLoop = CFRunLoopGetCurrent();
    CFRetain(m_impl->runLoop);
    IOHIDDeviceRegisterRemovalCallback(m_impl->device, Impl::removal, m_impl.get());
    IOHIDDeviceScheduleWithRunLoop(m_impl->device, m_impl->runLoop, CFSTR("BrockDJFLX10"));
}

void Flx10MacHidTransport::endWrites() noexcept
{
    if (m_impl->runLoop) {
        IOHIDDeviceUnscheduleFromRunLoop(m_impl->device, m_impl->runLoop, CFSTR("BrockDJFLX10"));
        CFRelease(m_impl->runLoop);
        m_impl->runLoop = nullptr;
    }
}

int Flx10MacHidTransport::write(const QByteArray& packet, int& transferred)
{
    transferred = 0;
    if (!m_impl->device || m_impl->removed)
        return kIOReturnNoDevice;
    if (!m_impl->runLoop)
        return kIOReturnNotOpen;
    if (m_impl->expired)
        return kIOReturnNotResponding;
    const auto report = flx10_protocol::nativeScreenReport(packet);
    if (!report)
        return kIOReturnBadArgument;
    m_impl->inFlight = packet;
    m_impl->completed = false;
    const auto result = IOHIDDeviceSetReportWithCallback(
        m_impl->device, kIOHIDReportTypeOutput, report->id,
        reinterpret_cast<const uint8_t*>(m_impl->inFlight.constData()),
        report->bytes.size(), flx10_protocol::kHidTransferTimeoutMs,
        Impl::completion, m_impl.get());
    if (result != kIOReturnSuccess)
        return result;

    // The API's timeout is milliseconds, unlike CFRunLoop's seconds. A driver
    // that never completes must not hold up join or allow a buffer to be reused.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(flx10_protocol::kHidTransferTimeoutMs + 25);
    while (!m_impl->completed && !m_impl->removed
           && std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode(CFSTR("BrockDJFLX10"), 0.001, true);
    if (!m_impl->completed) {
        m_impl->expired = true;
        return m_impl->removed ? kIOReturnNoDevice : kIOReturnNotResponding;
    }
    if (m_impl->result == kIOReturnSuccess)
        transferred = static_cast<int>(packet.size());
    return m_impl->result;
}
