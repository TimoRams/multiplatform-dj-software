#include "SetRecorder.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {
QByteArray waveHeader(quint32 rate, quint32 dataBytes)
{
    QByteArray header(44, '\0');
    std::memcpy(header.data(), "RIFF", 4);
    qToLittleEndian<quint32>(dataBytes + 36, header.data() + 4);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, header.data() + 16);
    qToLittleEndian<quint16>(1, header.data() + 20);
    qToLittleEndian<quint16>(2, header.data() + 22);
    qToLittleEndian<quint32>(rate, header.data() + 24);
    qToLittleEndian<quint32>(rate * 6, header.data() + 28);
    qToLittleEndian<quint16>(6, header.data() + 32);
    qToLittleEndian<quint16>(24, header.data() + 34);
    std::memcpy(header.data() + 36, "data", 4);
    qToLittleEndian<quint32>(dataBytes, header.data() + 40);
    return header;
}
}
SetRecorder::SetRecorder(QObject* parent) : QObject(parent), m_ring(kCapacity * 2)
{
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &SetRecorder::changed);
}
SetRecorder::~SetRecorder() { stopRecording(); }
void SetRecorder::configure(double rate) noexcept
{
    const double previous = m_rate.exchange(rate, std::memory_order_acq_rel);
    if (m_running.load(std::memory_order_acquire) && (rate <= 0 || rate != previous)) {
        m_failure.store(3, std::memory_order_relaxed);
        m_accepting.store(false, std::memory_order_release);
    }
}
QString SetRecorder::error() const
{
    switch (m_failure.load(std::memory_order_relaxed)) {
    case 1: return QStringLiteral("Recording stopped: disk write failed. Check storage and free space.");
    case 2: return QStringLiteral("Recording stopped: storage could not keep up. Playback was preserved.");
    case 3: return QStringLiteral("Recording stopped: audio device or sample rate changed.");
    default: return m_startError;
    }
}
double SetRecorder::elapsedSeconds() const
{ return m_recordingRate > 0 ? m_frames.load(std::memory_order_relaxed) / m_recordingRate : 0; }
double SetRecorder::freeSpaceMb() const
{ return m_path.isEmpty() ? 0 : std::max<qint64>(0, QStorageInfo(QFileInfo(m_path).absolutePath()).bytesAvailable()) / 1048576.0; }
bool SetRecorder::startRecording(const QString& directory)
{
    if (recording()) return false;
    stopRecording();
    m_startError.clear(); m_failure.store(0);
    m_recordingRate = m_rate.load(std::memory_order_acquire);
    if (!std::isfinite(m_recordingRate) || m_recordingRate < 8000 || m_recordingRate > 384000) {
        m_startError = QStringLiteral("Start the audio engine before recording"); emit changed(); return false;
    }
    const QString folder = directory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/BrockDJ/Recordings" : directory;
    if (!QDir().mkpath(folder) || QStorageInfo(folder).bytesAvailable() < 16 * 1024 * 1024) {
        m_startError = QStringLiteral("Choose a writable recording folder with at least 16 MiB free"); emit changed(); return false;
    }
    m_path = QDir(folder).absoluteFilePath("Set-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".wav");
    m_written.store(0); m_read.store(0); m_frames.store(0); m_peak.store(0); m_markAt.store(UINT64_MAX);
    m_accepting.store(true); m_running.store(true);
    m_writer = std::thread(&SetRecorder::writeLoop, this, m_path, m_recordingRate);
    m_timer.start(); emit changed(); return true;
}
void SetRecorder::stopRecording()
{
    m_accepting.store(false, std::memory_order_release);
    while (m_readers.load(std::memory_order_acquire)) std::this_thread::yield();
    if (m_writer.joinable()) m_writer.join();
    m_running.store(false); m_timer.stop(); emit changed();
}
void SetRecorder::markTrack()
{
    if (!recording()) return;
    std::uint64_t empty = UINT64_MAX;
    m_markAt.compare_exchange_strong(empty, m_written.load(std::memory_order_acquire), std::memory_order_release);
}
void SetRecorder::capture(const juce::AudioBuffer<float>& master, int samples) noexcept
{
    m_readers.fetch_add(1, std::memory_order_acquire);
    if (!m_accepting.load(std::memory_order_acquire)) { m_readers.fetch_sub(1, std::memory_order_release); return; }
    if (m_rate.load(std::memory_order_acquire) != m_recordingRate) {
        m_failure.store(3); m_accepting.store(false, std::memory_order_release);
        m_readers.fetch_sub(1, std::memory_order_release); return;
    }
    const auto write = m_written.load(std::memory_order_relaxed);
    if (samples < 0 || write - m_read.load(std::memory_order_acquire) + static_cast<unsigned>(samples) > kCapacity) {
        m_failure.store(2); m_accepting.store(false, std::memory_order_release);
        m_readers.fetch_sub(1, std::memory_order_release); return;
    }
    float peakValue = 0;
    for (int i = 0; i < samples; ++i) {
        for (int channel = 0; channel < 2; ++channel) {
            const float v = master.getSample(channel, i);
            m_ring[((write + i) % kCapacity) * 2 + channel] = v;
            peakValue = std::max(peakValue, std::abs(v));
        }
    }
    m_peak.store(peakValue, std::memory_order_relaxed);
    m_written.store(write + samples, std::memory_order_release);
    m_readers.fetch_sub(1, std::memory_order_release);
}
void SetRecorder::writeLoop(QString basePath, double rate)
{
    QFile file;
    quint32 bytes = 0;
    int part = 0;
    const auto header = [&] {
        const qint64 end = 44 + bytes;
        if (!file.resize(end) || !file.seek(0) || file.write(waveHeader(static_cast<quint32>(rate), bytes)) != 44 || !file.flush()) return false;
        return file.seek(std::max<qint64>(44, end));
    };
    const auto open = [&] {
        QString path = basePath;
        if (part) path.insert(path.size() - 4, QString("-%1").arg(part + 1, 3, 10, QChar('0')));
        file.setFileName(path); bytes = 0;
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(waveHeader(static_cast<quint32>(rate), 0)) == 44;
    };
    bool good = open();
    QByteArray data(4096 * 6, '\0');
    quint64 lastHeader = 0;
    for (;;) {
        const auto read = m_read.load(std::memory_order_relaxed);
        const auto available = m_written.load(std::memory_order_acquire) - read;
        if (!good) { m_failure.store(1); m_accepting.store(false); break; }
        if (!available) {
            if (!m_accepting.load(std::memory_order_acquire) && !m_readers.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(3)); continue;
        }
        const auto marker = m_markAt.load(std::memory_order_acquire);
        if (read >= marker) m_markAt.store(UINT64_MAX, std::memory_order_release);
        if (bytes >= 0x70000000u || (read >= marker && bytes > 0)) {
            good = header(); file.close(); ++part;
            if (good) good = open();
            if (!good) continue;
        }
        const auto count = std::min<std::uint64_t>({4096, available,
            marker > read ? marker - read : UINT64_MAX});
        for (quint64 i = 0; i < count * 2; ++i) {
            const float v = m_ring[((read * 2) + i) % (kCapacity * 2)];
            const auto sample = static_cast<qint32>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f) * 8388607.0f));
            for (int b = 0; b < 3; ++b) data[static_cast<qsizetype>(i * 3 + b)] = static_cast<char>((static_cast<quint32>(sample) >> (b * 8)) & 0xff);
        }
        const auto written = file.write(data.constData(), count * 6);
        good = written == static_cast<qint64>(count * 6);
        const auto savedFrames = std::max<qint64>(0, written) / 6;
        bytes += static_cast<quint32>(savedFrames * 6);
        m_read.store(read + count, std::memory_order_release);
        m_frames.fetch_add(savedFrames);
        // Periodic valid headers leave usable audio after an interrupted session.
        if (m_frames.load() - lastHeader >= static_cast<quint64>(rate)) {
            good = good && header(); lastHeader = m_frames.load();
        }
    }
    if (file.isOpen()) { if (!header()) m_failure.store(1); file.close(); }
    m_running.store(false, std::memory_order_release);
}
