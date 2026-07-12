#include "DjEngineCommonIncludes.h"


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
    m_trackFilePath     = rawPath;
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

