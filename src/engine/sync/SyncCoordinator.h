#pragma once

#include "DeckSyncController.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::sync {

class SyncCoordinator final {
public:
    static constexpr int kMaximumDecks = 4;

    bool registerDeck(int deckIndex, DeckSyncController& controller) noexcept;
    void unregisterDeck(int deckIndex) noexcept;
    void shutdown() noexcept;

    void setDeckSyncEnabled(int deckIndex, bool enabled) noexcept;
    void requestMaster(int deckIndex, bool requested) noexcept;
    void requestPhaseArrange(int deckIndex, bool resync = false) noexcept;
    void updateDeck(int deckIndex, const DeckSyncInputSnapshot& input) noexcept;
    void update() noexcept;

    void setTightDoubleSyncEnabled(bool enabled) noexcept;
    [[nodiscard]] bool tightDoubleSyncEnabled() const noexcept { return m_tightDoubleSync; }
    void setLinkSnapshot(const LinkSyncSnapshot& snapshot) noexcept;

    [[nodiscard]] SyncCoordinatorSnapshot snapshot() const noexcept;
    [[nodiscard]] std::size_t registeredDeckCount() const noexcept;

private:
    struct Slot {
        DeckSyncController* controller = nullptr;
        bool arrangeRequested = false;
        bool resyncRequested = false;
    };

    [[nodiscard]] static bool validDeckIndex(int deckIndex) noexcept;
    void selectMaster() noexcept;
    void distributeCommands(int onlyDeckIndex = -1) noexcept;

    std::array<Slot, kMaximumDecks> m_slots {};
    std::array<int, kMaximumDecks> m_enableOrder {-1, -1, -1, -1};
    int m_enableCount = 0;
    int m_masterDeckIndex = -1;
    std::uint64_t m_masterTrackGeneration = 0;
    std::uint64_t m_masterGeneration = 0;
    std::uint64_t m_stateGeneration = 0;
    bool m_tightDoubleSync = false;
    bool m_shuttingDown = false;
    LinkSyncSnapshot m_link;
};

} // namespace engine::sync
