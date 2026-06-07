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
    m_mainCueSec = -(PRE_ROLL_SECONDS + 1.0);

    if (!m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const double storedCue = m_libraryDb->mainCuePointForTrack(m_currentTrackId);
    m_mainCueSec = storedCue >= 0.0 ? storedCue : -(PRE_ROLL_SECONDS + 1.0);
    emit mainCueChanged();
}


void DjEngine::persistMainCuePoint()
{
    if (!m_libraryDb || m_currentTrackId.isEmpty())
        return;
    m_libraryDb->upsertMainCuePoint(m_currentTrackId, m_mainCueSec);
}


void DjEngine::resetMainCueButtonState()
{
    ++m_mainCuePressSerial;
    m_mainCueButtonDown = false;
    m_mainCueHoldPreviewPending = false;
    m_mainCuePreviewActive = false;
}


void DjEngine::startMainCueHoldPreview(quint64 pressSerial)
{
    if (pressSerial != m_mainCuePressSerial
        || !m_mainCueButtonDown
        || !m_mainCueHoldPreviewPending
        || m_mainCuePreviewActive
        || m_playRequested
        || !m_hasTrack) {
        return;
    }

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double cuePos = std::clamp(m_mainCueSec >= -PRE_ROLL_SECONDS ? m_mainCueSec : 0.0,
                                     -PRE_ROLL_SECONDS,
                                     trackLen);

    m_mainCueHoldPreviewPending = false;
    m_mainCuePreviewActive = true;
    transportSource.setPosition(std::max(0.0, cuePos));
    m_scrubHoldPosition = cuePos;
    if (cuePos < 0.0) {
        m_preRollCountdownActive = true;
        m_preRollVisualStartPos = cuePos;
        m_preRollClock.restart();
        m_snapValid = false;
        m_atomicPlayheadPos.store(cuePos, std::memory_order_relaxed);
    } else {
        setSnapAnchor(cuePos, true);
        armVisualSeekSettle();
        transportSource.start();
    }
    emit playingChanged();
    emit progressChanged();
}


void DjEngine::cueButtonPress()
{
    if (!m_hasTrack)
        return;

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    if (m_mainCueButtonDown)
        return;

    m_mainCueButtonDown = true;
    m_mainCueHoldPreviewPending = false;
    const quint64 pressSerial = ++m_mainCuePressSerial;

    const bool wasPlaying = m_playRequested;

    if (wasPlaying) {
        // While playing, CUE jumps to the stored cue and continues playback.
        if (m_mainCueSec < -PRE_ROLL_SECONDS) {
            m_mainCueSec = std::clamp(static_cast<double>(getVisualPosition()), -PRE_ROLL_SECONDS, trackLen);
            persistMainCuePoint();
            emit mainCueChanged();
        }

        const double cuePos = std::clamp(m_mainCueSec, -PRE_ROLL_SECONDS, trackLen);
        transportSource.setPosition(std::max(0.0, cuePos));
        m_scrubHoldPosition = cuePos;
        setSnapAnchor(cuePos, true);
        armVisualSeekSettle();
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(cuePos);
        emit progressChanged();
        return;
    }

    // While paused, pressing CUE sets the cue point at current position and previews while held.
    const double cuePos = std::clamp(static_cast<double>(getVisualPosition()), -PRE_ROLL_SECONDS, trackLen);
    m_mainCueSec = cuePos;
    persistMainCuePoint();
    emit mainCueChanged();

    transportSource.setPosition(std::max(0.0, cuePos));
    m_scrubHoldPosition = cuePos;
    setSnapAnchor(cuePos, true);
    armVisualSeekSettle();
    if (m_analyzer && m_analyzer->isThreadRunning())
        m_analyzer->setSeekHint(cuePos);

    // Start cue preview immediately so MIDI/controller cue has the same
    // down-event immediacy as a physical transport button.
    m_mainCueHoldPreviewPending = true;
    startMainCueHoldPreview(pressSerial);
    emit progressChanged();
}


void DjEngine::cueButtonRelease()
{
    if (!m_mainCueButtonDown && !m_mainCuePreviewActive)
        return;

    m_mainCueButtonDown = false;
    m_mainCueHoldPreviewPending = false;
    ++m_mainCuePressSerial;

    if (!m_mainCuePreviewActive)
        return;

    m_mainCuePreviewActive = false;

    // CUE+Play trick (Serato/Rekordbox behavior): if PLAY was pressed while CUE was
    // held, continue playing normally instead of snapping back to the cue point.
    if (m_playRequested) {
        emit playingChanged();
        return;
    }

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double cuePos = std::clamp(m_mainCueSec >= -PRE_ROLL_SECONDS ? m_mainCueSec : 0.0, -PRE_ROLL_SECONDS, trackLen);
    m_preRollCountdownActive = false;
    if (mixerSource)
        mixerSource->armClickFreeTransition();
    if (transportSource.isPlaying())
        transportSource.stop();
    transportSource.setPosition(std::max(0.0, cuePos));
    m_scrubHoldPosition = cuePos;  // sync visual hold so pre-roll position survives timer ticks

    m_snapPosition = cuePos;
    m_snapClock.restart();
    m_snapValid = false;
    m_atomicPlayheadPos.store(cuePos, std::memory_order_relaxed);
    armVisualSeekSettle();

    emit playingChanged();
    emit progressChanged();
}

