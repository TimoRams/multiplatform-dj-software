#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "deck/DjEngine.h"
#include "audio/AudioEngine.h"
#include "audio/DeckAudioPipeline.h"
#include "audio/DeckChannelProcessor.h"
#include "audio/RenderModeRouter.h"
#include "audio/TimeStretchProcessor.h"
#include "audio/cache/CachedPlaybackAudioSource.h"
#include "deck/DeckTransport.h"
#include "domain/BeatGridTiming.h"
#include "library/CoverArtExtractor.h"
#include "library/LibraryCoverService.h"
#include "library/LibraryDatabase.h"
#include "waveform/WaveformAnalyzer.h"
#include "waveform/WaveformCache.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <expected>
#include <ranges>
#include <vector>

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>
#endif


void DjEngine::activateLoopRange(double inSec, double outSec, bool jumpToIn)
{
    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    double in = std::clamp(inSec, -PRE_ROLL_SECONDS, trackLen);
    double out = std::clamp(outSec, -PRE_ROLL_SECONDS, trackLen);
    if (out <= in + 0.001)
        return;

    m_cueLoopController.activeLoop().inSec = in;
    m_cueLoopController.activeLoop().outSec = out;
    m_cueLoopController.activeLoop().inSet = true;
    m_cueLoopController.activeLoop().active = true;

    const double beatDur = beatDurationAround(in);
    if (beatDur > 1e-4) {
        constexpr double kMinLoopBeats = 1.0 / 64.0;
        constexpr double kMaxLoopBeats = 4096.0;
        const double beats = beatCountBetween(in, out);
        m_cueLoopController.activeLoop().lengthBeats = std::clamp(beats, kMinLoopBeats, kMaxLoopBeats);
    }

    applyLoopRangeToAudioSource();

    if (jumpToIn) {
        const double pos = std::max(0.0, in);
        m_transport->seekAudioToSeconds(pos);
        m_transport->setHeldPosition(pos);
        setSnapAnchor(pos, true);
        armVisualSeekSettle();
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(pos);
        emit progressChanged();
    }

    emit loopChanged();
}


DjEngine::BeatInterval DjEngine::beatIntervalAt(double positionSec) const
{
    const double bpm    = m_trackData->getBpm();
    const double nomLen = bpm > 0.0 ? 60.0 / bpm : 0.5;
    const auto snapshot = m_trackData->getBeatGridSnapshot();
    const auto& grid = *snapshot;

    if (grid.size() >= 2) {
        const auto it   = std::upper_bound(grid.begin(), grid.end(), positionSec,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        const auto prev = (it != grid.begin()) ? std::prev(it) : grid.begin();
        const double prevSec = prev->positionSec;
        double beatLen = nomLen;
        if (std::next(prev) != grid.end()) {
            const double candidate = std::next(prev)->positionSec - prevSec;
            if (candidate > 0.01)
                beatLen = candidate;
        } else if (prev != grid.begin()) {
            const double candidate = prevSec - std::prev(prev)->positionSec;
            if (candidate > 0.01)
                beatLen = candidate;
        }
        return {prevSec, beatLen};
    }

    const double sr        = m_trackData->getSampleRate();
    const double firstBeat = sr > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sr : 0.0;
    const double idx = std::floor((positionSec - firstBeat) / nomLen);
    return {firstBeat + idx * nomLen, nomLen};
}


void DjEngine::updateFxBeatSyncPosition()
{
    if (!m_audioPipeline->mixerPtr() || !m_trackData)
        return;

    const double pos = getPosition();
    const double beatDur = beatDurationAround(pos);
    if (beatDur <= 0.001)
        return;

    m_audioPipeline->mixer().setBeatSyncPosition(getBeatPosition(), beatDur);
}


double DjEngine::quantizedBeatAt(double sec) const
{
    if (!m_trackData)
        return sec;

    const auto snapshot = m_trackData->getBeatGridSnapshot();
    const auto& grid = *snapshot;
    if (!grid.empty()) {
        // Binary search: first marker strictly after sec.
        const auto it = std::upper_bound(grid.begin(), grid.end(), sec,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        if (it == grid.begin())
            return grid.front().positionSec;
        if (it == grid.end())
            return grid.back().positionSec;
        const auto prev  = std::prev(it);
        const double dPrev = sec - prev->positionSec;
        const double dNext = it->positionSec - sec;
        return (dPrev <= dNext) ? prev->positionSec : it->positionSec;
    }

    const double bpm = m_trackData->getBpm();
    const double sr  = m_trackData->getSampleRate();
    if (bpm <= 0.0 || sr <= 0.0)
        return sec;

    const double beatDur   = 60.0 / bpm;
    const double firstBeat = static_cast<double>(m_trackData->getFirstBeatSample()) / sr;
    const double beatIndex = std::round((sec - firstBeat) / beatDur);
    return firstBeat + beatIndex * beatDur;
}


double DjEngine::nextBeatBoundaryAfter(double sec) const
{
    if (!m_trackData)
        return sec;

    constexpr double kEps = 1e-4;
    const auto snapshot = m_trackData->getBeatGridSnapshot();
    const auto& grid = *snapshot;
    if (!grid.empty()) {
        const auto it = std::upper_bound(grid.begin(), grid.end(), sec + kEps,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        if (it != grid.end())
            return it->positionSec;

        // Past the last grid line: extrapolate using the local beat length.
        const double beatDur = beatDurationAround(sec);
        const double last = grid.back().positionSec;
        if (beatDur > 1e-4) {
            const double n = std::floor((sec - last) / beatDur) + 1.0;
            return last + n * beatDur;
        }
        return sec;
    }

    const double bpm = m_trackData->getBpm();
    const double sr  = m_trackData->getSampleRate();
    if (bpm <= 0.0 || sr <= 0.0)
        return sec;

    const double beatDur   = 60.0 / bpm;
    const double firstBeat = static_cast<double>(m_trackData->getFirstBeatSample()) / sr;
    const double idx       = std::floor((sec - firstBeat) / beatDur + kEps) + 1.0;
    return firstBeat + idx * beatDur;
}


double DjEngine::beatDurationAround(double sec) const
{
    if (!m_trackData)
        return 0.5;

    const auto snapshot = m_trackData->getBeatGridSnapshot();
    const auto& grid = *snapshot;
    if (grid.size() >= 2) {
        // Binary search for the containing interval [prev, next).
        const auto it   = std::upper_bound(grid.begin(), grid.end(), sec,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        const auto prev = (it != grid.begin()) ? std::prev(it) : grid.begin();
        if (std::next(prev) != grid.end()) {
            const double d = std::next(prev)->positionSec - prev->positionSec;
            if (d > 1e-3) return d;
        }
        if (prev != grid.begin()) {
            const double d = prev->positionSec - std::prev(prev)->positionSec;
            if (d > 1e-3) return d;
        }
    }

    const double bpm = m_trackData->getBpm();
    return bpm > 0.0 ? (60.0 / bpm) : 0.5;
}

double DjEngine::beatCountBetween(double startSec, double endSec) const
{
    if (!m_trackData)
        return (endSec - startSec) / 0.5;
    const auto grid = m_trackData->getBeatGridSnapshot();
    const double bpm = m_trackData->getBpm();
    return beatgrid::beatPosition(*grid, endSec, bpm)
        - beatgrid::beatPosition(*grid, startSec, bpm);
}

void DjEngine::startLoopAt(double startSec, double lengthBeats)
{
    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    double start = std::clamp(startSec, -PRE_ROLL_SECONDS, trackLen);
    if (m_quantizeEnabled)
        start = quantizedBeatAt(start);

    const double beatDur = beatDurationAround(start);
    if (beatDur <= 1e-4)
        return;

    constexpr double kMinLoopBeats = 1.0 / 64.0;
    constexpr double kMaxLoopBeats = 4096.0;
    double beats = std::clamp(lengthBeats, kMinLoopBeats, kMaxLoopBeats);
    DeckCueLoopController::BeatGridSnapshot loopGrid;
    loopGrid.bpm = m_trackData->getBpm();
    const auto markers = m_trackData->getBeatGridSnapshot();
    loopGrid.beats.reserve(markers->size());
    for (const auto& marker : *markers)
        loopGrid.beats.push_back(marker.positionSec);
    double end = DeckCueLoopController::beatJumpTarget(start, beats, loopGrid);
    if (end > trackLen)
        end = trackLen;
    if (end <= start + 0.001)
        return;

    m_cueLoopController.activeLoop().inSec = start;
    m_cueLoopController.activeLoop().outSec = end;
    m_cueLoopController.activeLoop().lengthBeats = beatCountBetween(start, end);
    m_cueLoopController.activeLoop().active = true;
    m_cueLoopController.activeLoop().inSet = true;
    applyLoopRangeToAudioSource();
    emit loopChanged();
}


void DjEngine::setLoopIn()
{
    double pos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        pos = quantizedBeatAt(pos);

    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    m_cueLoopController.activeLoop().inSec = std::clamp(pos, -PRE_ROLL_SECONDS, trackLen);
    m_cueLoopController.activeLoop().inSet = true;

    if (m_cueLoopController.activeLoop().active) {
        if (m_cueLoopController.activeLoop().outSec <= m_cueLoopController.activeLoop().inSec)
            m_cueLoopController.activeLoop().outSec = std::min(trackLen, m_cueLoopController.activeLoop().inSec + beatDurationAround(m_cueLoopController.activeLoop().inSec));
    } else {
        m_cueLoopController.activeLoop().outSec = m_cueLoopController.activeLoop().inSec;
        m_cueLoopController.activeLoop().lengthBeats = 0.0;
    }
    if (m_cueLoopController.activeLoop().active)
        applyLoopRangeToAudioSource();
    emit loopChanged();
}


void DjEngine::setLoopOut()
{
    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    double outPos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        outPos = quantizedBeatAt(outPos);
    outPos = std::clamp(outPos, -PRE_ROLL_SECONDS, trackLen);

    // If no IN point is set yet, create a sensible default one-beat loop ending at OUT.
    if (!m_cueLoopController.activeLoop().inSet) {
        const double beatDurAtOut = beatDurationAround(std::max(0.0, outPos));
        if (beatDurAtOut <= 1e-4)
            return;
        m_cueLoopController.activeLoop().inSec = std::clamp(outPos - beatDurAtOut, -PRE_ROLL_SECONDS, outPos);
        m_cueLoopController.activeLoop().inSet = true;
    }

    const double minLenSec = 0.001;
    if (outPos <= m_cueLoopController.activeLoop().inSec + minLenSec) {
        outPos = std::min(trackLen, m_cueLoopController.activeLoop().inSec + beatDurationAround(m_cueLoopController.activeLoop().inSec));
    }
    if (outPos <= m_cueLoopController.activeLoop().inSec + minLenSec)
        return;

    m_cueLoopController.activeLoop().outSec = outPos;
    m_cueLoopController.activeLoop().active = true;

    const double beatDurAtIn = beatDurationAround(m_cueLoopController.activeLoop().inSec);
    if (beatDurAtIn > 1e-4) {
        constexpr double kMinLoopBeats = 1.0 / 64.0;
        constexpr double kMaxLoopBeats = 4096.0;
        const double beats = beatCountBetween(m_cueLoopController.activeLoop().inSec,
                                               m_cueLoopController.activeLoop().outSec);
        m_cueLoopController.activeLoop().lengthBeats = std::clamp(beats, kMinLoopBeats, kMaxLoopBeats);
    }

    applyLoopRangeToAudioSource();
    emit loopChanged();
}


void DjEngine::toggleLoop4Beats()
{
    if (m_cueLoopController.activeLoop().active) {
        deactivateLoop();
        return;
    }
    setLoop4Beats();
}


void DjEngine::setLoop4Beats()
{
    startLoopAt(static_cast<double>(getVisualPosition()), 4.0);
}


void DjEngine::toggleLoopThreeQuarter()
{
    // 3/4 loop = three quarters of ONE beat.
    if (m_cueLoopController.activeLoop().active && std::abs(m_cueLoopController.activeLoop().lengthBeats - 0.75) < 0.06) {
        deactivateLoop();
        return;
    }
    startLoopAt(static_cast<double>(getVisualPosition()), 0.75);
}


void DjEngine::setBeatLoop(double beats)
{
    if (!std::isfinite(beats) || beats <= 0.0)
        return;
    auto& loop = m_cueLoopController.activeLoop();
    const auto sameLength = [beats](double length) {
        return std::abs(length - beats) <= std::max(0.01, beats * 0.03);
    };
    if (loop.active) {
        // A loop clipped by the track end is shorter than its pad; the pad
        // that created it must still be able to exit it.
        const bool samePad = std::abs(loop.outSec - m_beatLoopOutSec) < 1e-6
            && sameLength(m_beatLoopRequestedBeats);
        if (samePad || sameLength(loop.lengthBeats)) {
            deactivateLoop();
            return;
        }
        startLoopAt(loop.inSec, beats);
    } else {
        startLoopAt(static_cast<double>(getVisualPosition()), beats);
    }
    m_beatLoopRequestedBeats = beats;
    m_beatLoopOutSec = loop.active ? loop.outSec : -1.0;
}


void DjEngine::beginSlipLoop(double beats)
{
    if (!std::isfinite(beats) || beats <= 0.0)
        return;
    if (m_slipLoopActive) {
        // Sliding to another roll pad resizes the roll from the same loop-in.
        startLoopAt(m_cueLoopController.activeLoop().inSec, beats);
        return;
    }
    m_slipLoopRestoreSlip = slipActive();
    m_slipLoopActive = true;
    setSlip(true);
    startLoopAt(static_cast<double>(getVisualPosition()), beats);
    if (!m_cueLoopController.activeLoop().active)
        endSlipLoop();
}


void DjEngine::endSlipLoop()
{
    if (!m_slipLoopActive)
        return;
    m_slipLoopActive = false;
    // Slip is still engaged here, so dropping the loop returns the playhead to
    // the background position that kept running during the roll.
    clearLoop();
    if (!m_slipLoopRestoreSlip)
        setSlip(false);
}


void DjEngine::halveLoopLength()
{
    if (!m_cueLoopController.activeLoop().active) {
        startLoopAt(static_cast<double>(getVisualPosition()), 2.0);
        return;
    }
    startLoopAt(m_cueLoopController.activeLoop().inSec, m_cueLoopController.activeLoop().lengthBeats / 2.0);
}


void DjEngine::doubleLoopLength()
{
    if (!m_cueLoopController.activeLoop().active) {
        startLoopAt(static_cast<double>(getVisualPosition()), 8.0);
        return;
    }
    startLoopAt(m_cueLoopController.activeLoop().inSec, m_cueLoopController.activeLoop().lengthBeats * 2.0);
}


void DjEngine::clearLoop()
{
    if (!m_cueLoopController.activeLoop().active && !m_cueLoopController.activeLoop().inSet && m_cueLoopController.activeLoop().lengthBeats == 0.0)
        return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_cueLoopController.activeLoop().active = false;
    m_cueLoopController.activeLoop().inSet = false;
    m_cueLoopController.activeLoop().lengthBeats = 0.0;
    m_cueLoopController.activeLoop().inSec = 0.0;
    m_cueLoopController.activeLoop().outSec = 0.0;
    clearLoopRangeOnAudioSource();
    if (wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit loopChanged();
}


void DjEngine::deactivateLoop()
{
    if (!m_cueLoopController.activeLoop().active)
        return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_cueLoopController.activeLoop().active = false;
    clearLoopRangeOnAudioSource();
    if (wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit loopChanged();
}


void DjEngine::reactivateLoop()
{
    if (m_cueLoopController.activeLoop().active || m_cueLoopController.activeLoop().inSec >= m_cueLoopController.activeLoop().outSec)
        return;
    m_cueLoopController.activeLoop().active = true;
    applyLoopRangeToAudioSource();
    emit loopChanged();
}


void DjEngine::beatJump(double beats)
{
    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0 || !std::isfinite(beats))
        return;

    DeckCueLoopController::BeatGridSnapshot grid;
    grid.bpm = m_trackData ? m_trackData->getBpm() : 0.0;
    if (m_trackData) {
        const double sampleRate = m_trackData->getSampleRate();
        grid.firstBeatSec = sampleRate > 0.0
            ? static_cast<double>(m_trackData->getFirstBeatSample()) / sampleRate
            : 0.0;
        const auto& markers = m_trackData->getBeatGrid();
        grid.beats.reserve(markers.size());
        std::ranges::transform(markers, std::back_inserter(grid.beats),
                               &TrackData::BeatMarker::positionSec);
    }

    const double current = getPosition();
    double jumpOrigin = current;
    if (m_quantizeEnabled) {
        jumpOrigin = m_transport->audioRunning() && !m_transport->preRollActive()
            ? nextBeatBoundaryAfter(current)
            : DeckCueLoopController::quantizedBeatAt(current, grid);
    }

    const double target = DeckCueLoopController::beatJumpTarget(jumpOrigin, beats, grid);
    if (!std::isfinite(target))
        return;

    const double next = std::clamp(target, -PRE_ROLL_SECONDS, trackLen);
    if (m_quantizeEnabled && m_transport->audioRunning()
        && !m_transport->preRollActive() && next >= 0.0) {
        scheduleQuantizedCueJump(next);
        return;
    }

    setPositionInternal(next / trackLen, false);
}

void DjEngine::setBeatJumpBeats(double beats)
{
    static constexpr std::array<double, 8> kSupported {
        0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0
    };
    if (!std::isfinite(beats))
        return;

    const auto nearest = std::min_element(kSupported.begin(), kSupported.end(),
        [beats](double lhs, double rhs) {
            return std::abs(lhs - beats) < std::abs(rhs - beats);
        });
    if (nearest == kSupported.end() || std::abs(m_beatJumpBeats - *nearest) < 1.0e-9)
        return;
    m_beatJumpBeats = *nearest;
    emit beatJumpBeatsChanged();
}

void DjEngine::beginFastSearch()
{
    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0 || fastSearchActive())
        return;

    m_fastSearchPositionSeconds = std::clamp(
        getVisualPosition(), 0.0, trackLen);
    m_fastSearchPublishedPositionSeconds.store(
        m_fastSearchPositionSeconds, std::memory_order_release);
    m_fastSearchActive.store(true, std::memory_order_release);
    // Keep play intent, but stop the normal reader. Otherwise the transport,
    // loop and sync clocks continue advancing an older cursor between seeks.
    m_transport->stopAudio();
    if (m_audioPipeline->mixerPtr())
        m_audioPipeline->mixer().setSearchMuted(true);
}

void DjEngine::fastSearchBySeconds(double deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) || std::abs(deltaSeconds) < 1.0e-12)
        return;
    if (!fastSearchActive())
        beginFastSearch();
    if (!fastSearchActive())
        return;

    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;
    // Accumulate against our requested cursor. The audio playhead snapshot may
    // lag a burst of MIDI frames and must not erase earlier wheel deltas.
    m_fastSearchPositionSeconds = std::clamp(
        m_fastSearchPositionSeconds + deltaSeconds, 0.0, trackLen);
    m_fastSearchPublishedPositionSeconds.store(
        m_fastSearchPositionSeconds, std::memory_order_release);
    // This seek variant does not restart the reader. Repeated normal setPosition
    // calls did, which let its stale cursor race the next wheel delta.
    m_transport->seekAudioToSeconds(m_fastSearchPositionSeconds);
    m_trackLoader.setWaveformSeekHint(m_fastSearchPositionSeconds);
    if (m_analyzer && m_analyzer->isThreadRunning())
        m_analyzer->setSeekHint(m_fastSearchPositionSeconds);
    emit progressChanged();
}

void DjEngine::endFastSearch()
{
    if (!fastSearchActive())
        return;

    // Commit once more before yielding position authority. This final handoff
    // is durable for paused and playing decks alike.
    m_transport->seekAudioToSeconds(m_fastSearchPositionSeconds);
    m_fastSearchPublishedPositionSeconds.store(
        m_fastSearchPositionSeconds, std::memory_order_release);
    m_fastSearchActive.store(false, std::memory_order_release);
    if (m_audioPipeline->mixerPtr())
        m_audioPipeline->mixer().setSearchMuted(false);
    ensureTransportRunningForPlayIntent();
    emit progressChanged();
}


void DjEngine::applyLoopRangeToAudioSource()
{
    if (!m_transport->hasTrack() || !m_cueLoopController.activeLoop().active || m_cueLoopController.activeLoop().outSec <= m_cueLoopController.activeLoop().inSec)
        return;

    if (m_transport->reverse()) {
        clearLoopRangeOnAudioSource();
        return;
    }

    // Loops involving pre-roll are enforced in software by the transport control tick because the
    // audio source has no concept of negative sample positions (silence doesn't
    // exist in the buffer).  Clear any audio-source loop for these cases.
    if (m_cueLoopController.activeLoop().inSec < 0.0 || m_cueLoopController.activeLoop().outSec <= 0.0) {
        clearLoopRangeOnAudioSource();
        return;
    }

    m_transport->setLoopRegion({true, m_cueLoopController.activeLoop().inSec,
                                m_cueLoopController.activeLoop().outSec});
}


void DjEngine::clearLoopRangeOnAudioSource()
{
    m_transport->setLoopRegion({});
}

QVariantList DjEngine::hotCues() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_cueLoopController.hotCues().size()));

    for (size_t i = 0; i < m_cueLoopController.hotCues().size(); ++i) {
        const auto& slot = m_cueLoopController.hotCues()[i];
        QVariantMap m;
        m.insert("index",       static_cast<int>(i));
        m.insert("set",         slot.set);
        m.insert("positionSec", slot.positionSec);
        m.insert("label",       slot.label);
        m.insert("color",       slot.color);
        out.push_back(m);
    }

    return out;
}

void DjEngine::installExternalCues(const ExternalTrackLoadSnapshot& external)
{
    clearHotCueState();
    clearSavedLoopState();
    m_memoryCues.clear();

    int nextHotCue = 0;
    for (const auto& cue : external.analysis.hotCues) {
        int index = cue.index;
        if (!isValidHotCueIndex(index)) {
            while (nextHotCue < DeckCueLoopController::kSlotCount
                   && slotAt(nextHotCue).set) {
                ++nextHotCue;
            }
            index = nextHotCue;
        }
        if (!isValidHotCueIndex(index))
            break;
        auto& slot = slotAt(index);
        slot.set = true;
        slot.positionSec = cue.positionSec;
        slot.label = cue.label;
        slot.color = cue.color.isEmpty()
            ? DeckCueLoopController::defaultHotCueColor(index) : cue.color;
    }

    int loopIndex = 0;
    for (const auto& cue : external.analysis.loops) {
        if (!std::isfinite(cue.loopEndSec)
            || cue.loopEndSec <= cue.positionSec || !isValidSavedLoopIndex(loopIndex)) {
            continue;
        }
        auto& slot = savedLoopAt(loopIndex);
        slot.set = true;
        slot.inSec = cue.positionSec;
        slot.outSec = cue.loopEndSec;
        const double beatDuration = beatDurationAround(slot.inSec);
        slot.lengthBeats = beatDuration > 1e-4
            ? beatCountBetween(slot.inSec, slot.outSec) : 0.0;
        slot.label = cue.label;
        slot.color = cue.color.isEmpty()
            ? DeckCueLoopController::defaultSavedLoopColor(loopIndex) : cue.color;
        ++loopIndex;
    }

    for (const auto& cue : external.analysis.memoryCues) {
        m_memoryCues.append(QVariantMap{
            {QStringLiteral("positionSec"), cue.positionSec},
            {QStringLiteral("label"), cue.label},
            {QStringLiteral("color"), cue.color}});
    }
    if (!external.analysis.memoryCues.isEmpty())
        m_cueLoopController.mainCue().positionSec =
            external.analysis.memoryCues.front().positionSec;

    emit hotCuesChanged();
    emit savedLoopsChanged();
    emit memoryCuesChanged();
    emit mainCueChanged();
}


bool DjEngine::isValidHotCueIndex(int index) const
{
    return m_cueLoopController.validIndex(index);
}


void DjEngine::clearHotCueState()
{
    m_cueLoopController.clearHotCues();
}


void DjEngine::loadHotCuesForCurrentTrack()
{
    clearHotCueState();

    if (!m_libraryDb || m_currentTrackId.isEmpty()) {
        emit hotCuesChanged();
        return;
    }

    const QVariantList stored = m_libraryDb->cuePointsForTrack(m_currentTrackId);
    for (const QVariant& v : stored) {
        const QVariantMap m = v.toMap();
        const int index = m.value("index").toInt();
        if (!isValidHotCueIndex(index))
            continue;

        auto& slot = slotAt(index);
        slot.set = true;
        slot.positionSec = std::max(-PRE_ROLL_SECONDS, m.value("positionSec").toDouble());
        slot.label = m.value("label").toString();
        const QString color = m.value("color").toString().trimmed();
        slot.color = color.isEmpty()
            ? DeckCueLoopController::defaultHotCueColor(index) : color;
    }

    emit hotCuesChanged();
}


void DjEngine::persistHotCueSlot(int index)
{
    if (m_readOnlyExternalTrack) { savePerformanceExtras(); return; }
    if (!isValidHotCueIndex(index) || !m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const auto& slot = slotAt(index);
    if (slot.set) {
        const QString label = slot.label.isEmpty()
            ? QStringLiteral("HOT CUE %1").arg(index + 1)
            : slot.label;
        m_libraryDb->upsertCuePoint(m_currentTrackId, index, slot.positionSec, label, slot.color);
    } else {
        m_libraryDb->deleteCuePoint(m_currentTrackId, index);
    }
}


bool DjEngine::isHotCuePad(int index) const
{
    return isValidHotCueIndex(index) && slotAt(index).set;
}


bool DjEngine::isLoopCuePad(int index) const
{
    return isValidSavedLoopIndex(index) && savedLoopAt(index).set;
}


bool DjEngine::hasStorableLoopRegion() const
{
    // Only an *active* loop is stored as a loop cue. After a loop is switched off
    // the region is kept (so it can be reactivated), but a pad press must then
    // store a normal hot cue — not silently re-save the stale loop region.
    return m_cueLoopController.activeLoop().active && m_cueLoopController.activeLoop().inSet && m_cueLoopController.activeLoop().outSec > m_cueLoopController.activeLoop().inSec + 0.001;
}


void DjEngine::storeHotCue(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    if (isLoopCuePad(index))
        clearSavedLoop(index);

    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    // CDJ-3000 behaviour: with quantize on, a stored hot cue snaps onto the beat
    // grid so the cue always lands exactly on the grid (also while the track
    // plays). The stored point is what trigger jumps to, so we quantize here.
    double cuePos = std::clamp(static_cast<double>(getVisualPosition()), -PRE_ROLL_SECONDS, trackLen);
    if (m_quantizeEnabled)
        cuePos = std::clamp(quantizedBeatAt(cuePos), -PRE_ROLL_SECONDS, trackLen);

    auto& slot = slotAt(index);
    slot.set = true;
    slot.positionSec = cuePos;
    if (slot.color.isEmpty())
        slot.color = DeckCueLoopController::defaultHotCueColor(index);
    if (slot.label.isEmpty())
        slot.label = QStringLiteral("HOT CUE %1").arg(index + 1);

    persistHotCueSlot(index);
    emit hotCuesChanged();
}


void DjEngine::storeCuePad(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    if (hasStorableLoopRegion()) {
        if (isHotCuePad(index))
            clearHotCue(index);
        storeSavedLoop(index);
        return;
    }

    if (isLoopCuePad(index))
        clearSavedLoop(index);
    storeHotCue(index);
}


void DjEngine::performCueJump(double targetSec)
{
    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    const double pos = std::clamp(targetSec, -PRE_ROLL_SECONDS, trackLen);
    m_transport->seekAudioToSeconds(std::max(0.0, pos));
    m_transport->setHeldPosition(pos);
    if (m_transport->playRequested() && pos < 0.0) {
        m_transport->beginPreRoll(pos);
    } else {
        ensureTransportRunningForPlayIntent();
    }
    setSnapAnchor(pos, true);
    armVisualSeekSettle();
    if (m_analyzer && m_analyzer->isThreadRunning())
        m_analyzer->setSeekHint(std::max(0.0, pos));
    emit progressChanged();
}


void DjEngine::scheduleQuantizedCueJump(double targetSec)
{
    cancelQuantizedCueJump();
    const double cur = m_transport->audioPositionSeconds();
    m_cueLoopController.scheduleCueJump(targetSec, nextBeatBoundaryAfter(cur), cur);
}


void DjEngine::cancelQuantizedCueJump()
{
    m_cueLoopController.cancelCueJump();
    if (m_quantizedOverviewSeekPending) {
        m_quantizedOverviewSeekPending = false;
        if (m_seekPreviewActive) {
            m_seekPreviewActive = false;
            emit seekPreviewChanged();
        }
    }
}


bool DjEngine::serviceQuantizedCueJump()
{
    const double cur = m_transport->audioPositionSeconds();
    const auto target = m_cueLoopController.serviceCueJump(cur);
    if (!target)
        return false;

    if (m_quantizedOverviewSeekPending) {
        m_quantizedOverviewSeekPending = false;
        m_seekPreviewActive = false;
        emit seekPreviewChanged();
        const double length = m_transport->trackLengthSeconds();
        if (length > 0.0)
            setPositionInternal(*target / length, true);
        return true;
    }

    performCueJump(*target);
    return true;
}


void DjEngine::triggerHotCueJump(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack || !isHotCuePad(index))
        return;

    const auto& slot = slotAt(index);
    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    const double pos = std::clamp(slot.positionSec, -PRE_ROLL_SECONDS, trackLen);

    // CDJ-3000 quantize: while playing, defer the jump to the next beat so it
    // lands phase-locked on the grid instead of wherever the finger landed.
    if (m_quantizeEnabled && m_transport->audioRunning()
        && !m_transport->preRollActive() && pos >= 0.0) {
        scheduleQuantizedCueJump(pos);
        return;
    }

    performCueJump(pos);
}


void DjEngine::triggerCuePad(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    if (isLoopCuePad(index)) {
        triggerSavedLoop(index);
        return;
    }

    if (isHotCuePad(index)) {
        triggerHotCueJump(index);
        return;
    }

    storeCuePad(index);
}


void DjEngine::triggerHotCue(int index)
{
    triggerCuePad(index);
}


void DjEngine::clearCuePad(int index)
{
    if (isLoopCuePad(index))
        clearSavedLoop(index);
    if (isHotCuePad(index))
        clearHotCue(index);
}


void DjEngine::clearHotCue(int index)
{
    if (!isValidHotCueIndex(index))
        return;

    auto& slot = slotAt(index);
    slot.set = false;
    slot.positionSec = 0.0;
    slot.label.clear();
    if (slot.color.isEmpty())
        slot.color = DeckCueLoopController::defaultHotCueColor(index);

    persistHotCueSlot(index);
    emit hotCuesChanged();
}


void DjEngine::setHotCueColor(int index, const QString& colorHex)
{
    if (!isValidHotCueIndex(index))
        return;

    QString color = colorHex.trimmed();
    if (color.isEmpty())
        color = DeckCueLoopController::defaultHotCueColor(index);

    auto& slot = slotAt(index);
    slot.color = color;

    if (slot.set)
        persistHotCueSlot(index);

    emit hotCuesChanged();
}




bool DjEngine::beatgridLocked() const
{
    return m_trackData && m_trackData->beatgridLockedByUser();
}


void DjEngine::setBeatgridLocked(bool locked)
{
    if (!m_trackData)
        return;

    if (m_trackData->beatgridLockedByUser() == locked)
        return;

    m_trackData->setBeatgridLocked(locked);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
}


void DjEngine::loadMainCueForCurrentTrack()
{
    m_cueLoopController.mainCue().positionSec = -(PRE_ROLL_SECONDS + 1.0);

    if (!m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const double storedCue = m_libraryDb->mainCuePointForTrack(m_currentTrackId);
    m_cueLoopController.mainCue().positionSec = storedCue >= 0.0 ? storedCue : -(PRE_ROLL_SECONDS + 1.0);
    emit mainCueChanged();
}


void DjEngine::persistMainCuePoint()
{
    if (m_readOnlyExternalTrack) { savePerformanceExtras(); return; }
    if (!m_libraryDb || m_currentTrackId.isEmpty())
        return;
    m_libraryDb->upsertMainCuePoint(m_currentTrackId, m_cueLoopController.mainCue().positionSec);
}


void DjEngine::resetMainCueButtonState()
{
    ++m_cueLoopController.mainCue().pressSerial;
    m_cueLoopController.mainCue().buttonDown = false;
    m_cueLoopController.mainCue().holdPreviewPending = false;
    m_cueLoopController.mainCue().previewActive = false;
}


void DjEngine::startMainCueHoldPreview(quint64 pressSerial)
{
    if (pressSerial != m_cueLoopController.mainCue().pressSerial
        || !m_cueLoopController.mainCue().buttonDown
        || !m_cueLoopController.mainCue().holdPreviewPending
        || m_cueLoopController.mainCue().previewActive
        || m_transport->playRequested()
        || inputExternal()
        || !m_hasTrack) {
        return;
    }

    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    const double cuePos = std::clamp(m_cueLoopController.mainCue().positionSec >= -PRE_ROLL_SECONDS ? m_cueLoopController.mainCue().positionSec : 0.0,
                                     -PRE_ROLL_SECONDS,
                                     trackLen);

    m_cueLoopController.mainCue().holdPreviewPending = false;
    m_cueLoopController.mainCue().previewActive = true;
    m_transport->seekAudioToSeconds(std::max(0.0, cuePos));
    m_transport->setHeldPosition(cuePos);
    if (cuePos < 0.0) {
        m_transport->beginPreRoll(cuePos);
    } else {
        setSnapAnchor(cuePos, true);
        armVisualSeekSettle();
        m_transport->startAudio();
    }
    emit playingChanged();
    emit progressChanged();
}


void DjEngine::cueButtonPress()
{
    if (!m_hasTrack)
        return;

    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    if (m_cueLoopController.mainCue().buttonDown)
        return;

    m_cueLoopController.mainCue().buttonDown = true;
    m_cueLoopController.mainCue().holdPreviewPending = false;
    const quint64 pressSerial = ++m_cueLoopController.mainCue().pressSerial;

    const bool wasPlaying = m_transport->playRequested();

    if (wasPlaying) {
        // While playing, CUE jumps to the stored cue and continues playback.
        if (m_cueLoopController.mainCue().positionSec < -PRE_ROLL_SECONDS) {
            double newCue = std::clamp(static_cast<double>(getVisualPosition()), -PRE_ROLL_SECONDS, trackLen);
            if (m_quantizeEnabled)
                newCue = std::clamp(quantizedBeatAt(newCue), -PRE_ROLL_SECONDS, trackLen);
            m_cueLoopController.mainCue().positionSec = newCue;
            persistMainCuePoint();
            emit mainCueChanged();
        }

        const double cuePos = std::clamp(m_cueLoopController.mainCue().positionSec, -PRE_ROLL_SECONDS, trackLen);

        // CDJ-3000 quantize: defer the return-to-cue to the next beat so it lands
        // phase-locked on the grid while playing.
        if (m_quantizeEnabled && m_transport->audioRunning()
            && !m_transport->preRollActive() && cuePos >= 0.0) {
            scheduleQuantizedCueJump(cuePos);
            return;
        }

        performCueJump(cuePos);
        return;
    }

    // While paused, pressing CUE sets the cue point at current position and previews while held.
    double cuePos = std::clamp(static_cast<double>(getVisualPosition()), -PRE_ROLL_SECONDS, trackLen);
    if (m_quantizeEnabled)
        cuePos = std::clamp(quantizedBeatAt(cuePos), -PRE_ROLL_SECONDS, trackLen);
    m_cueLoopController.mainCue().positionSec = cuePos;
    persistMainCuePoint();
    emit mainCueChanged();

    m_transport->seekAudioToSeconds(std::max(0.0, cuePos));
    m_transport->setHeldPosition(cuePos);
    setSnapAnchor(cuePos, true);
    armVisualSeekSettle();
    if (m_analyzer && m_analyzer->isThreadRunning())
        m_analyzer->setSeekHint(cuePos);

    // Start cue preview immediately so MIDI/controller cue has the same
    // down-event immediacy as a physical transport button.
    m_cueLoopController.mainCue().holdPreviewPending = true;
    startMainCueHoldPreview(pressSerial);
    emit progressChanged();
}


void DjEngine::cueButtonRelease()
{
    if (!m_cueLoopController.mainCue().buttonDown && !m_cueLoopController.mainCue().previewActive)
        return;

    m_cueLoopController.mainCue().buttonDown = false;
    m_cueLoopController.mainCue().holdPreviewPending = false;
    ++m_cueLoopController.mainCue().pressSerial;

    if (!m_cueLoopController.mainCue().previewActive)
        return;

    m_cueLoopController.mainCue().previewActive = false;

    // CUE+Play trick (Serato/Rekordbox behavior): if PLAY was pressed while CUE was
    // held, continue playing normally instead of snapping back to the cue point.
    if (m_transport->playRequested()) {
        emit playingChanged();
        return;
    }

    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    const double cuePos = std::clamp(m_cueLoopController.mainCue().positionSec >= -PRE_ROLL_SECONDS ? m_cueLoopController.mainCue().positionSec : 0.0, -PRE_ROLL_SECONDS, trackLen);
    m_transport->cancelPreRoll();
    if (m_audioPipeline->mixerPtr())
        m_audioPipeline->mixer().armClickFreeTransition();
    if (m_transport->audioRunning())
        m_transport->stopAudio();
    m_transport->seekAudioToSeconds(std::max(0.0, cuePos));
    m_transport->setVisualAnchor(cuePos, false);  // keep negative pre-roll visible while paused
    armVisualSeekSettle();

    emit playingChanged();
    emit progressChanged();
}



QVariantList DjEngine::savedLoops() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_cueLoopController.savedLoops().size()));

    for (size_t i = 0; i < m_cueLoopController.savedLoops().size(); ++i) {
        const auto& slot = m_cueLoopController.savedLoops()[i];
        QVariantMap entry;
        entry.insert("index",       static_cast<int>(i));
        entry.insert("set",         slot.set);
        entry.insert("armed",       slot.armed);
        entry.insert("inSec",       slot.inSec);
        entry.insert("outSec",      slot.outSec);
        entry.insert("lengthBeats", slot.lengthBeats);
        entry.insert("label",       slot.label);
        entry.insert("color",       slot.color);
        out.push_back(entry);
    }

    return out;
}


bool DjEngine::isValidSavedLoopIndex(int index) const
{
    return m_cueLoopController.validIndex(index);
}


void DjEngine::clearSavedLoopState()
{
    m_cueLoopController.clearSavedLoops();
}


void DjEngine::loadSavedLoopsForCurrentTrack()
{
    clearSavedLoopState();

    if (!m_libraryDb || m_currentTrackId.isEmpty()) {
        emit savedLoopsChanged();
        return;
    }

    const QVariantList stored = m_libraryDb->savedLoopsForTrack(m_currentTrackId);
    for (const QVariant& v : stored) {
        const QVariantMap m = v.toMap();
        const int index = m.value("index").toInt();
        if (!isValidSavedLoopIndex(index))
            continue;

        auto& slot = savedLoopAt(index);
        slot.set = true;
        slot.inSec = m.value("inSec").toDouble();
        slot.outSec = m.value("outSec").toDouble();
        slot.label = m.value("label").toString();
        const QString color = m.value("color").toString().trimmed();
        slot.color = color.isEmpty()
            ? DeckCueLoopController::defaultSavedLoopColor(index) : color;

        const double beatDur = beatDurationAround(slot.inSec);
        if (beatDur > 1e-4)
            slot.lengthBeats = beatCountBetween(slot.inSec, slot.outSec);
    }

    emit savedLoopsChanged();
}


void DjEngine::persistSavedLoopSlot(int index)
{
    savePerformanceExtras();
    if (m_readOnlyExternalTrack) return;
    if (!isValidSavedLoopIndex(index) || !m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const auto& slot = savedLoopAt(index);
    if (slot.set) {
        const QString label = slot.label.isEmpty()
            ? QStringLiteral("LOOP %1").arg(index + 1)
            : slot.label;
        m_libraryDb->upsertSavedLoop(m_currentTrackId,
                                     index,
                                     slot.inSec,
                                     slot.outSec,
                                     label,
                                     slot.color);
    } else {
        m_libraryDb->deleteSavedLoop(m_currentTrackId, index);
    }
}


void DjEngine::storeSavedLoop(int index)
{
    if (!isValidSavedLoopIndex(index) || !m_hasTrack)
        return;

    if (isHotCuePad(index))
        clearHotCue(index);

    const double trackLen = m_transport->trackLengthSeconds();
    if (trackLen <= 0.0)
        return;

    double inSec = 0.0;
    double outSec = 0.0;

    if (m_cueLoopController.activeLoop().inSet && m_cueLoopController.activeLoop().outSec > m_cueLoopController.activeLoop().inSec + 0.001) {
        inSec = m_cueLoopController.activeLoop().inSec;
        outSec = m_cueLoopController.activeLoop().outSec;
    } else if (m_cueLoopController.activeLoop().active && m_cueLoopController.activeLoop().outSec > m_cueLoopController.activeLoop().inSec + 0.001) {
        inSec = m_cueLoopController.activeLoop().inSec;
        outSec = m_cueLoopController.activeLoop().outSec;
    } else {
        const double pos = static_cast<double>(getVisualPosition());
        const double beatDur = beatDurationAround(pos);
        if (beatDur <= 1e-4)
            return;
        inSec = std::clamp(pos, -PRE_ROLL_SECONDS, trackLen);
        outSec = std::min(trackLen, inSec + 4.0 * beatDur);
    }

    if (outSec <= inSec + 0.001)
        return;

    auto& slot = savedLoopAt(index);
    slot.set = true;
    slot.inSec = inSec;
    slot.outSec = outSec;
    const double beatDur = beatDurationAround(inSec);
    slot.lengthBeats = beatDur > 1e-4 ? beatCountBetween(inSec, outSec) : 4.0;
    if (slot.color.isEmpty())
        slot.color = DeckCueLoopController::defaultSavedLoopColor(index);
    if (slot.label.isEmpty())
        slot.label = QStringLiteral("LOOP %1").arg(index + 1);

    persistSavedLoopSlot(index);
    emit savedLoopsChanged();
}


void DjEngine::triggerSavedLoop(int index)
{
    if (!isValidSavedLoopIndex(index) || !m_hasTrack)
        return;

    const auto& slot = savedLoopAt(index);
    if (!slot.set) {
        storeSavedLoop(index);
        return;
    }

    activateLoopRange(slot.inSec, slot.outSec, true);
    ensureTransportRunningForPlayIntent();
}


void DjEngine::clearSavedLoop(int index)
{
    if (!isValidSavedLoopIndex(index))
        return;

    auto& slot = savedLoopAt(index);
    slot.set = false;
    slot.inSec = 0.0;
    slot.outSec = 0.0;
    slot.lengthBeats = 0.0;
    slot.label.clear();
    slot.color = DeckCueLoopController::defaultSavedLoopColor(index);

    persistSavedLoopSlot(index);
    emit savedLoopsChanged();
}

void DjEngine::savePerformanceExtras()
{
    if (!m_libraryDb || m_performanceKey.isEmpty()) return;
    QJsonObject data;
    data["memoryCues"] = QJsonArray::fromVariantList(m_memoryCues);
    data["loops"] = QJsonArray::fromVariantList(savedLoops());
    if (m_readOnlyExternalTrack) {
        data["hotCues"] = QJsonArray::fromVariantList(hotCues());
        data["mainCue"] = mainCueSec();
    }
    m_libraryDb->setSetting("performance/" + m_performanceKey,
        QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)));
}
void DjEngine::loadPerformanceExtras()
{
    if (!m_libraryDb || m_performanceKey.isEmpty()) return;
    const auto data = QJsonDocument::fromJson(m_libraryDb->getSetting("performance/" + m_performanceKey).toUtf8()).object();
    if (data.isEmpty()) return;
    if (data.contains("memoryCues")) {
        m_memoryCues.clear();
        for (const auto& value : data["memoryCues"].toArray()) {
            const auto cue = value.toObject();
            const double position = cue["positionSec"].toDouble(-1);
            if (position >= 0 && position <= m_transport->trackLengthSeconds()) m_memoryCues.append(cue.toVariantMap());
        }
    }
    for (const auto& value : data["loops"].toArray()) {
        const auto map = value.toObject(); const int index = map["index"].toInt(-1);
        if (!isValidSavedLoopIndex(index)) continue;
        auto& slot = savedLoopAt(index);
        if (m_readOnlyExternalTrack) {
            const double in = map["inSec"].toDouble(-1), out = map["outSec"].toDouble(-1);
            slot.set = map["set"].toBool() && in >= -PRE_ROLL_SECONDS && out > in && out <= m_transport->trackLengthSeconds();
            if (slot.set) {
                slot.inSec = in; slot.outSec = out;
                slot.lengthBeats = beatCountBetween(in, out);
                slot.label = map["label"].toString(); slot.color = map["color"].toString();
            }
        }
        slot.armed = slot.set && map["armed"].toBool();
    }
    if (m_readOnlyExternalTrack && data.contains("hotCues")) {
        clearHotCueState();
        for (const auto& value : data["hotCues"].toArray()) {
            const auto map = value.toObject(); const int index = map["index"].toInt(-1);
            const double pos = map["positionSec"].toDouble(-1);
            if (!isValidHotCueIndex(index) || !map["set"].toBool() || pos < -PRE_ROLL_SECONDS || pos > m_transport->trackLengthSeconds()) continue;
            auto& slot = slotAt(index); slot.set = true; slot.positionSec = pos;
            slot.label = map["label"].toString(); slot.color = map["color"].toString();
        }
        const double main = data["mainCue"].toDouble(-1);
        if (main >= 0 && main <= m_transport->trackLengthSeconds()) m_cueLoopController.mainCue().positionSec = main;
    }
    emit memoryCuesChanged(); emit savedLoopsChanged(); emit hotCuesChanged(); emit mainCueChanged();
}
void DjEngine::storeMemoryCue(const QString& label)
{
    if (!m_hasTrack || m_memoryCues.size() >= 32) return;
    const double pos = std::clamp(getPosition(), 0.0, m_transport->trackLengthSeconds());
    for (const auto& cue : std::as_const(m_memoryCues))
        if (std::abs(cue.toMap().value("positionSec").toDouble() - pos) < 0.01) return;
    m_memoryCues.append(QVariantMap{{"positionSec", pos}, {"label", label.left(128)}, {"color", "#f2b134"}});
    std::sort(m_memoryCues.begin(), m_memoryCues.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("positionSec").toDouble() < b.toMap().value("positionSec").toDouble();
    });
    savePerformanceExtras(); emit memoryCuesChanged();
}
void DjEngine::renameMemoryCue(int index, const QString& label)
{
    if (index < 0 || index >= m_memoryCues.size()) return;
    auto cue = m_memoryCues[index].toMap(); cue["label"] = label.left(128); m_memoryCues[index] = cue;
    savePerformanceExtras(); emit memoryCuesChanged();
}
void DjEngine::deleteMemoryCue(int index)
{
    if (index < 0 || index >= m_memoryCues.size()) return;
    m_memoryCues.removeAt(index); savePerformanceExtras(); emit memoryCuesChanged();
}
void DjEngine::jumpMemoryCue(int direction)
{
    if (m_memoryCues.isEmpty() || direction == 0) return;
    const double pos = getPosition();
    for (int i = direction > 0 ? 0 : m_memoryCues.size() - 1;
         i >= 0 && i < m_memoryCues.size(); i += direction > 0 ? 1 : -1) {
        const double target = m_memoryCues[i].toMap().value("positionSec").toDouble();
        if ((direction > 0 && target > pos + 0.05) || (direction < 0 && target < pos - 0.05)) { performCueJump(target); break; }
    }
}
void DjEngine::setSavedLoopArmed(int index, bool armed)
{
    if (!isValidSavedLoopIndex(index) || !savedLoopAt(index).set) return;
    savedLoopAt(index).armed = armed; m_lastActiveLoopPosition = -1; savePerformanceExtras(); emit savedLoopsChanged();
}
void DjEngine::serviceArmedLoops()
{
    const double pos = getPosition();
    if (isPlaying() && !isReverse() && !loopActive()) {
        for (const auto& loop : m_cueLoopController.savedLoops()) {
            const bool entering = !m_activeLoopWasPlaying || m_lastActiveLoopPosition < loop.inSec || m_lastActiveLoopPosition >= loop.outSec;
            if (loop.set && loop.armed && entering && pos >= loop.inSec
                && (pos < loop.outSec || (m_lastActiveLoopPosition >= 0
                    && m_lastActiveLoopPosition < loop.inSec && pos - m_lastActiveLoopPosition < 0.5))) {
                activateLoopRange(loop.inSec, loop.outSec, false); break;
            }
        }
    }
    m_lastActiveLoopPosition = pos;
    m_activeLoopWasPlaying = isPlaying() && !isReverse();
}
