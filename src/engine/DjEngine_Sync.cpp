#include "DjEngineCommonIncludes.h"



std::mutex DjEngine::s_syncMutex;
std::vector<DjEngine*> DjEngine::s_syncDecks;
DjEngine* DjEngine::s_syncMasterDeck = nullptr;

namespace {

constexpr int kBeatsPerBar = 4;

[[nodiscard]] double wrapUnitPhase(double diff) noexcept
{
    if (diff > 0.5)  diff -= 1.0;
    if (diff < -0.5) diff += 1.0;
    return diff;
}

} // namespace

static double nearestDownbeatAnchor(const std::vector<TrackData::BeatMarker>& grid,
                                    double currentSec)
{
    if (grid.empty())
        return currentSec;

    double best = currentSec;
    double bestDist = std::numeric_limits<double>::max();
    for (const auto& beat : grid) {
        if (!beat.isDownbeat)
            continue;
        const double dist = std::abs(beat.positionSec - currentSec);
        if (dist < bestDist) {
            bestDist = dist;
            best = beat.positionSec;
        }
    }
    return best;
}

void DjEngine::updateSyncMasterLocked()
{
    DjEngine* newMaster = nullptr;
    for (auto* d : s_syncDecks) {
        if (d && d->m_syncEnabled) {
            newMaster = d;
            break;
        }
    }

    s_syncMasterDeck = newMaster;
    for (auto* d : s_syncDecks) {
        if (!d)
            continue;
        const bool wasMaster = d->m_isSyncMaster;
        d->m_isSyncMaster = (d == s_syncMasterDeck) && d->m_syncEnabled;
        if (wasMaster != d->m_isSyncMaster)
            emit d->syncMasterChanged();
    }
}


void DjEngine::propagateMasterTempoLocked(DjEngine* master)
{
    if (!master || !master->m_trackData)
        return;

    double masterBpm = 0.0;
    std::vector<DjEngine*> followers;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        masterBpm = master->getCurrentBpm();
        if (masterBpm <= 0.0)
            return;

        auto followerView = s_syncDecks
            | std::views::filter([master](DjEngine* d) {
                  return d && d != master && d->m_syncEnabled && !d->m_isSyncMaster;
              });
        followers.assign(followerView.begin(), followerView.end());
    }

    for (auto* d : followers) {
        if (!d->m_trackData)
            continue;
        const double baseBpm = d->m_trackData->getBpm();
        if (baseBpm <= 0.0)
            continue;
        const double pct = ((masterBpm / baseBpm) - 1.0) * 100.0;
        d->applyTempoPercent(pct);
    }
}


double DjEngine::getBeatPhase() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0) return 0.0;

    const double pos = getPosition();
    if (m_trackData->getBeatGrid().size() >= 2) {
        const auto [prevSec, beatLen] = beatIntervalAt(pos);
        return std::clamp((pos - prevSec) / beatLen, 0.0, 0.9999);
    }

    // No usable beat grid — phase from BPM alone.
    return std::fmod(pos / (60.0 / bpm), 1.0);
}


double DjEngine::getBarPhase() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0) return 0.0;

    constexpr int kBeatsPerBar = 4;
    const auto& grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        // Fractional beat index from grid[0], then fold into a bar anchored to the
        // first downbeat marker so two decks line up on the musical "1".
        const double beatPos = getBeatPosition();
        int downbeatOffset = 0;
        if (const auto it = std::ranges::find_if(grid, &TrackData::BeatMarker::isDownbeat);
            it != grid.end()) {
            downbeatOffset = static_cast<int>(std::distance(grid.begin(), it) % kBeatsPerBar);
        }
        const double rel = beatPos - static_cast<double>(downbeatOffset);
        const double barPos = std::fmod(std::fmod(rel, kBeatsPerBar) + kBeatsPerBar, kBeatsPerBar);
        return barPos / static_cast<double>(kBeatsPerBar);
    }

    // No usable grid — measure bars from the first beat using BPM alone.
    const double sr        = m_trackData->getSampleRate();
    const double firstBeat = sr > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sr : 0.0;
    const double barLen = static_cast<double>(kBeatsPerBar) * 60.0 / bpm;
    const double rel    = getPosition() - firstBeat;
    return std::fmod(std::fmod(rel, barLen) + barLen, barLen) / barLen;
}


double DjEngine::getBeatPosition() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0) return 0.0;

    const double pos  = getPosition();
    const auto&  grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        const auto it = std::upper_bound(grid.begin(), grid.end(), pos,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        const auto prev      = (it != grid.begin()) ? std::prev(it) : grid.begin();
        const int  beatIndex = static_cast<int>(std::distance(grid.begin(), prev));
        // Use the same interval length as beatIntervalAt (> 0.01 threshold),
        // except fall back to max(0.001, …) at the very first marker so
        // pre-roll positions before the first beat return sensible fractions.
        double beatLen = 60.0 / bpm;
        if (std::next(prev) != grid.end()) {
            const double candidate = std::next(prev)->positionSec - prev->positionSec;
            if (candidate > 0.001)
                beatLen = candidate;
        }
        return static_cast<double>(beatIndex) + ((pos - prev->positionSec) / beatLen);
    }

    const double sr        = m_trackData->getSampleRate();
    const double firstBeat = sr > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sr : 0.0;
    return (pos - firstBeat) / (60.0 / bpm);
}


void DjEngine::updatePhaseCorrection()
{
    // Only sync followers with a valid, playing track should phase-correct.
    if (!m_syncEnabled || m_isSyncMaster || !isPlaying() || !m_trackData) {
        m_phaseIntegral = 0.0;
        m_phaseClock.invalidate();
        if (m_phaseNudge != 0.0) {
            m_phaseNudge = 0.0;
            updateSpeedAndPitch();
        }
        return;
    }

    DjEngine* master = currentSyncMaster();

    if (!master || !master->isPlaying() || !master->m_trackData
            || master->m_trackData->getBpm() <= 0.0) {
        m_phaseIntegral = 0.0;
        m_phaseClock.invalidate();
        if (m_phaseNudge != 0.0) {
            m_phaseNudge = 0.0;
            updateSpeedAndPitch();
        }
        return;
    }

    const double masterPhase = master->getBeatPhase();
    const double myPhase     = getBeatPhase();
    const double diff        = wrapUnitPhase(masterPhase - myPhase);

    // Elapsed time since the last correction (for the integral term).
    const double dt = m_phaseClock.isValid()
        ? std::clamp(static_cast<double>(m_phaseClock.nsecsElapsed()) * 1e-9, 0.001, 0.05)
        : 0.004;
    m_phaseClock.restart();

    // PI controller on beat-phase error. The proportional term snaps the phase;
    // the integral term accumulates residual error and trims the baseline tempo so
    // any systematic mismatch (analysed BPM vs true grid spacing) is cancelled and
    // the decks stop drifting apart. Without the integral a pure-P loop leaves a
    // steady tempo bias that slowly walks the phase out (the reported drift).
    const double kMaxNudge = m_resyncBoost ? 15.0 : 6.0;
    const double kP        = m_resyncBoost ? 30.0 : 14.0;  // % per beat of error
    constexpr double kI    = 9.0;                            // % per (beat·second)

    m_phaseIntegral += diff * dt;
    // Anti-windup: never let the integral alone exceed the nudge ceiling.
    const double integralClamp = kMaxNudge / kI;
    m_phaseIntegral = std::clamp(m_phaseIntegral, -integralClamp, integralClamp);

    const double newNudge = std::clamp(kP * diff + kI * m_phaseIntegral,
                                       -kMaxNudge, kMaxNudge);

    // Clear the reSync boost once the phase is essentially locked.
    if (m_resyncBoost && std::abs(diff) < 0.01)
        m_resyncBoost = false;

    // Apply only on a meaningful change to avoid redundant time-stretch updates.
    if (std::abs(newNudge - m_phaseNudge) > 1e-3) {
        m_phaseNudge = newNudge;
        updateSpeedAndPitch();
    }
}


DjEngine* DjEngine::currentSyncMaster()
{
    std::lock_guard<std::mutex> g(s_syncMutex);
    return s_syncMasterDeck;
}


void DjEngine::applySyncSeekOffset(double seekOffset)
{
    const double len    = transportSource.getLengthInSeconds();
    const double newPos = std::clamp(getPosition() + seekOffset, -PRE_ROLL_SECONDS,
                                     len > 0.0 ? len : PRE_ROLL_SECONDS);
    if (newPos < 0.0) {
        if (m_preRollCountdownActive) {
            m_preRollVisualStartPos = newPos;
            m_preRollClock.restart();
        }
        m_scrubHoldPosition = newPos;
        m_atomicPlayheadPos.store(newPos, std::memory_order_relaxed);
    } else {
        transportSource.setPosition(newPos);
        armSnapFromTransportPosition();
    }
}


void DjEngine::alignToSyncMasterOnPlay()
{
    if (!m_syncEnabled || m_isSyncMaster)
        return;

    DjEngine* master = currentSyncMaster();
    if (!master || master == this || !master->isPlaying() || !master->m_trackData
            || !m_trackData)
        return;

    // The master may have changed (or its tempo moved) while we were paused —
    // re-derive our tempo, then arrange bars so we drop in phase-locked.
    const double masterBpm = master->getCurrentBpm();
    const double baseBpm   = m_trackData->getBpm();
    if (masterBpm > 0.0 && baseBpm > 0.0)
        applyTempoPercent(((masterBpm / baseBpm) - 1.0) * 100.0);

    snapPhaseToMaster(master);
}


void DjEngine::snapPhaseToMaster(DjEngine* master)
{
    if (!master || !master->m_trackData || !m_trackData)
        return;

    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0)
        return;

    // Align on the BAR (downbeat), not just the sub-beat phase: Serato-style sync
    // arranges the beatgrids so the musical "1"s line up. Error is measured in bars
    // and wrapped to ±0.5 bar (±2 beats), so the largest arrange jump is 2 beats.
    const double diff = wrapUnitPhase(master->getBarPhase() - getBarPhase());

    if (std::abs(diff) < 0.002)
        return;

    applySyncSeekOffset(diff * 4.0 * beatIntervalAt(getPosition()).lengthSec);
    m_phaseNudge    = 0.0;
    m_phaseIntegral = 0.0;
    m_phaseClock.invalidate();
}


void DjEngine::setSyncEnabled(bool enabled)
{
    if (m_syncEnabled == enabled)
        return;

    m_syncEnabled = enabled;

    if (!enabled) {
        m_resyncBoost   = false;
        m_phaseIntegral = 0.0;
        m_phaseClock.invalidate();
        if (m_phaseNudge != 0.0) {
            m_phaseNudge = 0.0;
            updateSpeedAndPitch();
        }
    }

    emit syncChanged();

    bool amMaster = false;
    DjEngine* masterDeck = nullptr;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        updateSyncMasterLocked();
        amMaster = m_isSyncMaster;
        masterDeck = s_syncMasterDeck;
    }

    if (m_syncEnabled) {
        if (amMaster) {
            propagateMasterTempoLocked(this);
        } else if (masterDeck) {
            const double masterBpm = masterDeck->getCurrentBpm();
            if (m_trackData) {
                const double baseBpm = m_trackData->getBpm();
                if (masterBpm > 0.0 && baseBpm > 0.0) {
                    const double pct = ((masterBpm / baseBpm) - 1.0) * 100.0;
                    applyTempoPercent(pct);
                }
            }
            snapPhaseToMaster(masterDeck);
            return;
        }
    }
}


void DjEngine::reSync()
{
    if (!m_syncEnabled || m_isSyncMaster || !m_trackData)
        return;

    DjEngine* master = currentSyncMaster();
    if (!master || !master->m_trackData)
        return;

    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0)
        return;

    const double diff = wrapUnitPhase(master->getBarPhase() - getBarPhase());

    if (std::abs(diff) >= 0.002)
        applySyncSeekOffset(diff * 4.0 * beatIntervalAt(getPosition()).lengthSec);
    m_phaseNudge    = 0.0;
    m_phaseIntegral = 0.0;
    m_phaseClock.invalidate();
    m_resyncBoost   = true;
    updatePhaseCorrection();
}

