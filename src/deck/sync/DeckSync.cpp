#include "DeckSync.h"

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

    double diff = wrapPhase(command.masterBeatPhase - m_input.beatPhase);
    // Beat phase is derived from the reader position, which runs ahead of what
    // is actually audible by the time-stretch pipeline latency. The audible
    // phase is therefore readerPhase - latency/beatLength, so the error that
    // matters is
    //     (masterPhase - lat_m/bl) - (followerPhase - lat_f/bl)
    //   = diff + (lat_f - lat_m)/bl
    // This only bites when the two decks run different keylock settings, but
    // without it the loop silently locks to a constant non-zero offset.
    if (validPositive(m_input.beatLengthSeconds)) {
        const double latencyDeltaSeconds =
            m_input.keylockLatencySeconds - command.masterKeylockLatencySeconds;
        diff = wrapPhase(diff + latencyDeltaSeconds / m_input.beatLengthSeconds);
    }
    m_phaseError = diff;
    double dt = 0.008;
    if (m_phaseTime != std::chrono::steady_clock::time_point{})
        dt = std::clamp(std::chrono::duration<double>(now - m_phaseTime).count(), 0.001, 0.05);
    m_phaseTime = now;

    // The controlled quantity is phase but the actuator sets *rate*, and rate
    // is the derivative of phase, so the plant is already an integrator. The
    // closed loop is therefore second order:
    //
    //     d2e/dt2 + kp_eff * de/dt + ki_eff * e = 0
    //     wn = sqrt(ki_eff)     zeta = kp_eff / (2 * sqrt(ki_eff))
    //
    // The nudge is expressed in percent, so kp_eff = kp/100 and ki_eff = ki/100.
    // The historical kp=14 / ki=9 gives zeta = 0.23: badly underdamped. A
    // closed-loop simulation of a tempo move reproduces it as a 43% overshoot
    // ringing with a ~10 s period that needs 30 s to settle -- exactly the
    // reported "beats pull together and drift apart again". The integral gain
    // was the culprit: on an integrating plant it adds a second pole at the
    // origin, and 9 was far too large for the available proportional gain.
    //
    // Transport latency is not the limiting factor here: at a crossover of
    // kp_eff ~ 0.45 rad/s even 50 ms of pipeline delay costs only ~1.3 deg of
    // phase margin. So choose kp as large as the acceptable pitch bend allows
    // (saturating at maxNudge around a seventh of a beat keeps the approach
    // fast) and then pin ki to zeta ~ 1 so the response is critically damped
    // and cannot overshoot.
    const double maxNudge = m_resyncBoost ? 12.0 : 6.0;
    const double kp = m_resyncBoost ? 60.0 : 45.0;
    const double ki = m_resyncBoost ? 9.0 : 5.0;
    // The integral only has to cancel a constant rate bias (beatgrid BPM
    // rounding), so cap its authority well below the proportional term's to
    // keep it from winding up while the output is saturated.
    const double integralLimit = 0.4 * maxNudge / ki;
    const double integralCandidate =
        std::clamp(m_phaseIntegral + diff * dt, -integralLimit, integralLimit);
    // Conditional integration: while the output is already saturated the extra
    // rate cannot be delivered, so accumulating it only stores a correction
    // that has to be paid back later as overshoot. Integrate only when the
    // output is inside its range, or when the new sample pulls it back out of
    // saturation. This is what turns the remaining overshoot into a monotonic
    // approach.
    const double candidateOutput = kp * diff + ki * integralCandidate;
    if (std::abs(candidateOutput) < maxNudge
        || (candidateOutput > 0.0) != (diff > 0.0)) {
        m_phaseIntegral = integralCandidate;
    }
    const double target = std::clamp(kp * diff + ki * m_phaseIntegral, -maxNudge, maxNudge);
    // Purely an artefact guard so a phase jump cannot step the pitch audibly.
    // Set far above the loop bandwidth (wn < 0.3 rad/s) so it never shapes the
    // dynamics -- a slew limit inside the loop would add lag and undo the
    // damping chosen above.
    const double maxStep = 40.0 * dt;
    const double nudge = std::clamp(target, m_phaseNudge - maxStep, m_phaseNudge + maxStep);
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

void SyncCoordinator::stageDeckInput(int deckIndex, const DeckSyncInputSnapshot& input) noexcept
{
    if (m_shuttingDown || !validDeckIndex(deckIndex) || !m_slots[deckIndex].controller)
        return;
    m_slots[deckIndex].controller->update(input);
    if (deckIndex == m_masterDeckIndex && input.trackGeneration != m_masterTrackGeneration) {
        m_masterTrackGeneration = input.trackGeneration;
        ++m_masterGeneration;
        ++m_stateGeneration;
    }
}

void SyncCoordinator::updateDeck(int deckIndex, const DeckSyncInputSnapshot& input) noexcept
{
    stageDeckInput(deckIndex, input);
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
