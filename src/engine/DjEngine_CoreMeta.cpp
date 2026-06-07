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

void DjEngine::populateMetadataFromReader(const juce::AudioFormatReader& reader,
                                          const QString& rawPath,
                                          const juce::File& file)
{
    const auto metaMap = metadata::buildMetadataLookup(reader.metadataValues);

    m_trackTitle = metadata::metaValue(metaMap, {"title", "id3title", "tit2", "tt2", "name", "tracktitle", "song"});
    m_trackArtist = metadata::metaValue(metaMap, {"artist", "id3artist", "tpe1", "albumartist", "tpe2", "band", "performer", "leadartist"});
    m_trackAlbum = metadata::metaValue(metaMap, {"album", "id3album", "talb", "record", "albumtitle"});
    m_trackGenre = metadata::metaValue(metaMap, {"genre", "tcon", "contenttype"});
    m_trackComment = metadata::metaValue(metaMap, {"comment", "comm", "description"});
    m_trackKey = metadata::metaValue(metaMap, {"key", "tkey", "initialkey", "musickey", "keysig"});

    // Tag BPM is used immediately; the background analyzer will overwrite it later.
    const QString tagBpm = metadata::metaValue(metaMap, {"bpm", "tbpm", "tmpo", "tempo", "beatsperminute"});
    const double bpmVal = metadata::parseBpmString(tagBpm);
    if (bpmVal > 0.0)
        m_trackData->setBpmData(bpmVal, 0, reader.sampleRate);

    // TagLib fills gaps left by JUCE readers — JUCE's FLAC reader skips Vorbis comments entirely.
    {
        TagLib::FileRef tlFile(rawPath.toLocal8Bit().constData());
        if (!tlFile.isNull() && tlFile.tag()) {
            const TagLib::Tag* tag = tlFile.tag();
            const QString tlTitle   = metadata::cleanup(QString::fromStdString(tag->title().to8Bit(true)));
            const QString tlArtist  = metadata::cleanup(QString::fromStdString(tag->artist().to8Bit(true)));
            const QString tlGenre   = metadata::cleanup(QString::fromStdString(tag->genre().to8Bit(true)));
            const QString tlComment = metadata::cleanup(QString::fromStdString(tag->comment().to8Bit(true)));
            if (m_trackTitle.isEmpty()   && !tlTitle.isEmpty())   m_trackTitle   = tlTitle;
            if (m_trackArtist.isEmpty()  && !tlArtist.isEmpty())  m_trackArtist  = tlArtist;
            if (m_trackGenre.isEmpty()   && !tlGenre.isEmpty())   m_trackGenre   = tlGenre;
            if (m_trackComment.isEmpty() && !tlComment.isEmpty()) m_trackComment = tlComment;
        }
    }

    const auto v1 = metadata::readId3v1(rawPath);
    if (v1) {
        if (m_trackTitle.isEmpty() && !v1->title.isEmpty())
            m_trackTitle = v1->title;
        if (m_trackArtist.isEmpty() && !v1->artist.isEmpty())
            m_trackArtist = v1->artist;
        if (m_trackAlbum.isEmpty() && !v1->album.isEmpty())
            m_trackAlbum = v1->album;
    }

    const QString baseName = metadata::cleanup(QString::fromStdString(file.getFileNameWithoutExtension().toStdString()));
    metadata::filenameHeuristic(baseName, m_trackTitle, m_trackArtist);
}


bool DjEngine::hydrateLibraryStateForTrack(const QString& rawPath, double durationSec)
{
    if (!m_libraryDb)
        return false;

    const int durSec = static_cast<int>(durationSec);
    int bitrateKbps = 0;
    const juce::File file(rawPath.toStdString());
    if (durationSec > 0.0) {
        const auto bytes = static_cast<double>(file.getSize());
        bitrateKbps = static_cast<int>(std::lround((bytes * 8.0) / durationSec / 1000.0));
    }

    // Prefer the existing DB id for this file path so that analysis data and cue points
    // are preserved even when metadata (and thus a freshly-generated hash) has changed.
    const QString existingId = m_libraryDb->trackIdForFilePath(rawPath);
    m_currentTrackId    = existingId.isEmpty()
        ? TrackIdGenerator::generate(m_trackArtist, m_trackTitle, durSec, rawPath)
        : existingId;
    m_playLogged       = false;
    m_playedAccumSec   = 0.0;
    m_playHistoryClock.restart();
    m_libraryDb->addTrack(m_currentTrackId,
                          m_trackTitle, m_trackArtist, durSec, rawPath, bitrateKbps,
                          m_trackGenre, m_trackAlbum, m_trackComment);

    bool hasDbAnalysis = false;
    LibraryDatabase::AnalysisSnapshot cachedAnalysis;
    if (m_libraryDb->tryGetAnalysisData(m_currentTrackId, &cachedAnalysis)
        && cachedAnalysis.isAnalyzed) {
        hasDbAnalysis = true;
        m_currentSegments = m_libraryDb->trackSegmentsForTrack(m_currentTrackId);
        emit segmentsChanged();

        if (cachedAnalysis.bpm > 0.0) {
            m_trackData->setBpmData(cachedAnalysis.bpm,
                                    cachedAnalysis.firstBeatSample,
                                    cachedAnalysis.sampleRate,
                                    cachedAnalysis.beatGrid,
                                    cachedAnalysis.confidence,
                                    cachedAnalysis.beatGridInfo);
        }

        const QString cachedKey = cachedAnalysis.key.trimmed();
        if (!cachedKey.isEmpty()) {
            m_trackKey = cachedKey;
            m_trackData->setKeyData(cachedKey);
        }

        std::vector<TrackSegment> cachedSegments;
        cachedSegments.reserve(static_cast<size_t>(m_currentSegments.size()));
        for (const QVariant& value : m_currentSegments) {
            const QVariantMap map = value.toMap();
            TrackSegment segment;
            segment.label = map.value(QStringLiteral("label")).toString();
            segment.startTime = static_cast<float>(map.value(QStringLiteral("startTime")).toDouble());
            segment.endTime = static_cast<float>(map.value(QStringLiteral("endTime")).toDouble());
            segment.colorHex = map.value(QStringLiteral("colorHex")).toString();
            segment.confidence = static_cast<float>(map.value(QStringLiteral("confidence")).toDouble());
            if (segment.endTime > segment.startTime + 0.01f)
                cachedSegments.push_back(segment);
        }
        if (!cachedSegments.empty())
            m_trackData->setSegmentsData(std::move(cachedSegments));
    } else {
        // Strictly hide segment UI state until fresh analysis writes data.
        m_currentSegments = QVariantList();
        emit segmentsChanged();
    }

    loadHotCuesForCurrentTrack();
    loadSavedLoopsForCurrentTrack();
    loadMainCueForCurrentTrack();
    emit beatgridLockedChanged();
    return hasDbAnalysis;
}


