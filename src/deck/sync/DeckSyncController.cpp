#include "DeckSyncController.h"

#include <algorithm>
#include <cmath>

namespace engine::sync {

namespace {
bool validPositive(double value) noexcept { return std::isfinite(value) && value > 0.0; }
}

DeckSyncController::DeckSyncController(const Configuration& configuration) noexcept
    : m_deckIndex(configuration.deckIndex)
{
}

bool DeckSyncController::setSyncEnabled(bool enabled) noexcept
{
    if (m_syncEnabled == enabled)
        return false;
    m_syncEnabled = enabled;
    ++m_stateGeneration;
    if (!enabled) {
        m_isMaster = false;
        m_targetBpm = 0.0;
        resetPhaseState(true);
    }
    return true;
}

void DeckSyncController::update(const DeckSyncInputSnapshot& input) noexcept
{
    if (input.trackGeneration != m_input.trackGeneration)
        resetPhaseState(true);
    m_input = input;
    ++m_stateGeneration;
}

void DeckSyncController::resetPhaseCorrection() noexcept
{
    resetPhaseState(true);
    ++m_stateGeneration;
}

double DeckSyncController::wrapPhase(double value) noexcept
{
    if (value > 0.5) value -= 1.0;
    if (value < -0.5) value += 1.0;
    return value;
}

void DeckSyncController::resetPhaseState(bool publishNudge) noexcept
{
    m_resyncBoost = false;
    m_phaseIntegral = 0.0;
    m_phaseTime = {};
    if (m_phaseNudge != 0.0) {
        m_phaseNudge = 0.0;
        if (publishNudge) {
            m_actions.phaseNudgeChanged = true;
            m_actions.phaseNudgePercent = 0.0;
        }
    }
}

void DeckSyncController::publishSeek(double seconds, const DeckSyncCommand& command) noexcept
{
    if (!std::isfinite(seconds))
        return;
    m_actions.seekRequested = true;
    m_actions.seekOffsetSeconds = seconds;
    m_actions.targetTrackGeneration = command.targetTrackGeneration;
    m_actions.masterGeneration = command.masterGeneration;
}

void DeckSyncController::applyCoordinatorCommand(const DeckSyncCommand& command) noexcept
{
    if (command.masterGeneration < m_masterGeneration) {
        m_error = SyncError::StaleMasterGeneration;
        return;
    }
    if (command.targetTrackGeneration != m_input.trackGeneration) {
        m_error = SyncError::StaleTrackGeneration;
        return;
    }

    if (command.masterGeneration != m_masterGeneration) {
        m_masterGeneration = command.masterGeneration;
        resetPhaseState(true);
    }

    m_syncEnabled = command.syncEnabled;
    m_isMaster = command.isMaster && command.syncEnabled;
    m_error = SyncError::None;
    ++m_stateGeneration;

    if (!m_syncEnabled || m_isMaster) {
        m_targetBpm = m_isMaster ? m_input.effectiveBpm : 0.0;
        resetPhaseState(true);
        return;
    }

    if (!m_input.hasTrack) {
        m_error = SyncError::NoTrack;
        resetPhaseState(true);
        return;
    }
    if (!validPositive(m_input.trackBpm) || !validPositive(command.targetBpm)) {
        m_error = SyncError::InvalidBpm;
        resetPhaseState(true);
        return;
    }

    m_targetBpm = command.targetBpm;
    const double targetPercent = std::clamp(((command.targetBpm / m_input.trackBpm) - 1.0) * 100.0,
                                            -100.0, 100.0);
    if (!m_actions.tempoChanged || std::abs(m_actions.targetTempoPercent - targetPercent) > 1.0e-9) {
        m_actions.tempoChanged = true;
        m_actions.targetTempoPercent = targetPercent;
    }

    if (command.phaseArrangeRequested || command.resyncRequested) {
        const double barDiff = wrapPhase(command.masterBarPosition - m_input.barPosition);
        const bool barSeek = std::abs(barDiff) >= 0.002 && validPositive(m_input.beatLengthSeconds);
        if (barSeek)
            publishSeek(barDiff * 4.0 * m_input.beatLengthSeconds, command);
        if (!barSeek && command.tightDoubleSync && command.sameTrack) {
            const double delta = command.masterPositionSeconds - m_input.audiblePositionSeconds
                - (m_input.keylockLatencySeconds - command.masterKeylockLatencySeconds);
            if (std::abs(delta) >= 0.0005)
                publishSeek(std::clamp(delta, -0.020, 0.020), command);
        }
        m_resyncBoost = command.resyncRequested;
        m_phaseIntegral = 0.0;
        m_phaseNudge = 0.0;
        m_actions.phaseNudgeChanged = true;
        m_actions.phaseNudgePercent = 0.0;
        m_phaseTime = {};
    }

    const bool phaseAllowed = command.masterPlaying && m_input.playing
        && !m_input.scratching && !m_input.scratchRelease;
    if (!phaseAllowed) {
        resetPhaseState(true);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (command.tightDoubleSync && command.sameTrack) {
        const double delta = command.masterPositionSeconds - m_input.audiblePositionSeconds
            - (m_input.keylockLatencySeconds - command.masterKeylockLatencySeconds);
        const bool throttlePassed = m_tightAlignTime == std::chrono::steady_clock::time_point{}
            || now - m_tightAlignTime >= std::chrono::milliseconds(100);
        if (std::abs(delta) >= 0.0005 && throttlePassed) {
            publishSeek(std::clamp(delta, -0.020, 0.020), command);
            m_tightAlignTime = now;
            m_phaseIntegral = 0.0;
            m_phaseTime = {};
        }
    }

    const double diff = wrapPhase(command.masterBeatPhase - m_input.beatPhase);
    m_phaseError = diff;
    double dt = 0.004;
    if (m_phaseTime != std::chrono::steady_clock::time_point{})
        dt = std::clamp(std::chrono::duration<double>(now - m_phaseTime).count(), 0.001, 0.05);
    m_phaseTime = now;

    const double maxNudge = m_resyncBoost ? 15.0 : 6.0;
    const double kp = m_resyncBoost ? 30.0 : 14.0;
    constexpr double ki = 9.0;
    m_phaseIntegral = std::clamp(m_phaseIntegral + diff * dt, -maxNudge / ki, maxNudge / ki);
    const double nudge = std::clamp(kp * diff + ki * m_phaseIntegral, -maxNudge, maxNudge);
    if (m_resyncBoost && std::abs(diff) < 0.01)
        m_resyncBoost = false;
    if (std::abs(nudge - m_phaseNudge) > 1.0e-3) {
        m_phaseNudge = nudge;
        m_actions.phaseNudgeChanged = true;
        m_actions.phaseNudgePercent = nudge;
    }
}

DeckSyncSnapshot DeckSyncController::snapshot() const noexcept
{
    return {m_deckIndex, m_syncEnabled, m_isMaster,
            m_syncEnabled && validPositive(m_targetBpm),
            m_syncEnabled && m_input.beatgridValid,
            m_targetBpm, m_phaseError, m_phaseNudge, m_error,
            m_masterGeneration, m_input.trackGeneration, m_stateGeneration};
}

DeckSyncActions DeckSyncController::takeActions() noexcept
{
    const DeckSyncActions result = m_actions;
    m_actions = {};
    return result;
}

} // namespace engine::sync
