#include "DjEngineCommonIncludes.h"

#include <QSemaphore>
#include <algorithm>
#include <thread>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif


namespace {

// Yield CPU to the audio callback and Qt UI/render threads while a background
// load decodes. Lowering priority needs no privileges; on Linux the nice value
// is per-task so this affects only this loader thread. No-op elsewhere.
void lowerCurrentThreadPriority()
{
#ifdef __linux__
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 10);
#endif
}

// Bound concurrent background track loads across all decks. Each load opens
// several decoders and scans/decodes audio (overview, silence/auto-cue, cover
// art); four decks loading at once can otherwise saturate the CPU and stall the
// UI and audio callback. The gate is sized to the host core count so it scales
// from low-core ARM64 boards up to many-core x86 desktops without hard-coding.
int maxConcurrentTrackLoads()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        return 2;
    // Leave headroom for the audio device thread, the Qt UI/render thread and
    // the per-deck analyzer threads; clamp to avoid disk I/O thrashing too.
    return std::clamp(static_cast<int>(hw) / 2, 1, 6);
}

QSemaphore& trackLoadGate()
{
    static QSemaphore gate(maxConcurrentTrackLoads());
    return gate;
}

} // namespace


void DjEngine::loadTrack(const QString& rawPath)
{
    juce::File file(rawPath.toStdString());

    if (!file.existsAsFile()) {
        qWarning() << "File does not exist:" << rawPath;
        return;
    }

    const quint64 gen = ++m_loadGen;

    if (m_analyzer)
        m_analyzer->stopAnalysis();

    resetTrackLoadState();
    m_trackFilePath.clear();
    m_trackTitle.clear();   m_trackArtist.clear();  m_trackAlbum.clear();
    m_trackGenre.clear();   m_trackComment.clear();
    m_trackKey.clear();     m_trackDuration.clear(); m_trackDurationSec = 0.0;
    m_hasCoverArt = false; m_coverArtUrl.clear();
    if (m_coverProvider)
        m_coverProvider->clearCover(m_deckId);
    emit trackMetadataChanged();

    const int pps = static_cast<int>(WAVEFORM_POINTS_PER_SECOND);
    auto analysisState = std::make_shared<std::atomic<bool>>(false);
    std::thread([this, rawPath, file, gen, pps, analysisState]() {
        std::lock_guard<std::mutex> loadGuard(m_loadMutex);
        if (m_loadGen != gen)
            return;

        // Throttle total concurrent heavy loads across all decks (see gate above).
        // Held for the whole decode/scan span and auto-released on every exit path.
        trackLoadGate().acquire();
        const QSemaphoreReleaser loadReleaser(trackLoadGate());
        if (m_loadGen != gen)
            return;

        lowerCurrentThreadPriority();

        auto* reader = formatManager.createReaderFor(file);
        if (!reader) {
            qWarning() << "[DjEngine] loadTrack: unsupported or unreadable format:" << rawPath;
            return;
        }
        auto* directReader = formatManager.createReaderFor(file);
        if (!directReader) {
            qWarning() << "[DjEngine] loadTrack: could not create direct reader:" << rawPath;
            delete reader;
            return;
        }

        WaveformCache::Payload cache;
        bool wfLoaded = WaveformCache::loadForFile(rawPath, pps, &cache)
                        && !cache.waveform.isEmpty()
                        && !cache.rgb.isEmpty();
        if (wfLoaded) {
            const int expected = cache.totalExpected > 0 ? cache.totalExpected : cache.waveform.size();
            wfLoaded = expected > 0
                       && cache.waveform.size() >= static_cast<int>(expected * 0.98)
                       && cache.rgb.size()      >= static_cast<int>(expected * 0.98);
        }

        QVector<TrackData::RgbWaveformFrame> instantOvr;
        int instantExpected = 0;
        if (!wfLoaded) {
            if (auto* ovrReader = formatManager.createReaderFor(file)) {
                instantOvr = WaveformAnalyzer::buildInstantOverview(ovrReader);
                const double durationSec =
                    static_cast<double>(ovrReader->lengthInSamples) / ovrReader->sampleRate;
                instantExpected = static_cast<int>(durationSec * pps);
                delete ovrReader;
            }
        }

        QMetaObject::invokeMethod(this,
            [this, gen, reader, directReader, file, rawPath, analysisState]() mutable
            {
                if (m_loadGen != gen) {
                    delete reader;
                    delete directReader;
                    return;
                }

                m_hasTrack = true;
                attachReaderToTransport(reader, directReader);

                populateMetadataFromReader(*reader, rawPath, file);
                const double durationSec =
                    static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
                updateTrackDuration(durationSec);
                clearLoop();

                const bool hasDbAnalysis = hydrateLibraryStateForTrack(rawPath, durationSec);
                analysisState->store(hasDbAnalysis, std::memory_order_relaxed);

                emit trackMetadataChanged();
                emit trackLoaded();
                emit progressChanged();
            },
            Qt::QueuedConnection);

        QMetaObject::invokeMethod(this,
            [this, gen, rawPath,
             cache           = std::move(cache),
             instantOvr        = std::move(instantOvr),
             instantExpected,
             analysisState,
             wfLoaded]() mutable
            {
                if (m_loadGen != gen)
                    return;

                QTimer::singleShot(0, this, [this, gen, rawPath,
                                             cache           = std::move(cache),
                                             instantOvr        = std::move(instantOvr),
                                             instantExpected,
                                             analysisState,
                                             wfLoaded]() mutable
                {
                    if (m_loadGen != gen)
                        return;

                    if (wfLoaded) {
                        const int expected =
                            cache.totalExpected > 0 ? cache.totalExpected : cache.waveform.size();
                        m_trackData->setTotalExpected(expected);
                        m_trackData->replaceAllData(
                            std::move(cache.waveform), std::max(0.001f, cache.globalMaxPeak));
                        m_trackData->setRgbWaveformData(std::move(cache.rgb));
                        if (!cache.peakMip.isEmpty())
                            m_trackData->setPeakMipData(std::move(cache.peakMip));
                    } else if (!instantOvr.isEmpty()) {
                        m_trackData->setTotalExpected(std::max(1, instantExpected));
                        m_trackData->setOverviewRgbData(std::move(instantOvr));
                    }

                    const bool hasDbAnalysis = analysisState->load(std::memory_order_relaxed);
                    if (!(wfLoaded && hasDbAnalysis))
                        m_analyzer->startAnalysis(rawPath, transportSource.getCurrentPosition());
                });
            },
            Qt::QueuedConnection);

        QImage coverImage;
        const QByteArray coverData = CoverArtExtractor::extractCoverArt(rawPath).first;
        if (!coverData.isEmpty())
            coverImage.loadFromData(coverData);

        double autoCueSec = -1.0;
        {
            auto* cueReader = formatManager.createReaderFor(file);
            if (cueReader) {
                const double sr = cueReader->sampleRate;
                if (sr > 0.0) {
                    static constexpr double kMaxScanSec      = 10.0;
                    static constexpr float  kSilenceThreshold = 0.001f;
                    static constexpr int    kBlockSize        = 1024;

                    const juce::int64 maxScan = static_cast<juce::int64>(sr * kMaxScanSec);
                    const int numCh = static_cast<int>(std::max<unsigned int>(cueReader->numChannels, 1u));
                    juce::AudioBuffer<float> buf(numCh, kBlockSize);

                    juce::int64 firstAudibleSample = -1;
                    for (juce::int64 pos = 0; pos < maxScan && firstAudibleSample < 0; pos += kBlockSize) {
                        const int toRead = static_cast<int>(
                            std::min<juce::int64>(kBlockSize, maxScan - pos));
                        buf.clear();
                        cueReader->read(&buf, 0, toRead, pos, true, true);
                        for (int i = 0; i < toRead && firstAudibleSample < 0; ++i) {
                            for (int ch = 0; ch < numCh; ++ch) {
                                if (std::abs(buf.getSample(ch, i)) >= kSilenceThreshold) {
                                    firstAudibleSample = pos + i;
                                    break;
                                }
                            }
                        }
                    }

                    if (firstAudibleSample > 0)
                        autoCueSec = static_cast<double>(firstAudibleSample) / sr;
                }
                delete cueReader;
            }
        }

        QMetaObject::invokeMethod(this,
            [this, gen,
             coverImage = std::move(coverImage),
             autoCueSec]() mutable
            {
                if (m_loadGen != gen)
                    return;

                if (!coverImage.isNull() && m_coverProvider) {
                    m_coverProvider->setCoverImage(m_deckId, coverImage);
                    m_coverArtUrl = QString("image://coverart/%1?t=%2")
                                        .arg(m_deckId)
                                        .arg(QDateTime::currentMSecsSinceEpoch());
                    m_hasCoverArt = true;

                    if (m_libraryCoverService && !m_currentTrackId.isEmpty()) {
                        QByteArray coverBytes;
                        QBuffer coverBuffer(&coverBytes);
                        coverBuffer.open(QIODevice::WriteOnly);
                        if (coverImage.save(&coverBuffer, "JPG"))
                            m_libraryCoverService->publishCover(m_currentTrackId, coverBytes);
                    }

                    emit trackMetadataChanged();
                }

                if (autoCueSec > 0.0
                    && m_mainCueSec < 0.0
                    && !m_playRequested
                    && !transportSource.isPlaying()) {
                    m_mainCueSec = autoCueSec;
                    emit mainCueChanged();
                    transportSource.setPosition(autoCueSec);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}
