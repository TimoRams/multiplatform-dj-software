#include "DjEngineCommonIncludes.h"


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
        const double beats = (out - in) / beatDur;
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
    const double nomLen = 60.0 / bpm;
    const auto&  grid   = m_trackData->getBeatGrid();

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
    if (!m_audioGraph->mixerPtr() || !m_trackData)
        return;

    const double pos = getPosition();
    const double beatDur = beatDurationAround(pos);
    if (beatDur <= 0.001)
        return;

    m_audioGraph->mixer().setBeatSyncPosition(getBeatPosition(), beatDur);
}


double DjEngine::quantizedBeatAt(double sec) const
{
    if (!m_trackData)
        return sec;

    const auto& grid = m_trackData->getBeatGrid();
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
    const auto& grid = m_trackData->getBeatGrid();
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

    const auto& grid = m_trackData->getBeatGrid();
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
    double end = start + beats * beatDur;
    if (end > trackLen)
        end = trackLen;
    if (end <= start + 0.001)
        return;

    m_cueLoopController.activeLoop().inSec = start;
    m_cueLoopController.activeLoop().outSec = end;
    m_cueLoopController.activeLoop().lengthBeats = (end - start) / beatDur;
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
        const double beats = (m_cueLoopController.activeLoop().outSec - m_cueLoopController.activeLoop().inSec) / beatDurAtIn;
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
    if (trackLen <= 0.0)
        return;

    const double current = getVisualPosition();
    const double beatDur = beatDurationAround(std::max(0.0, current));
    if (beatDur <= 1e-4)
        return;

    const double next = std::clamp(current + beats * beatDur, -PRE_ROLL_SECONDS, trackLen);
    setPosition(static_cast<float>(next / trackLen));
}


void DjEngine::applyLoopRangeToAudioSource()
{
    if (!m_transport->hasTrack() || !m_cueLoopController.activeLoop().active || m_cueLoopController.activeLoop().outSec <= m_cueLoopController.activeLoop().inSec)
        return;

    if (m_transport->reverse()) {
        clearLoopRangeOnAudioSource();
        return;
    }

    // Loops involving pre-roll are enforced in software by onTimer() because the
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



namespace {

constexpr double kVolumeMin = 0.0;
constexpr double kVolumeMax = 1.0;
constexpr double kTrimMin = 0.0;
constexpr double kTrimMax = 2.0;
constexpr double kEqMin = -1.0;
constexpr double kEqMax = 1.0;
constexpr double kFilterMin = -1.0;
constexpr double kFilterMax = 1.0;

double playHistoryThresholdSeconds(double durationSec)
{
    if (durationSec <= 0.0)
        return 12.0;

    if (durationSec <= 45.0)
        return std::clamp(durationSec * 0.35, 5.0, 12.0);

    return std::clamp(durationSec * 0.12, 10.0, 20.0);
}

QString defaultHotCueColor(int index)
{
    static const char* kColors[] = {
        "#e04040", "#e08030", "#e0c030", "#40c040",
        "#3080e0", "#8040e0", "#e040a0", "#40c0c0",
    };
    return QString::fromUtf8(kColors[static_cast<size_t>(index) % 8]);
}

QString defaultSavedLoopColor(int index)
{
    static const char* kColors[] = {
        "#30b050", "#3080e0", "#e08030", "#8040e0",
        "#e04040", "#40c0c0", "#e0c030", "#e040a0",
    };
    return QString::fromUtf8(kColors[static_cast<size_t>(index) % 8]);
}

} // namespace

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


bool DjEngine::isValidHotCueIndex(int index) const
{
    return index >= 0 && index < static_cast<int>(m_cueLoopController.hotCues().size());
}


void DjEngine::clearHotCueState()
{
    for (size_t i = 0; i < m_cueLoopController.hotCues().size(); ++i) {
        auto& slot = m_cueLoopController.hotCues()[i];
        slot.set = false;
        slot.positionSec = 0.0;
        slot.label.clear();
        slot.color = defaultHotCueColor(static_cast<int>(i));
    }
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
        slot.color = color.isEmpty() ? defaultHotCueColor(index) : color;
    }

    emit hotCuesChanged();
}


void DjEngine::persistHotCueSlot(int index)
{
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
        slot.color = defaultHotCueColor(index);
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
    const double cur = m_transport->audioPositionSeconds();
    m_cueLoopController.scheduleCueJump(targetSec, nextBeatBoundaryAfter(cur), cur);
}


void DjEngine::cancelQuantizedCueJump()
{
    m_cueLoopController.cancelCueJump();
}


bool DjEngine::serviceQuantizedCueJump()
{
    const double cur = m_transport->audioPositionSeconds();
    const auto target = m_cueLoopController.serviceCueJump(cur);
    if (!target)
        return false;
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
        slot.color = defaultHotCueColor(index);

    persistHotCueSlot(index);
    emit hotCuesChanged();
}


void DjEngine::setHotCueColor(int index, const QString& colorHex)
{
    if (!isValidHotCueIndex(index))
        return;

    QString color = colorHex.trimmed();
    if (color.isEmpty())
        color = defaultHotCueColor(index);

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
    if (m_audioGraph->mixerPtr())
        m_audioGraph->mixer().armClickFreeTransition();
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
    return index >= 0 && index < static_cast<int>(m_cueLoopController.savedLoops().size());
}


void DjEngine::clearSavedLoopState()
{
    for (size_t i = 0; i < m_cueLoopController.savedLoops().size(); ++i) {
        auto& slot = m_cueLoopController.savedLoops()[i];
        slot.set = false;
        slot.inSec = 0.0;
        slot.outSec = 0.0;
        slot.lengthBeats = 0.0;
        slot.label.clear();
        slot.color = defaultSavedLoopColor(static_cast<int>(i));
    }
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
        slot.color = color.isEmpty() ? defaultSavedLoopColor(index) : color;

        const double beatDur = beatDurationAround(slot.inSec);
        if (beatDur > 1e-4)
            slot.lengthBeats = (slot.outSec - slot.inSec) / beatDur;
    }

    emit savedLoopsChanged();
}


void DjEngine::persistSavedLoopSlot(int index)
{
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
    slot.lengthBeats = beatDur > 1e-4 ? (outSec - inSec) / beatDur : 4.0;
    if (slot.color.isEmpty())
        slot.color = defaultSavedLoopColor(index);
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
    slot.color = defaultSavedLoopColor(index);

    persistSavedLoopSlot(index);
    emit savedLoopsChanged();
}
