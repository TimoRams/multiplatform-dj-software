#pragma once

#include "SyncTypes.h"

#include <chrono>

namespace engine::sync {

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

} // namespace engine::sync
