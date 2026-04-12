#include "WaveformCache.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtGlobal>

namespace {

constexpr quint32 kMagic = 0x52574631; // RWF1
constexpr qint32 kVersion = 2;
constexpr int kBlockSize = 4096;

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
constexpr auto kDataStreamVersion = QDataStream::Qt_6_5;
#elif QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
constexpr auto kDataStreamVersion = QDataStream::Qt_6_4;
#else
constexpr auto kDataStreamVersion = QDataStream::Qt_6_0;
#endif

QString cacheBaseDir()
{
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDir.isEmpty())
        configDir = QDir::homePath() + "/.config";

    QDir dir(configDir + "/waveform_cache");
    if (!dir.exists())
        dir.mkpath(".");
    return dir.absolutePath();
}

QString cacheKeyFor(const QString& filePath, int pointsPerSecond)
{
    QFileInfo fi(filePath);
    const QString src = QString("%1|%2|%3|%4")
        .arg(fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath())
        .arg(fi.size())
        .arg(fi.lastModified().toMSecsSinceEpoch())
        .arg(pointsPerSecond);

    const QByteArray hash = QCryptographicHash::hash(src.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(hash.left(24));
}

void fillLegacyFields(TrackData::FrequencyData* d)
{
    if (!d)
        return;

    d->lowEnv = d->low;
    d->lowPeak = d->low;
    d->lowRms = d->low;

    d->midEnv = d->mid;
    d->midPeak = d->mid;
    d->midRms = d->mid;

    d->highEnv = d->high;
    d->highPeak = d->high;
    d->highRms = d->high;

    d->transientDelta = 0.0f;
}

} // namespace

QString WaveformCache::cachePathFor(const QString& filePath, int pointsPerSecond)
{
    const QString base = cacheBaseDir();
    const QString key = cacheKeyFor(filePath, pointsPerSecond);
    return QDir(base).filePath(key + ".bin");
}

bool WaveformCache::loadForFile(const QString& filePath, int pointsPerSecond, Payload* out)
{
    if (!out)
        return false;

    QFile f(cachePathFor(filePath, pointsPerSecond));
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return false;

    QDataStream in(&f);
    in.setVersion(kDataStreamVersion);

    quint32 magic = 0;
    qint32 version = 0;
    qint32 pps = 0;
    qint32 totalExpected = 0;
    float globalMaxPeak = 0.001f;
    qint32 wfCount = 0;
    qint32 rgbCount = 0;

    in >> magic >> version >> pps >> totalExpected >> globalMaxPeak >> wfCount >> rgbCount;
    if (in.status() != QDataStream::Ok)
        return false;
    if (magic != kMagic || version != kVersion || pps != pointsPerSecond)
        return false;
    if (wfCount < 0 || rgbCount < 0)
        return false;

    Payload payload;
    payload.pointsPerSecond = pps;
    payload.totalExpected = totalExpected;
    payload.globalMaxPeak = globalMaxPeak;
    payload.waveform.reserve(wfCount);
    payload.rgb.reserve(rgbCount);

    int wfRead = 0;
    while (wfRead < wfCount) {
        const int n = std::min(kBlockSize, wfCount - wfRead);
        for (int i = 0; i < n; ++i) {
            float low = 0.0f;
            float lowMid = 0.0f;
            float mid = 0.0f;
            float high = 0.0f;
            in >> low >> lowMid >> mid >> high;

            TrackData::FrequencyData d;
            d.low = low;
            d.lowMid = lowMid;
            d.mid = mid;
            d.high = high;
            fillLegacyFields(&d);
            payload.waveform.push_back(d);
        }
        wfRead += n;
    }

    int rgbRead = 0;
    while (rgbRead < rgbCount) {
        const int n = std::min(kBlockSize, rgbCount - rgbRead);
        for (int i = 0; i < n; ++i) {
            float rms = 0.0f;
            float low = 0.0f;
            float mid = 0.0f;
            float high = 0.0f;
            quint8 r = 255;
            quint8 g = 255;
            quint8 b = 255;
            in >> rms >> low >> mid >> high >> r >> g >> b;

            TrackData::RgbWaveformFrame frame;
            frame.rms = rms;
            frame.low = low;
            frame.mid = mid;
            frame.high = high;
            frame.color = QColor(r, g, b, 230);
            payload.rgb.push_back(frame);
        }
        rgbRead += n;
    }

    if (in.status() != QDataStream::Ok)
        return false;

    *out = std::move(payload);
    return true;
}

bool WaveformCache::saveForFile(const QString& filePath, const Payload& payload)
{
    if (payload.waveform.isEmpty() || payload.rgb.isEmpty() || payload.pointsPerSecond <= 0)
        return false;

    QSaveFile f(cachePathFor(filePath, payload.pointsPerSecond));
    if (!f.open(QIODevice::WriteOnly))
        return false;

    QDataStream out(&f);
    out.setVersion(kDataStreamVersion);

    out << static_cast<quint32>(kMagic)
        << static_cast<qint32>(kVersion)
        << static_cast<qint32>(payload.pointsPerSecond)
        << static_cast<qint32>(payload.totalExpected)
        << payload.globalMaxPeak
        << static_cast<qint32>(payload.waveform.size())
        << static_cast<qint32>(payload.rgb.size());

    const qsizetype wfTotal = payload.waveform.size();
    qsizetype wfWritten = 0;
    while (wfWritten < wfTotal) {
        const int n = std::min(kBlockSize, static_cast<int>(wfTotal - wfWritten));
        for (int i = 0; i < n; ++i) {
            const auto& d = payload.waveform[wfWritten + i];
            out << d.low << d.lowMid << d.mid << d.high;
        }
        wfWritten += n;
    }

    const qsizetype rgbTotal = payload.rgb.size();
    qsizetype rgbWritten = 0;
    while (rgbWritten < rgbTotal) {
        const int n = std::min(kBlockSize, static_cast<int>(rgbTotal - rgbWritten));
        for (int i = 0; i < n; ++i) {
            const auto& fr = payload.rgb[rgbWritten + i];
            out << fr.rms
                << fr.low
                << fr.mid
                << fr.high
                << static_cast<quint8>(fr.color.red())
                << static_cast<quint8>(fr.color.green())
                << static_cast<quint8>(fr.color.blue());
        }
        rgbWritten += n;
    }

    if (out.status() != QDataStream::Ok)
        return false;

    return f.commit();
}
