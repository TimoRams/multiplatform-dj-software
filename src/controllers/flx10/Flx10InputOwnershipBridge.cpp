#include "controllers/midi/MidiControllerManager.h"

#include "deck/DjEngine.h"

#include <QDebug>

#include <algorithm>

namespace {

constexpr int kNoteMsgBase = 10000;
constexpr int kChannelStride = 2000;

} // namespace

// Wire id of a note is 10000 + channel * 2000 + note (see handleIncomingMidiMessage).
bool MidiControllerManager::handleFlx10InputSelect(int msgId, float value, bool isNoteOff)
{
    if (msgId < kNoteMsgBase || !shouldUseFlx10Feedback())
        return false;
    const int channel = (msgId - kNoteMsgBase) / kChannelStride;
    const int note = (msgId - kNoteMsgBase) % kChannelStride;
    if (channel < 0 || channel >= static_cast<int>(flx10::kInputChannelCount)
        || note < flx10::kInputSelectNotePcA || note > flx10::kInputSelectNotePcB) {
        return false;
    }

    // Consume all three notes, even the 0x00 companions, so none of them can
    // fall through into a generic mapping.
    if (isNoteOff || value < 0.999f)
        return true;

    const auto event = flx10::decodeInputSelect(
        static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note), 0x7F);
    if (!event)
        return true;
    if (m_flx10InputOwnership.position(event->channel) == event->position)
        return true;

    m_flx10InputOwnership.setPosition(event->channel, event->position);
    if (m_midiTraceEnabled)
        qDebug() << "[FLX10 INPUT SELECT] channel" << channel + 1
                 << "position" << static_cast<int>(event->position);
    applyFlx10InputOwnership();
    return true;
}

void MidiControllerManager::applyFlx10InputOwnership()
{
    const std::array<DjEngine*, flx10::kInputChannelCount> decks {
        m_deckA, m_deckB, m_deckC, m_deckD };
    for (std::size_t channel = 0; channel < decks.size(); ++channel) {
        if (decks[channel])
            decks[channel]->setInputOwnership(
                static_cast<int>(m_flx10InputOwnership.ownership(channel)));
    }
    emit flx10InputOwnershipChanged();
}

void MidiControllerManager::resetFlx10InputOwnership()
{
    m_flx10InputOwnership.reset();
    applyFlx10InputOwnership();
}

void MidiControllerManager::setFlx10HostUsbPort(int port)
{
    const auto resolved = flx10::hostUsbPortFromSetting(
        static_cast<std::uint8_t>(std::clamp(port, 0, 2)));
    if (resolved == m_flx10InputOwnership.hostPort())
        return;
    m_flx10InputOwnership.setHostPort(resolved);
    applyFlx10InputOwnership();
}

int MidiControllerManager::flx10HostUsbPort() const noexcept
{
    return static_cast<int>(m_flx10InputOwnership.hostPort());
}

int MidiControllerManager::flx10InputSelector(int deckIndex) const noexcept
{
    return deckIndex < 0 ? 0
        : static_cast<int>(m_flx10InputOwnership.position(static_cast<std::size_t>(deckIndex)));
}
