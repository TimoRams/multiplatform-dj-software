#include "LibraryPreviewPlayer.h"

#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <algorithm>
#include <cmath>

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

LibraryPreviewPlayer::LibraryPreviewPlayer(QObject* parent)
    : QObject(parent)
{
    m_formatManager.registerBasicFormats();
    m_positionTimer.setInterval(80);
    connect(&m_positionTimer, &QTimer::timeout, this, &LibraryPreviewPlayer::pollPosition);
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
    std::lock_guard lock(m_mutex);
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    m_blockSize = std::max(64, samplesPerBlockExpected);
    m_transport.prepareToPlay(m_blockSize, m_sampleRate);
    m_prepared = true;
}

void LibraryPreviewPlayer::releaseResources()
{
    stopPositionTimer();
    std::lock_guard lock(m_mutex);
    m_transport.stop();
    m_transport.setSource(nullptr);
    m_transport.releaseResources();
    m_readerSource.reset();
    m_reader.reset();
    m_prepared = false;
    m_playing.store(false, std::memory_order_relaxed);
    m_durationSec.store(0.0, std::memory_order_relaxed);
    m_positionSec.store(0.0, std::memory_order_relaxed);
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

void LibraryPreviewPlayer::publishTransportStateLocked()
{
    const double len = m_transport.getLengthInSeconds();
    const double pos = m_transport.getCurrentPosition();
    m_durationSec.store(len, std::memory_order_relaxed);
    m_positionSec.store(pos, std::memory_order_relaxed);
}

void LibraryPreviewPlayer::startPositionTimer()
{
    if (!m_positionTimer.isActive())
        m_positionTimer.start();
}

void LibraryPreviewPlayer::stopPositionTimer()
{
    m_positionTimer.stop();
}

void LibraryPreviewPlayer::pollPosition()
{
    bool playing = false;
    double pos = 0.0;
    double len = 0.0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_playing.load(std::memory_order_relaxed) || !m_reader) {
            stopPositionTimer();
            return;
        }
        playing = m_transport.isPlaying();
        pos = m_transport.getCurrentPosition();
        len = m_transport.getLengthInSeconds();
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

void LibraryPreviewPlayer::beginPreviewLocked(const QString& filePath)
{
    m_transport.stop();
    m_transport.setSource(nullptr);
    m_readerSource.reset();
    m_reader.reset();

    juce::File file(filePath.toStdString());
    if (!file.existsAsFile())
        return;

    m_reader.reset(m_formatManager.createReaderFor(file));
    if (!m_reader)
        return;

    m_readerSource = std::make_unique<juce::AudioFormatReaderSource>(m_reader.get(), false);
    m_transport.setSource(m_readerSource.get(), 0, nullptr, m_reader->sampleRate);

    const double len = m_reader->lengthInSamples / std::max(1.0, m_reader->sampleRate);
    const double start = previewStartSeconds(len);
    m_transport.setPosition(start);
    m_transport.start();

    m_currentPath = filePath;
    m_playing.store(true, std::memory_order_relaxed);
    publishTransportStateLocked();
}

void LibraryPreviewPlayer::finishPreviewLocked()
{
    m_transport.stop();
    m_transport.setSource(nullptr);
    m_readerSource.reset();
    m_reader.reset();
    m_currentPath.clear();
    m_playing.store(false, std::memory_order_relaxed);
    m_durationSec.store(0.0, std::memory_order_relaxed);
    m_positionSec.store(0.0, std::memory_order_relaxed);
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

    auto* watcher = new QFutureWatcher<LoadedPreview>(this);
    connect(watcher, &QFutureWatcher<LoadedPreview>::finished, this, [this, watcher]() {
        const LoadedPreview loaded = watcher->result();
        watcher->deleteLater();

        if (!loaded.reader)
            return;

        {
            std::lock_guard lock(m_mutex);
            m_transport.stop();
            m_transport.setSource(nullptr);
            m_readerSource.reset();
            m_reader = loaded.reader;
            m_readerSource = std::make_unique<juce::AudioFormatReaderSource>(m_reader.get(), false);
            if (m_prepared)
                m_transport.prepareToPlay(m_blockSize, m_sampleRate);
            m_transport.setSource(m_readerSource.get(), 0, nullptr, m_reader->sampleRate);
            m_transport.setPosition(loaded.startSec);
            m_transport.start();
            m_currentPath = loaded.path;
            m_playing.store(true, std::memory_order_relaxed);
            publishTransportStateLocked();
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
        if (!m_reader)
            return;

        const double len = m_transport.getLengthInSeconds();
        if (len <= 0.0)
            return;

        positionSec = std::clamp(positionSec, 0.0, len);
        m_transport.setPosition(positionSec);
        if (m_playing.load(std::memory_order_relaxed) && !m_transport.isPlaying())
            m_transport.start();

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
                                          int numSamples)
{
    if (numSamples <= 0 || !m_playing.load(std::memory_order_relaxed))
        return;

    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_prepared)
        return;

    if (!m_transport.isPlaying())
        return;

    if (scratch.getNumSamples() < numSamples || scratch.getNumChannels() < 2)
        scratch.setSize(2, numSamples, false, false, true);

    scratch.clear();
    juce::AudioSourceChannelInfo info(&scratch, 0, numSamples);
    m_transport.getNextAudioBlock(info);

    const float gain = static_cast<float>(kPreviewGain);
    masterBuf.addFrom(0, 0, scratch, 0, 0, numSamples, gain);
    masterBuf.addFrom(1, 0, scratch, 1, 0, numSamples, gain);
}
