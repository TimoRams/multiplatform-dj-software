#include "DjEngine.h"
#include "DjMasterBus.h"
#include "audio/ReverseStreamAudioSource.h"
#include "audio/AudioDeviceUtils.h"
#include "audio/MetadataUtils.h"
#include "library/CoverArtExtractor.h"
#include "library/CoverArtProvider.h"
#include "library/LibraryCoverService.h"
#include "fx/FxProcessor.h"
#include "library/LibraryDatabase.h"
#include "library/TrackIdGenerator.h"
#include "WaveformCache.h"
#include "WaveformAnalyzer.h"
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
#include <QVariantMap>
#include <QImage>
#include <QBuffer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
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

void DjEngine::activateLoopRange(double inSec, double outSec, bool jumpToIn)
{
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    double in = std::clamp(inSec, -PRE_ROLL_SECONDS, trackLen);
    double out = std::clamp(outSec, -PRE_ROLL_SECONDS, trackLen);
    if (out <= in + 0.001)
        return;

    m_loopInSec = in;
    m_loopOutSec = out;
    m_loopInSet = true;
    m_loopActive = true;

    const double beatDur = beatDurationAround(in);
    if (beatDur > 1e-4) {
        constexpr double kMinLoopBeats = 1.0 / 64.0;
        constexpr double kMaxLoopBeats = 4096.0;
        const double beats = (out - in) / beatDur;
        m_loopLengthBeats = std::clamp(beats, kMinLoopBeats, kMaxLoopBeats);
    }

    applyLoopRangeToAudioSource();

    if (jumpToIn) {
        const double pos = std::max(0.0, in);
        transportSource.setPosition(pos);
        m_scrubHoldPosition = pos;
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
    if (!mixerSource || !m_trackData)
        return;

    const double pos = getPosition();
    const double beatDur = beatDurationAround(pos);
    if (beatDur <= 0.001)
        return;

    mixerSource->setBeatSyncPosition(getBeatPosition(), beatDur);
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
    const double trackLen = transportSource.getLengthInSeconds();
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

    m_loopInSec = start;
    m_loopOutSec = end;
    m_loopLengthBeats = (end - start) / beatDur;
    m_loopActive = true;
    m_loopInSet = true;
    applyLoopRangeToAudioSource();
    emit loopChanged();
}


void DjEngine::setLoopIn()
{
    double pos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        pos = quantizedBeatAt(pos);

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    m_loopInSec = std::clamp(pos, -PRE_ROLL_SECONDS, trackLen);
    m_loopInSet = true;

    if (m_loopActive) {
        if (m_loopOutSec <= m_loopInSec)
            m_loopOutSec = std::min(trackLen, m_loopInSec + beatDurationAround(m_loopInSec));
    } else {
        m_loopOutSec = m_loopInSec;
        m_loopLengthBeats = 0.0;
    }
    if (m_loopActive)
        applyLoopRangeToAudioSource();
    emit loopChanged();
}


void DjEngine::setLoopOut()
{
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    double outPos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        outPos = quantizedBeatAt(outPos);
    outPos = std::clamp(outPos, -PRE_ROLL_SECONDS, trackLen);

    // If no IN point is set yet, create a sensible default one-beat loop ending at OUT.
    if (!m_loopInSet) {
        const double beatDurAtOut = beatDurationAround(std::max(0.0, outPos));
        if (beatDurAtOut <= 1e-4)
            return;
        m_loopInSec = std::clamp(outPos - beatDurAtOut, -PRE_ROLL_SECONDS, outPos);
        m_loopInSet = true;
    }

    const double minLenSec = 0.001;
    if (outPos <= m_loopInSec + minLenSec) {
        outPos = std::min(trackLen, m_loopInSec + beatDurationAround(m_loopInSec));
    }
    if (outPos <= m_loopInSec + minLenSec)
        return;

    m_loopOutSec = outPos;
    m_loopActive = true;

    const double beatDurAtIn = beatDurationAround(m_loopInSec);
    if (beatDurAtIn > 1e-4) {
        constexpr double kMinLoopBeats = 1.0 / 64.0;
        constexpr double kMaxLoopBeats = 4096.0;
        const double beats = (m_loopOutSec - m_loopInSec) / beatDurAtIn;
        m_loopLengthBeats = std::clamp(beats, kMinLoopBeats, kMaxLoopBeats);
    }

    applyLoopRangeToAudioSource();
    emit loopChanged();
}


void DjEngine::toggleLoop4Beats()
{
    if (m_loopActive) {
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
    if (m_loopActive && std::abs(m_loopLengthBeats - 0.75) < 0.06) {
        deactivateLoop();
        return;
    }
    startLoopAt(static_cast<double>(getVisualPosition()), 0.75);
}


void DjEngine::halveLoopLength()
{
    if (!m_loopActive) {
        startLoopAt(static_cast<double>(getVisualPosition()), 2.0);
        return;
    }
    startLoopAt(m_loopInSec, m_loopLengthBeats / 2.0);
}


void DjEngine::doubleLoopLength()
{
    if (!m_loopActive) {
        startLoopAt(static_cast<double>(getVisualPosition()), 8.0);
        return;
    }
    startLoopAt(m_loopInSec, m_loopLengthBeats * 2.0);
}


void DjEngine::clearLoop()
{
    if (!m_loopActive && !m_loopInSet && m_loopLengthBeats == 0.0)
        return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_loopActive = false;
    m_loopInSet = false;
    m_loopLengthBeats = 0.0;
    m_loopInSec = 0.0;
    m_loopOutSec = 0.0;
    clearLoopRangeOnAudioSource();
    if (wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit loopChanged();
}


void DjEngine::deactivateLoop()
{
    if (!m_loopActive)
        return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_loopActive = false;
    clearLoopRangeOnAudioSource();
    if (wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit loopChanged();
}


void DjEngine::reactivateLoop()
{
    if (m_loopActive || m_loopInSec >= m_loopOutSec)
        return;
    m_loopActive = true;
    applyLoopRangeToAudioSource();
    emit loopChanged();
}


void DjEngine::beatJump(double beats)
{
    const double trackLen = transportSource.getLengthInSeconds();
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
    if (!reverseWrapSource || !m_loopActive || m_loopOutSec <= m_loopInSec)
        return;

    if (m_isReverse) {
        clearLoopRangeOnAudioSource();
        return;
    }

    // Loops involving pre-roll are enforced in software by onTimer() because the
    // audio source has no concept of negative sample positions (silence doesn't
    // exist in the buffer).  Clear any audio-source loop for these cases.
    if (m_loopInSec < 0.0 || m_loopOutSec <= 0.0) {
        clearLoopRangeOnAudioSource();
        return;
    }

    auto* reverseSource = reverseWrapSource.get();
    if (!reverseSource)
        return;

    const double sr = (m_loadedTrackSampleRate > 1.0)
        ? m_loadedTrackSampleRate
        : (m_trackData ? m_trackData->getSampleRate() : 44100.0);

    const juce::int64 loopInSample  = static_cast<juce::int64>(std::llround(m_loopInSec  * sr));
    const juce::int64 loopOutSample = static_cast<juce::int64>(std::llround(m_loopOutSec * sr));

    reverseSource->setLoopRangeSamples(loopInSample, loopOutSample, sr);
}


void DjEngine::clearLoopRangeOnAudioSource()
{
    if (!reverseWrapSource)
        return;
    auto* reverseSource = reverseWrapSource.get();
    if (reverseSource)
        reverseSource->clearLoopRangeSamples();
}

