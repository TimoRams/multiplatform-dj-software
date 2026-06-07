#include "DjEngine.h"
#include "audio/TimeStretchAudioSource.h"
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

void DjEngine::setDownbeatAtPosition(double anchorSec)
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed())
        return;

    const double trackLengthSec = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLengthSec <= 0.0)
        return;

    m_trackData->shiftBeatgridToDownbeat(anchorSec, trackLengthSec);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
}


void DjEngine::setDownbeatAtCurrentPosition()
{
    setDownbeatAtPosition(static_cast<double>(getVisualPosition()));
}


void DjEngine::nudgeBeatgridMs(double milliseconds)
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed())
        return;

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0 || std::abs(milliseconds) < 1e-6)
        return;

    m_trackData->nudgeBeatgrid(milliseconds / 1000.0, trackLen);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
}


void DjEngine::nudgeBeatgridBeats(double beats)
{
    if (!m_trackData || std::abs(beats) < 1e-6)
        return;

    const double pos = static_cast<double>(getVisualPosition());
    const double beatDur = beatDurationAround(pos);
    if (beatDur <= 1e-4)
        return;

    nudgeBeatgridMs(beats * beatDur * 1000.0);
}

// Helper: find the positionSec of the downbeat (isDownbeat == true) nearest
// to currentSec in the existing beat grid.  Falls back to currentSec itself
// if the grid is empty or has no downbeats.
static double nearestDownbeatAnchor(const std::vector<TrackData::BeatMarker>& grid,
                                    double currentSec)
{
    double best     = currentSec;
    double bestDist = std::numeric_limits<double>::max();
    for (const auto& m : grid) {
        if (!m.isDownbeat) continue;
        double d = std::abs(m.positionSec - currentSec);
        if (d < bestDist) { bestDist = d; best = m.positionSec; }
    }
    return best;
}


void DjEngine::doubleBpm()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed()) return;
    double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLen <= 0.0) return;

    double currentSec = static_cast<double>(getVisualPosition());
    double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    double newBpm = m_trackData->getBpm() * 2.0;
    m_trackData->setBpm(newBpm);
    m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();   // update BPM display in UI
}


void DjEngine::halveBpm()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed()) return;
    double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLen <= 0.0) return;

    double currentSec = static_cast<double>(getVisualPosition());
    double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    double newBpm = m_trackData->getBpm() / 2.0;
    m_trackData->setBpm(newBpm);
    m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();   // update BPM display in UI
}


void DjEngine::updateSpeedAndPitch()
{
    double speedMultiplier = 1.0 + ((m_tempoPercent + m_phaseNudge + m_jogNudgePercent) / 100.0);
    speedMultiplier = std::clamp(speedMultiplier, 0.01, 8.0);

    if (scratchBridge) {
        scratchBridge->setDeckTempoRatio(speedMultiplier);
        scratchBridge->setKeylockPassthrough(m_keylock);
    }

    if (timeStretchSource) {
        timeStretchSource->setTempoRatio(speedMultiplier);
        timeStretchSource->setPitchLockEnabled(m_keylock);
    }
}


void DjEngine::setKeylock(bool on)
{
    if (m_keylock == on) return;
    m_keylock = on;
    updateSpeedAndPitch();
    emit keylockChanged();
}


void DjEngine::applyTempoPercent(double percent)
{
    percent = std::clamp(percent, -100.0, 100.0);
    if (m_tempoPercent == percent) return;
    m_tempoPercent = percent;

    if (scratchBridge && (m_scratch.scrubbing() || m_scratch.releaseGlide()))
        scratchBridge->setDeckTempoRatio(getTempoRatio());

    updateSpeedAndPitch();
    emit tempoChanged();

    if (m_syncEnabled) {
        bool amMaster = false;
        {
            std::lock_guard<std::mutex> g(s_syncMutex);
            updateSyncMasterLocked();
            amMaster = m_isSyncMaster;
        }
        if (amMaster)
            propagateMasterTempoLocked(this);
    }
}


void DjEngine::setTempoPercent(double percent)
{
    // Sync followers ignore tempo-fader moves — only the master drives tempo.
    if (m_syncEnabled && !m_isSyncMaster)
        return;

    applyTempoPercent(percent);
}


void DjEngine::setTempoRangePercent(double percent)
{
    const double clamped = std::clamp(percent, 6.0, 100.0);
    if (std::abs(m_tempoRangePercent - clamped) < 0.001)
        return;

    m_tempoRangePercent = clamped;
    applyTempoPercent(std::clamp(m_tempoPercent, -m_tempoRangePercent, m_tempoRangePercent));
    emit tempoRangeChanged();
}


void DjEngine::setManualBpm(double bpm)
{
    if (!m_trackData)
        return;

    const double clamped = std::clamp(bpm, 20.0, 300.0);
    if (clamped <= 0.0)
        return;

    const double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    const double currentSec = static_cast<double>(getVisualPosition());
    const double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    m_trackData->setBpm(clamped);
    if (trackLen > 0.0)
        m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);

    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();

    if (m_syncEnabled) {
        bool amMaster = false;
        {
            std::lock_guard<std::mutex> g(s_syncMutex);
            updateSyncMasterLocked();
            amMaster = m_isSyncMaster;
        }
        if (amMaster)
            propagateMasterTempoLocked(this);
    }
}

