#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// Which computer owns a DDJ-FLX10 channel. The controller's INPUT SELECT
// switch reports PC-A / LINE-PHONO / PC-B; the official MIDI message list
// (DDJ-FLX10_MIDI_Message_List_E1, "M17 INPUT SELECT") does not define any
// message telling a computer which of the two USB ports it is plugged into.
// The port therefore comes from user configuration and stays Unknown until it
// is set, in which case a PC-A/PC-B position is never resolved either way.
namespace flx10 {

enum class InputSelectorPosition : std::uint8_t { Unknown, PcA, Line, PcB };
enum class HostUsbPort : std::uint8_t { Unknown, A, B };
enum class InputOwnership : std::uint8_t { Unknown, Local, External };

constexpr std::size_t kInputChannelCount = 4;

// INPUT SELECT is three notes per channel (status 9n, n = channel 0..3);
// exactly one of them carries 0x7F for the chosen position.
constexpr std::uint8_t kInputSelectNotePcA = 0x55;
constexpr std::uint8_t kInputSelectNoteLine = 0x56;
constexpr std::uint8_t kInputSelectNotePcB = 0x57;

struct InputSelectEvent {
    std::size_t channel = 0;
    InputSelectorPosition position = InputSelectorPosition::Unknown;
};

// Only the 0x7F note names the new position. The 0x00 companions arrive for
// the positions being left and carry no information of their own.
[[nodiscard]] inline std::optional<InputSelectEvent> decodeInputSelect(
    std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept
{
    if ((status & 0xF0) != 0x90 || data2 != 0x7F)
        return std::nullopt;
    const std::size_t channel = status & 0x0F;
    if (channel >= kInputChannelCount)
        return std::nullopt;
    switch (data1) {
    case kInputSelectNotePcA: return InputSelectEvent{channel, InputSelectorPosition::PcA};
    case kInputSelectNoteLine: return InputSelectEvent{channel, InputSelectorPosition::Line};
    case kInputSelectNotePcB: return InputSelectEvent{channel, InputSelectorPosition::PcB};
    default: return std::nullopt;
    }
}

[[nodiscard]] constexpr InputOwnership resolveInputOwnership(
    InputSelectorPosition position, HostUsbPort hostPort) noexcept
{
    switch (position) {
    case InputSelectorPosition::Line:
        return InputOwnership::External;
    case InputSelectorPosition::PcA:
        return hostPort == HostUsbPort::Unknown ? InputOwnership::Unknown
            : hostPort == HostUsbPort::A ? InputOwnership::Local : InputOwnership::External;
    case InputSelectorPosition::PcB:
        return hostPort == HostUsbPort::Unknown ? InputOwnership::Unknown
            : hostPort == HostUsbPort::B ? InputOwnership::Local : InputOwnership::External;
    case InputSelectorPosition::Unknown:
        break;
    }
    return InputOwnership::Unknown;
}

[[nodiscard]] inline HostUsbPort hostUsbPortFromSetting(std::uint8_t value) noexcept
{
    return value == 1 ? HostUsbPort::A : value == 2 ? HostUsbPort::B : HostUsbPort::Unknown;
}

// Selector positions are only learned from reports (the controller has no
// documented query), so every channel starts Unknown and returns to Unknown
// when the controller goes away.
class InputOwnershipTracker {
public:
    void setHostPort(HostUsbPort port) noexcept { m_hostPort = port; }
    [[nodiscard]] HostUsbPort hostPort() const noexcept { return m_hostPort; }

    void reset() noexcept { m_positions.fill(InputSelectorPosition::Unknown); }

    void setPosition(std::size_t channel, InputSelectorPosition position) noexcept
    {
        if (channel < m_positions.size())
            m_positions[channel] = position;
    }

    [[nodiscard]] InputSelectorPosition position(std::size_t channel) const noexcept
    {
        return channel < m_positions.size() ? m_positions[channel]
                                            : InputSelectorPosition::Unknown;
    }

    [[nodiscard]] InputOwnership ownership(std::size_t channel) const noexcept
    {
        return resolveInputOwnership(position(channel), m_hostPort);
    }

private:
    std::array<InputSelectorPosition, kInputChannelCount> m_positions {};
    HostUsbPort m_hostPort = HostUsbPort::Unknown;
};

} // namespace flx10
