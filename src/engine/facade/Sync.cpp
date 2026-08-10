#include "../DjEngine.h"

#include "audio/DeckAudioPipeline.h"
#include "deck/DeckTransport.h"
#include "domain/TrackData.h"
#include "sync/SyncCoordinator.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ranges>

namespace {

constexpr int kBeatsPerBar = 4;

QString normalizedTrackPath(const QString& path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

double finiteOr(double value, double fallback = 0.0) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

} // namespace

double DjEngine::keylockLatencySeconds() const
{
    if (!m_keylock)
        return 0.0;
    const double sampleRate = m_transport->sourceSampleRate() > 0.0
        ? m_transport->sourceSampleRate() : 44100.0;
    return static_cast<double>(m_transport->keylockLatencySamples()) / sampleRate;
}

double DjEngine::getBeatPhase() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (!std::isfinite(bpm) || bpm <= 0.0) return 0.0;

    const double position = getPosition();
    if (m_trackData->getBeatGrid().size() >= 2) {
        const auto [previous, beatLength] = beatIntervalAt(position);
        return std::clamp((position - previous) / beatLength, 0.0, 0.9999);
    }
    const double phase = std::fmod(position / (60.0 / bpm), 1.0);
    return phase < 0.0 ? phase + 1.0 : phase;
}

double DjEngine::getBarPhase() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (!std::isfinite(bpm) || bpm <= 0.0) return 0.0;

    const auto& grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        const double beatPosition = getBeatPosition();
        int downbeatOffset = 0;
        if (const auto it = std::ranges::find_if(grid, &TrackData::BeatMarker::isDownbeat);
            it != grid.end()) {
            downbeatOffset = static_cast<int>(std::distance(grid.begin(), it) % kBeatsPerBar);
        }
        const double relative = beatPosition - static_cast<double>(downbeatOffset);
        const double barPosition = std::fmod(std::fmod(relative, kBeatsPerBar) + kBeatsPerBar,
                                             kBeatsPerBar);
        return barPosition / static_cast<double>(kBeatsPerBar);
    }

    const double sampleRate = m_trackData->getSampleRate();
    const double firstBeat = sampleRate > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sampleRate : 0.0;
    const double barLength = static_cast<double>(kBeatsPerBar) * 60.0 / bpm;
    const double relative = getPosition() - firstBeat;
    return std::fmod(std::fmod(relative, barLength) + barLength, barLength) / barLength;
}

double DjEngine::getBeatPosition() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (!std::isfinite(bpm) || bpm <= 0.0) return 0.0;

    const double position = getPosition();
    const auto& grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        const auto it = std::upper_bound(grid.begin(), grid.end(), position,
            [](double value, const TrackData::BeatMarker& marker) {
                return value < marker.positionSec;
            });
        const auto previous = it != grid.begin() ? std::prev(it) : grid.begin();
        const int beatIndex = static_cast<int>(std::distance(grid.begin(), previous));
        double beatLength = 60.0 / bpm;
        if (std::next(previous) != grid.end()) {
            const double candidate = std::next(previous)->positionSec - previous->positionSec;
            if (candidate > 0.001)
                beatLength = candidate;
        }
        return static_cast<double>(beatIndex) + ((position - previous->positionSec) / beatLength);
    }

    const double sampleRate = m_trackData->getSampleRate();
    const double firstBeat = sampleRate > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sampleRate : 0.0;
    return (position - firstBeat) / (60.0 / bpm);
}

engine::sync::DeckSyncInputSnapshot DjEngine::buildSyncInputSnapshot() const
{
    engine::sync::DeckSyncInputSnapshot input;
    const DeckTransportSnapshot transport = m_transport->snapshot();
    const double bpm = m_trackData ? m_trackData->getBpm() : 0.0;
    const auto& grid = m_trackData->getBeatGrid();
    const bool validBpm = std::isfinite(bpm) && bpm > 0.0;
    const bool gridValid = validBpm && grid.size() >= 2;
    const bool downbeatValid = gridValid
        && std::ranges::any_of(grid, &TrackData::BeatMarker::isDownbeat);

    input.hasTrack = transport.hasTrack && m_trackData;
    input.playing = transport.playing;
    input.scratching = m_scratch.scrubbing();
    input.scratchRelease = m_scratch.releaseGlide();
    input.reverse = transport.reverse;
    input.slipEnabled = transport.slipEnabled;
    input.loopActive = loopActive();
    input.keylockEnabled = m_keylock;
    input.beatgridValid = gridValid;
    input.downbeatValid = downbeatValid;
    input.trackBpm = validBpm ? bpm : 0.0;
    input.effectiveBpm = validBpm ? finiteOr(getCurrentBpm()) : 0.0;
    input.playbackRate = finiteOr(transport.playbackRate, 1.0);
    input.audiblePositionSeconds = finiteOr(transport.audiblePositionSeconds);
    input.beatPosition = finiteOr(getBeatPosition());
    input.barPosition = finiteOr(getBarPhase());
    input.beatPhase = finiteOr(getBeatPhase());
    input.beatLengthSeconds = validBpm
        ? finiteOr(beatIntervalAt(input.audiblePositionSeconds).lengthSec, 60.0 / bpm) : 0.0;
    input.keylockLatencySeconds = finiteOr(keylockLatencySeconds());
    input.beatConfidence = gridValid ? 1.0 : (validBpm ? 0.5 : 0.0);
    input.downbeatConfidence = downbeatValid ? 1.0 : 0.0;
    const QString path = normalizedTrackPath(m_trackFilePath);
    input.trackIdentity = path.isEmpty() ? 0 : static_cast<std::uint64_t>(qHash(path));
    if (!path.isEmpty() && input.trackIdentity == 0)
        input.trackIdentity = 1;
    input.trackGeneration = transport.trackGeneration;
    input.transportGeneration = transport.stateGeneration;
    return input;
}

void DjEngine::publishSyncInputAndApplyActions()
{
    m_syncCoordinator.updateDeck(m_deckIndex, buildSyncInputSnapshot());
    applyPendingSyncActions();
    refreshSyncFacadeSignals();
}

void DjEngine::applyPendingSyncActions()
{
    const auto actions = m_syncController->takeActions();
    const auto controllerState = m_syncController->snapshot();
    if (actions.masterGeneration != 0
        && actions.masterGeneration != controllerState.masterGeneration)
        return;
    if (actions.targetTrackGeneration != 0
        && actions.targetTrackGeneration != m_transport->trackGeneration())
        return;

    bool speedUpdateRequired = false;
    if (actions.tempoChanged) {
        const double clamped = std::clamp(actions.targetTempoPercent, -100.0, 100.0);
        if (std::abs(clamped - m_tempoPercent) > 1.0e-9) {
            m_tempoPercent = clamped;
            emit tempoChanged();
            speedUpdateRequired = true;
        }
    }
    if (actions.phaseNudgeChanged)
        speedUpdateRequired = true;
    if (speedUpdateRequired)
        updateSpeedAndPitch();
    if (actions.seekRequested)
        applySyncSeekOffset(actions.seekOffsetSeconds);
}

void DjEngine::refreshSyncFacadeSignals()
{
    const auto state = m_syncController->snapshot();
    if (state.syncEnabled != m_lastPublishedSyncEnabled) {
        m_lastPublishedSyncEnabled = state.syncEnabled;
        emit syncChanged();
    }
    if (state.isMaster != m_lastPublishedSyncMaster) {
        m_lastPublishedSyncMaster = state.isMaster;
        emit syncMasterChanged();
    }
}

void DjEngine::applySyncSeekOffset(double seekOffset)
{
    const double length = m_transport->trackLengthSeconds();
    const double newPosition = std::clamp(getPosition() + seekOffset, -PRE_ROLL_SECONDS,
                                           length > 0.0 ? length : PRE_ROLL_SECONDS);
    if (newPosition < 0.0) {
        if (m_transport->preRollActive())
            m_transport->beginPreRoll(newPosition);
        else
            m_transport->setHeldPosition(newPosition);
    } else {
        m_transport->seekAudioToSeconds(newPosition);
        armSnapFromTransportPosition();
    }
}

void DjEngine::alignToSyncMasterOnPlay()
{
    if (!syncEnabled() || isSyncMaster())
        return;
    publishSyncInputAndApplyActions();
    m_syncCoordinator.requestPhaseArrange(m_deckIndex);
    applyPendingSyncActions();
}

void DjEngine::setSyncEnabled(bool enabled)
{
    if (syncEnabled() == enabled)
        return;
    m_syncCoordinator.setDeckSyncEnabled(m_deckIndex, enabled);
    applyPendingSyncActions();
    refreshSyncFacadeSignals();
}

void DjEngine::reSync()
{
    if (!syncEnabled() || isSyncMaster())
        return;
    publishSyncInputAndApplyActions();
    m_syncCoordinator.requestPhaseArrange(m_deckIndex, true);
    applyPendingSyncActions();
}
