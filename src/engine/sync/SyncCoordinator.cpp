#include "SyncCoordinator.h"

#include <algorithm>
#include <cmath>

namespace engine::sync {

bool SyncCoordinator::validDeckIndex(int deckIndex) noexcept
{
    return deckIndex >= 0 && deckIndex < kMaximumDecks;
}

bool SyncCoordinator::registerDeck(int deckIndex, DeckSyncController& controller) noexcept
{
    if (m_shuttingDown || !validDeckIndex(deckIndex) || m_slots[deckIndex].controller)
        return false;
    if (controller.snapshot().deckIndex != deckIndex)
        return false;
    m_slots[deckIndex].controller = &controller;
    ++m_stateGeneration;
    return true;
}

void SyncCoordinator::unregisterDeck(int deckIndex) noexcept
{
    if (!validDeckIndex(deckIndex) || !m_slots[deckIndex].controller)
        return;
    setDeckSyncEnabled(deckIndex, false);
    m_slots[deckIndex] = {};
    ++m_stateGeneration;
    selectMaster();
    distributeCommands();
}

void SyncCoordinator::shutdown() noexcept
{
    m_shuttingDown = true;
    for (int index = 0; index < kMaximumDecks; ++index)
        unregisterDeck(index);
}

void SyncCoordinator::setDeckSyncEnabled(int deckIndex, bool enabled) noexcept
{
    if (m_shuttingDown || !validDeckIndex(deckIndex) || !m_slots[deckIndex].controller)
        return;
    auto& controller = *m_slots[deckIndex].controller;
    if (!controller.setSyncEnabled(enabled))
        return;

    const auto end = m_enableOrder.begin() + m_enableCount;
    const auto found = std::find(m_enableOrder.begin(), end, deckIndex);
    if (enabled && found == end && m_enableCount < kMaximumDecks) {
        m_enableOrder[m_enableCount++] = deckIndex;
    } else if (!enabled && found != end) {
        std::move(std::next(found), end, found);
        m_enableOrder[--m_enableCount] = -1;
    }
    ++m_stateGeneration;
    selectMaster();
    if (enabled && deckIndex != m_masterDeckIndex)
        m_slots[deckIndex].arrangeRequested = true;
    distributeCommands();
}

void SyncCoordinator::requestMaster(int deckIndex, bool requested) noexcept
{
    if (!requested || m_shuttingDown || !validDeckIndex(deckIndex)
        || !m_slots[deckIndex].controller
        || !m_slots[deckIndex].controller->snapshot().syncEnabled)
        return;

    const auto end = m_enableOrder.begin() + m_enableCount;
    const auto found = std::find(m_enableOrder.begin(), end, deckIndex);
    if (found != end)
        std::rotate(m_enableOrder.begin(), found, std::next(found));
    ++m_stateGeneration;
    selectMaster();
    distributeCommands();
}

void SyncCoordinator::requestPhaseArrange(int deckIndex, bool resync) noexcept
{
    if (m_shuttingDown || !validDeckIndex(deckIndex) || !m_slots[deckIndex].controller)
        return;
    m_slots[deckIndex].arrangeRequested = true;
    m_slots[deckIndex].resyncRequested = resync;
    distributeCommands(deckIndex);
}

void SyncCoordinator::updateDeck(int deckIndex, const DeckSyncInputSnapshot& input) noexcept
{
    if (m_shuttingDown || !validDeckIndex(deckIndex) || !m_slots[deckIndex].controller)
        return;
    m_slots[deckIndex].controller->update(input);
    if (deckIndex == m_masterDeckIndex && input.trackGeneration != m_masterTrackGeneration) {
        m_masterTrackGeneration = input.trackGeneration;
        ++m_masterGeneration;
        ++m_stateGeneration;
    }
    distributeCommands(deckIndex);
}

void SyncCoordinator::update() noexcept
{
    if (!m_shuttingDown) {
        selectMaster();
        distributeCommands();
    }
}

void SyncCoordinator::selectMaster() noexcept
{
    int selected = -1;
    for (int i = 0; i < m_enableCount; ++i) {
        const int index = m_enableOrder[i];
        if (validDeckIndex(index) && m_slots[index].controller
            && m_slots[index].controller->snapshot().syncEnabled) {
            selected = index;
            break;
        }
    }
    if (selected == m_masterDeckIndex)
        return;
    m_masterDeckIndex = selected;
    m_masterTrackGeneration = selected >= 0
        ? m_slots[selected].controller->inputSnapshot().trackGeneration : 0;
    ++m_masterGeneration;
    ++m_stateGeneration;
}

void SyncCoordinator::distributeCommands(int onlyDeckIndex) noexcept
{
    DeckSyncInputSnapshot master;
    const bool hasMaster = validDeckIndex(m_masterDeckIndex)
        && m_slots[m_masterDeckIndex].controller;
    if (hasMaster)
        master = m_slots[m_masterDeckIndex].controller->inputSnapshot();

    for (int index = 0; index < kMaximumDecks; ++index) {
        if (onlyDeckIndex >= 0 && index != onlyDeckIndex)
            continue;
        Slot& slot = m_slots[index];
        if (!slot.controller)
            continue;
        const DeckSyncInputSnapshot input = slot.controller->inputSnapshot();
        const bool enabled = slot.controller->snapshot().syncEnabled;
        DeckSyncCommand command;
        command.syncEnabled = enabled;
        command.isMaster = enabled && index == m_masterDeckIndex;
        command.phaseArrangeRequested = slot.arrangeRequested;
        command.resyncRequested = slot.resyncRequested;
        command.tightDoubleSync = m_tightDoubleSync;
        command.targetBpm = hasMaster ? master.effectiveBpm : 0.0;
        command.masterBeatPhase = master.beatPhase;
        command.masterBarPosition = master.barPosition;
        command.masterPositionSeconds = master.audiblePositionSeconds;
        command.masterKeylockLatencySeconds = master.keylockLatencySeconds;
        command.masterPlaying = hasMaster && master.playing;
        command.sameTrack = hasMaster && master.trackIdentity != 0
            && master.trackIdentity == input.trackIdentity;
        command.masterGeneration = m_masterGeneration;
        command.targetTrackGeneration = input.trackGeneration;
        slot.controller->applyCoordinatorCommand(command);
        slot.arrangeRequested = false;
        slot.resyncRequested = false;
    }
}

void SyncCoordinator::setTightDoubleSyncEnabled(bool enabled) noexcept
{
    if (m_tightDoubleSync == enabled)
        return;
    m_tightDoubleSync = enabled;
    ++m_stateGeneration;
    distributeCommands();
}

void SyncCoordinator::setLinkSnapshot(const LinkSyncSnapshot& snapshot) noexcept
{
    m_link = snapshot;
    ++m_stateGeneration;
}

SyncCoordinatorSnapshot SyncCoordinator::snapshot() const noexcept
{
    DeckSyncInputSnapshot master;
    if (validDeckIndex(m_masterDeckIndex) && m_slots[m_masterDeckIndex].controller)
        master = m_slots[m_masterDeckIndex].controller->inputSnapshot();
    return {m_masterDeckIndex, master.effectiveBpm, master.beatPhase, master.barPosition,
            m_link.enabled, m_masterGeneration, m_stateGeneration};
}

std::size_t SyncCoordinator::registeredDeckCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(m_slots.begin(), m_slots.end(),
        [](const Slot& slot) { return slot.controller != nullptr; }));
}

} // namespace engine::sync
