#include "DjEngineCommonIncludes.h"



std::mutex DjEngine::s_syncMutex;
std::vector<DjEngine*> DjEngine::s_syncDecks;
DjEngine* DjEngine::s_syncMasterDeck = nullptr;

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
        if (m_phaseNudge != 0.0) {
            m_phaseNudge = 0.0;
            updateSpeedAndPitch();
        }
        return;
    }

    DjEngine* master = nullptr;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        master = s_syncMasterDeck;
    }

    if (!master || !master->isPlaying() || !master->m_trackData
            || master->m_trackData->getBpm() <= 0.0) {
        if (m_phaseNudge != 0.0) {
            m_phaseNudge = 0.0;
            updateSpeedAndPitch();
        }
        return;
    }

    const double masterPhase = master->getBeatPhase();
    const double myPhase     = getBeatPhase();

    // Signed phase error wrapped to [-0.5, +0.5] of a beat.
    double diff = masterPhase - myPhase;
    if (diff >  0.5) diff -= 1.0;
    if (diff < -0.5) diff += 1.0;

    constexpr double kTolerance = 0.02;
    // Normal: ±4% nudge. Boosted (reSync()): ±15% — after the 85% seek the remaining
    // 15% of error converges in ~2 s (τ = 50 * beatLen / kMaxNudge ≈ 1.6 s at 128 BPM).
    const double kMaxNudge = m_resyncBoost ? 15.0 : 4.0;
    const double kGain     = kMaxNudge / 0.5;

    const double newNudge = (std::abs(diff) < kTolerance)
        ? 0.0
        : std::clamp(diff * kGain, -kMaxNudge, kMaxNudge);

    // Clear boost once we're within tolerance.
    if (m_resyncBoost && std::abs(diff) < kTolerance)
        m_resyncBoost = false;

    if (newNudge != m_phaseNudge) {
        m_phaseNudge = newNudge;
        updateSpeedAndPitch();
    }
}


void DjEngine::snapPhaseToMaster(DjEngine* master)
{
    if (!master || !master->m_trackData || !m_trackData)
        return;

    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0)
        return;

    const double masterPhase = master->getBeatPhase();
    const double myPhase     = getBeatPhase();

    // Signed phase error wrapped to [-0.5, +0.5].
    double diff = masterPhase - myPhase;
    if (diff >  0.5) diff -= 1.0;
    if (diff < -0.5) diff += 1.0;

    // Nothing to do if already aligned.
    if (std::abs(diff) < 0.005)
        return;

    const double beatLen    = beatIntervalAt(getPosition()).lengthSec;
    const double seekOffset = diff * beatLen;
    const double len        = transportSource.getLengthInSeconds();
    const double newPos     = std::clamp(getPosition() + seekOffset, -PRE_ROLL_SECONDS,
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
    m_phaseNudge = 0.0;
}


void DjEngine::setSyncEnabled(bool enabled)
{
    if (m_syncEnabled == enabled)
        return;

    m_syncEnabled = enabled;

    if (!enabled) {
        m_resyncBoost = false;
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

    DjEngine* master = nullptr;
    { std::lock_guard<std::mutex> g(s_syncMutex); master = s_syncMasterDeck; }
    if (!master || !master->m_trackData)
        return;

    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0)
        return;

    const double masterPhase = master->getBeatPhase();
    const double myPhase     = getBeatPhase();
    double diff = masterPhase - myPhase;
    if (diff >  0.5) diff -= 1.0;
    if (diff < -0.5) diff += 1.0;

    if (std::abs(diff) < 0.005)
        return;

    // Seek 85% of the phase error immediately (barely audible for errors up to half a beat),
    // then let the boosted P-controller handle the remaining 15% in ~2 seconds.
    const double beatLen    = beatIntervalAt(getPosition()).lengthSec;
    const double seekOffset = diff * beatLen * 0.85;
    const double len        = transportSource.getLengthInSeconds();
    const double newPos     = std::clamp(getPosition() + seekOffset, -PRE_ROLL_SECONDS,
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
    m_phaseNudge  = 0.0;
    m_resyncBoost = true;
    updatePhaseCorrection();
}

