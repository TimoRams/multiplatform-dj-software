#include "DjEngineCommonIncludes.h"


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
    out.reserve(static_cast<int>(m_hotCueSlots.size()));

    for (size_t i = 0; i < m_hotCueSlots.size(); ++i) {
        const auto& slot = m_hotCueSlots[i];
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
    return index >= 0 && index < static_cast<int>(m_hotCueSlots.size());
}


void DjEngine::clearHotCueState()
{
    for (size_t i = 0; i < m_hotCueSlots.size(); ++i) {
        auto& slot = m_hotCueSlots[i];
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
    return m_loopActive && m_loopInSet && m_loopOutSec > m_loopInSec + 0.001;
}


void DjEngine::storeHotCue(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    if (isLoopCuePad(index))
        clearSavedLoop(index);

    const double trackLen = transportSource.getLengthInSeconds();
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
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double pos = std::clamp(targetSec, -PRE_ROLL_SECONDS, trackLen);
    transportSource.setPosition(std::max(0.0, pos));
    m_scrubHoldPosition = pos;
    if (m_playRequested && pos < 0.0) {
        m_preRollCountdownActive = true;
        m_preRollVisualStartPos = pos;
        m_preRollClock.restart();
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
    const double cur = transportSource.getCurrentPosition();
    m_pendingCueJumpTargetSec = targetSec;
    m_pendingCueJumpFireSec   = nextBeatBoundaryAfter(cur);
    m_pendingCueJumpLastPos   = cur;
    m_pendingCueJumpActive    = true;
}


void DjEngine::cancelQuantizedCueJump()
{
    m_pendingCueJumpActive = false;
}


bool DjEngine::serviceQuantizedCueJump()
{
    if (!m_pendingCueJumpActive)
        return false;

    const double cur = transportSource.getCurrentPosition();
    const bool reached     = cur + 1e-4 >= m_pendingCueJumpFireSec;
    const bool wrappedBack = cur < m_pendingCueJumpLastPos - 0.02; // loop wrapped first
    m_pendingCueJumpLastPos = cur;

    if (!reached && !wrappedBack)
        return false;

    const double target = m_pendingCueJumpTargetSec;
    m_pendingCueJumpActive = false;
    performCueJump(target);
    return true;
}


void DjEngine::triggerHotCueJump(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack || !isHotCuePad(index))
        return;

    const auto& slot = slotAt(index);
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double pos = std::clamp(slot.positionSec, -PRE_ROLL_SECONDS, trackLen);

    // CDJ-3000 quantize: while playing, defer the jump to the next beat so it
    // lands phase-locked on the grid instead of wherever the finger landed.
    if (m_quantizeEnabled && transportSource.isPlaying()
        && !m_preRollCountdownActive && pos >= 0.0) {
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


