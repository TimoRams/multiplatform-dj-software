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

QVariantList DjEngine::savedLoops() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_savedLoopSlots.size()));

    for (size_t i = 0; i < m_savedLoopSlots.size(); ++i) {
        const auto& slot = m_savedLoopSlots[i];
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
    return index >= 0 && index < static_cast<int>(m_savedLoopSlots.size());
}


void DjEngine::clearSavedLoopState()
{
    for (size_t i = 0; i < m_savedLoopSlots.size(); ++i) {
        auto& slot = m_savedLoopSlots[i];
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

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    double inSec = 0.0;
    double outSec = 0.0;

    if (m_loopInSet && m_loopOutSec > m_loopInSec + 0.001) {
        inSec = m_loopInSec;
        outSec = m_loopOutSec;
    } else if (m_loopActive && m_loopOutSec > m_loopInSec + 0.001) {
        inSec = m_loopInSec;
        outSec = m_loopOutSec;
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


