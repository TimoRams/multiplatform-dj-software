#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace engine {

[[nodiscard]] constexpr bool shouldRunFollowerSyncMaintenance(
    bool syncEnabled,
    bool isSyncMaster,
    bool scrubbing,
    bool releaseGlide) noexcept
{
    return syncEnabled && !isSyncMaster && !scrubbing && !releaseGlide;
}

} // namespace engine

namespace engine::sync {

enum class SyncError : std::uint8_t {
    None,
    NoTrack,
    InvalidBpm,
    StaleMasterGeneration,
    StaleTrackGeneration
};

struct DeckSyncInputSnapshot {
    bool hasTrack = false;
    bool playing = false;
    bool scratching = false;
    bool scratchRelease = false;
    bool reverse = false;
    bool slipEnabled = false;
    bool loopActive = false;
    bool keylockEnabled = false;
    bool beatgridValid = false;
    bool downbeatValid = false;

    double trackBpm = 0.0;
    double effectiveBpm = 0.0;
    double playbackRate = 1.0;
    double audiblePositionSeconds = 0.0;
    double beatPosition = 0.0;
    double barPosition = 0.0;
    double beatPhase = 0.0;
    double beatLengthSeconds = 0.0;
    double keylockLatencySeconds = 0.0;
    double beatConfidence = 0.0;
    double downbeatConfidence = 0.0;

    std::uint64_t trackIdentity = 0;
    std::uint64_t trackGeneration = 0;
    std::uint64_t transportGeneration = 0;
};

struct DeckSyncCommand {
    bool syncEnabled = false;
    bool isMaster = false;
    bool phaseArrangeRequested = false;
    bool resyncRequested = false;
    bool tightDoubleSync = false;

    double targetBpm = 0.0;
    double masterBeatPhase = 0.0;
    double masterBarPosition = 0.0;
    double masterPositionSeconds = 0.0;
    double masterKeylockLatencySeconds = 0.0;
    bool masterPlaying = false;
    bool sameTrack = false;

    std::uint64_t masterGeneration = 0;
    std::uint64_t targetTrackGeneration = 0;
};

struct DeckSyncSnapshot {
    int deckIndex = -1;
    bool syncEnabled = false;
    bool isMaster = false;
    bool tempoSyncAvailable = false;
    bool phaseSyncAvailable = false;
    double targetBpm = 0.0;
    double phaseErrorBeats = 0.0;
    double phaseNudgePercent = 0.0;
    SyncError error = SyncError::None;
    std::uint64_t masterGeneration = 0;
    std::uint64_t trackGeneration = 0;
    std::uint64_t stateGeneration = 0;
};

struct DeckSyncActions {
    bool tempoChanged = false;
    double targetTempoPercent = 0.0;
    bool phaseNudgeChanged = false;
    double phaseNudgePercent = 0.0;
    bool seekRequested = false;
    double seekOffsetSeconds = 0.0;
    std::uint64_t targetTrackGeneration = 0;
    std::uint64_t masterGeneration = 0;
};

struct LinkSyncSnapshot {
    bool enabled = false;
    int numPeers = 0;
    double bpm = 120.0;
    double beat = 0.0;
    double phase = 0.0;
    std::uint64_t generation = 0;
};

struct SyncCoordinatorSnapshot {
    int masterDeckIndex = -1;
    double masterBpm = 0.0;
    double masterBeatPhase = 0.0;
    double masterBarPosition = 0.0;
    bool linkActive = false;
    std::uint64_t masterGeneration = 0;
    std::uint64_t stateGeneration = 0;
};

class DeckSyncController final {
public:
    struct Configuration { int deckIndex = 0; };

    explicit DeckSyncController(const Configuration& configuration) noexcept;

    bool setSyncEnabled(bool enabled) noexcept;
    void update(const DeckSyncInputSnapshot& input) noexcept;
    void applyCoordinatorCommand(const DeckSyncCommand& command) noexcept;
    void resetPhaseCorrection() noexcept;

    [[nodiscard]] DeckSyncInputSnapshot inputSnapshot() const noexcept { return m_input; }
    [[nodiscard]] DeckSyncSnapshot snapshot() const noexcept;
    [[nodiscard]] DeckSyncActions takeActions() noexcept;

private:
    static double wrapPhase(double value) noexcept;
    void resetPhaseState(bool publishNudge) noexcept;
    void publishSeek(double seconds, const DeckSyncCommand& command) noexcept;

    int m_deckIndex = 0;
    bool m_syncEnabled = false;
    bool m_isMaster = false;
    bool m_resyncBoost = false;
    double m_targetBpm = 0.0;
    double m_phaseError = 0.0;
    double m_phaseNudge = 0.0;
    double m_phaseIntegral = 0.0;
    SyncError m_error = SyncError::None;
    std::uint64_t m_masterGeneration = 0;
    std::uint64_t m_stateGeneration = 0;
    DeckSyncInputSnapshot m_input;
    DeckSyncActions m_actions;
    std::chrono::steady_clock::time_point m_phaseTime {};
    std::chrono::steady_clock::time_point m_tightAlignTime {};
};

class SyncCoordinator final {
public:
    static constexpr int kMaximumDecks = 4;

    bool registerDeck(int deckIndex, DeckSyncController& controller) noexcept;
    void unregisterDeck(int deckIndex) noexcept;
    void shutdown() noexcept;

    void setDeckSyncEnabled(int deckIndex, bool enabled) noexcept;
    void requestMaster(int deckIndex, bool requested) noexcept;
    void requestPhaseArrange(int deckIndex, bool resync = false) noexcept;
    void stageDeckInput(int deckIndex, const DeckSyncInputSnapshot& input) noexcept;
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
