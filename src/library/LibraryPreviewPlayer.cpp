#include "LibraryPreviewPlayer.h"

#include "audio/cache/AudioPageCache.h"
#include "audio/cache/CachedPlaybackAudioSource.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <algorithm>
#include <cmath>
#include <thread>

namespace {

struct LoadedPreview
{
    std::shared_ptr<juce::AudioFormatReader> reader;
    double startSec = 0.0;
    QString path;
};

LoadedPreview loadPreviewFile(const QString& filePath)
{
    LoadedPreview result;
    result.path = filePath;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    juce::File file(filePath.toStdString());
    if (!file.existsAsFile())
        return result;

    result.reader.reset(formatManager.createReaderFor(file));
    if (!result.reader)
        return result;

    const double len = result.reader->lengthInSamples
                           / std::max(1.0, result.reader->sampleRate);
    result.startSec = std::clamp(len * 0.25, 0.0, std::max(0.0, len - 1.0));
    if (len > 30.0)
        result.startSec = std::max(result.startSec, 30.0);
    result.startSec = std::min(result.startSec, std::max(0.0, len - 0.5));

    return result;
}

} // namespace

LibraryPreviewPlayer::LibraryPreviewPlayer(ControlClock& controlClock,
                                           AudioPageCache& cache,
                                           QObject* parent)
    : QObject(parent)
    , m_cache(cache)
{
    ControlClock::Callbacks callbacks;
    callbacks.statistics = [this](const ControlTickContext&) {
        if (m_positionPollingEnabled)
            pollPosition();
    };
    m_clockRegistration = controlClock.registerCallbacks(std::move(callbacks));
}

LibraryPreviewPlayer::~LibraryPreviewPlayer()
{
    stop();
}

QString LibraryPreviewPlayer::currentPath() const
{
    std::lock_guard lock(m_mutex);
    return m_currentPath;
}

double LibraryPreviewPlayer::progress() const
{
    const double dur = m_durationSec.load(std::memory_order_relaxed);
    if (dur <= 0.0)
        return 0.0;
    return std::clamp(m_positionSec.load(std::memory_order_relaxed) / dur, 0.0, 1.0);
}

void LibraryPreviewPlayer::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    (void) samplesPerBlockExpected;
    (void) sampleRate;
    std::lock_guard lock(m_mutex);
    if (m_readerSource)
        m_readerSource->prepareToPlay(samplesPerBlockExpected, sampleRate);
    m_prepared.store(true, std::memory_order_release);
}

void LibraryPreviewPlayer::releaseResources()
{
    stopPositionTimer();
    std::lock_guard lock(m_mutex);
    finishPreviewLocked();
    m_prepared.store(false, std::memory_order_release);
}

void LibraryPreviewPlayer::prepareAuxAudio(int maximumBlockSize, double sampleRate)
{
    prepareToPlay(maximumBlockSize, sampleRate);
}

void LibraryPreviewPlayer::releaseAuxAudio()
{
    releaseResources();
}

void LibraryPreviewPlayer::mixAuxAudio(juce::AudioBuffer<float>& masterBuffer,
                                       juce::AudioBuffer<float>& scratchBuffer,
                                       int numberOfSamples) noexcept
{
    mixIntoOutputs(masterBuffer, scratchBuffer, 0, numberOfSamples);
}

double LibraryPreviewPlayer::previewStartSeconds(double trackLengthSec) const
{
    if (trackLengthSec <= 0.0)
        return 0.0;
    double start = trackLengthSec * 0.25;
    if (trackLengthSec > 30.0)
        start = std::max(start, 30.0);
    return std::clamp(start, 0.0, std::max(0.0, trackLengthSec - 0.5));
}

void LibraryPreviewPlayer::startPositionTimer()
{
    m_positionPollingEnabled = true;
}

void LibraryPreviewPlayer::stopPositionTimer()
{
    m_positionPollingEnabled = false;
}

void LibraryPreviewPlayer::pollPosition()
{
    bool playing = false;
    double pos = 0.0;
    double len = 0.0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_playing.load(std::memory_order_relaxed) || !m_readerSource) {
            stopPositionTimer();
            return;
        }
        playing = true;
        pos = static_cast<double>(m_readerSource->getNextReadPosition())
            / std::max(1.0, m_cacheHandle.sampleRate());
        len = static_cast<double>(m_cacheHandle.lengthInSamples())
            / std::max(1.0, m_cacheHandle.sampleRate());
        m_positionSec.store(pos, std::memory_order_relaxed);
        if (len > 0.0)
            m_durationSec.store(len, std::memory_order_relaxed);
    }

    emit positionChanged();

    if (len > 0.0 && pos >= len - 0.02) {
        stop();
        return;
    }

    if (!playing && m_playing.load(std::memory_order_relaxed))
        stop();
}

void LibraryPreviewPlayer::finishPreviewLocked()
{
    m_playing.store(false, std::memory_order_release);
    m_audioReader.store(nullptr, std::memory_order_release);
    waitForAudioReaders();
    if (m_readerSource)
        m_readerSource->releaseResources();
    m_readerSource.reset();
    m_cache.releaseTrack(m_cacheHandle);
    m_cacheHandle = {};
    m_currentPath.clear();
    m_durationSec.store(0.0, std::memory_order_relaxed);
    m_positionSec.store(0.0, std::memory_order_relaxed);
}

void LibraryPreviewPlayer::waitForAudioReaders() const noexcept
{
    while (m_activeAudioReaders.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
}

void LibraryPreviewPlayer::preview(const QString& filePath)
{
    if (filePath.isEmpty())
        return;

    {
        std::lock_guard lock(m_mutex);
        if (m_currentPath == filePath && m_playing.load(std::memory_order_relaxed)) {
            finishPreviewLocked();
            stopPositionTimer();
            emit playingChanged();
            emit currentPathChanged();
            emit durationChanged();
            emit positionChanged();
            return;
        }
    }

    stop();
    const auto loadGeneration = m_loadGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

    auto* watcher = new QFutureWatcher<LoadedPreview>(this);
    connect(watcher, &QFutureWatcher<LoadedPreview>::finished, this,
            [this, watcher, loadGeneration]() {
        const LoadedPreview loaded = watcher->result();
        watcher->deleteLater();

        if (!loaded.reader
            || loadGeneration != m_loadGeneration.load(std::memory_order_acquire))
            return;

        auto cacheHandle = m_cache.openTrack({loaded.path});
        if (!cacheHandle.isValid())
            return;
        auto readerSource = std::make_unique<CachedPlaybackAudioSource>(m_cache, cacheHandle);
        readerSource->setCommandedReadPosition(static_cast<juce::int64>(
            std::llround(loaded.startSec * cacheHandle.sampleRate())));

        {
            std::lock_guard lock(m_mutex);
            finishPreviewLocked();
            m_cacheHandle = cacheHandle;
            m_readerSource = std::move(readerSource);
            m_currentPath = loaded.path;
            m_durationSec.store(static_cast<double>(m_cacheHandle.lengthInSamples())
                                    / std::max(1.0, m_cacheHandle.sampleRate()),
                                std::memory_order_relaxed);
            m_positionSec.store(loaded.startSec, std::memory_order_relaxed);
            m_audioReader.store(m_readerSource.get(), std::memory_order_release);
            m_playing.store(true, std::memory_order_release);
        }

        startPositionTimer();
        emit playingChanged();
        emit currentPathChanged();
        emit durationChanged();
        emit positionChanged();
    });

    watcher->setFuture(QtConcurrent::run(loadPreviewFile, filePath));
}

void LibraryPreviewPlayer::togglePreview(const QString& filePath)
{
    preview(filePath);
}

void LibraryPreviewPlayer::stop()
{
    m_loadGeneration.fetch_add(1, std::memory_order_acq_rel);
    bool changed = false;
    stopPositionTimer();
    {
        std::lock_guard lock(m_mutex);
        if (m_playing.load(std::memory_order_relaxed) || !m_currentPath.isEmpty()) {
            finishPreviewLocked();
            changed = true;
        }
    }
    if (changed) {
        emit playingChanged();
        emit currentPathChanged();
        emit durationChanged();
        emit positionChanged();
    }
}

void LibraryPreviewPlayer::seekSeconds(double positionSec)
{
    bool moved = false;
    {
        std::lock_guard lock(m_mutex);
        if (!m_readerSource)
            return;

        const double len = static_cast<double>(m_cacheHandle.lengthInSamples())
            / std::max(1.0, m_cacheHandle.sampleRate());
        if (len <= 0.0)
            return;

        positionSec = std::clamp(positionSec, 0.0, len);
        m_readerSource->setCommandedReadPosition(static_cast<juce::int64>(
            std::llround(positionSec * m_cacheHandle.sampleRate())));

        m_positionSec.store(positionSec, std::memory_order_relaxed);
        moved = true;
    }
    if (moved)
        emit positionChanged();
}

void LibraryPreviewPlayer::seekProgress(double progress)
{
    const double dur = m_durationSec.load(std::memory_order_relaxed);
    if (dur <= 0.0)
        return;
    seekSeconds(std::clamp(progress, 0.0, 1.0) * dur);
}

void LibraryPreviewPlayer::mixIntoOutputs(juce::AudioBuffer<float>& masterBuf,
                                          juce::AudioBuffer<float>& scratch,
                                          int startSample,
                                          int numSamples) noexcept
{
    if (numSamples <= 0 || !m_playing.load(std::memory_order_relaxed))
        return;

    m_activeAudioReaders.fetch_add(1, std::memory_order_acq_rel);
    auto* reader = m_audioReader.load(std::memory_order_acquire);
    if (!reader || !m_prepared.load(std::memory_order_acquire)) {
        m_activeAudioReaders.fetch_sub(1, std::memory_order_release);
        return;
    }

    if (scratch.getNumSamples() < numSamples || scratch.getNumChannels() < 2) {
        m_activeAudioReaders.fetch_sub(1, std::memory_order_release);
        return;
    }

    scratch.clear(0, 0, numSamples);
    scratch.clear(1, 0, numSamples);
    juce::AudioSourceChannelInfo info(&scratch, 0, numSamples);
    reader->getNextAudioBlock(info);
    m_positionSec.store(static_cast<double>(reader->getNextReadPosition())
                            / std::max(1.0, m_cacheHandle.sampleRate()),
                        std::memory_order_relaxed);

    const float gain = static_cast<float>(kPreviewGain);
    masterBuf.addFrom(0, startSample, scratch, 0, 0, numSamples, gain);
    masterBuf.addFrom(1, startSample, scratch, 1, 0, numSamples, gain);
    m_activeAudioReaders.fetch_sub(1, std::memory_order_release);
}
