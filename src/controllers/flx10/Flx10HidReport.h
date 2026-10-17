#pragma once

#include <QByteArrayView>

#include <array>
#include <cstdint>
#include <optional>

namespace flx10_protocol {

// Interface 5 declares an unnumbered 128-byte output report. Its first byte
// (0x10/0x20 for the native decks) is payload, not a HID report ID.
inline bool isScreenReportDescriptor(QByteArrayView descriptor)
{
    struct Globals {
        std::uint32_t size = 0;
        std::uint32_t count = 0;
        std::uint32_t id = 0;
    } globals;
    std::array<Globals, 16> stack;
    std::size_t depth = 0;
    std::uint64_t outputBits = 0;
    for (qsizetype offset = 0; offset < descriptor.size();) {
        const auto prefix = static_cast<std::uint8_t>(descriptor[offset++]);
        // Unknown long items cannot establish a known-safe report layout.
        if (prefix == 0xfe)
            return false;
        const int sizeCode = prefix & 3;
        const int size = sizeCode == 3 ? 4 : sizeCode;
        if (descriptor.size() - offset < size)
            return false;
        std::uint32_t value = 0;
        for (int byte = 0; byte < size; ++byte)
            value |= static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(descriptor[offset++])) << (8 * byte);
        const int type = (prefix >> 2) & 3;
        const int tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 7: globals.size = value; break;
            case 8:
                if (value == 0 || value > 255)
                    return false;
                globals.id = value;
                break;
            case 9: globals.count = value; break;
            case 10:
                if (depth == stack.size())
                    return false;
                stack[depth++] = globals;
                break;
            case 11:
                if (depth == 0)
                    return false;
                globals = stack[--depth];
                break;
            default: break;
            }
        } else if (type == 0 && tag == 9) {
            if (globals.id != 0 || globals.size == 0 || globals.count == 0)
                return false;
            outputBits += static_cast<std::uint64_t>(globals.size) * globals.count;
            if (outputBits > 128 * 8)
                return false;
        }
    }
    return depth == 0 && outputBits == 128 * 8;
}

inline bool isScreenHidInterface(int interfaceNumber, QByteArrayView descriptor)
{
    return interfaceNumber == 5 && isScreenReportDescriptor(descriptor);
}

struct NativeScreenReport {
    int id = 0;
    QByteArrayView bytes;
};

inline std::optional<NativeScreenReport> nativeScreenReport(QByteArrayView packet)
{
    if (packet.size() != 128)
        return std::nullopt;
    // Unlike Linux hidraw, IOHID takes ID separately and needs no zero prefix.
    return NativeScreenReport {0, packet};
}

} // namespace flx10_protocol
