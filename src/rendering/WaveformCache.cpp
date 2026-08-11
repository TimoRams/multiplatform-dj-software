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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace {

constexpr quint32 kMagic = 0x52574631; // RWF1
constexpr qint32 kVersion = 6;         // v6: analysis pipeline version bumped; binary payload stays v5-compatible.
constexpr quint32 kRenderMagic = 0x52574c31; // RWL1
constexpr qint32 kRenderVersion = 1;
constexpr int kBlockSize = 4096;
constexpr qint32 kMaxCachedBins = 100'000'000;
constexpr int kRenderOverviewBins = 4096;
constexpr qint64 kFullPayloadMaximumDurationSeconds = 10LL * 60LL;
constexpr qint64 kRenderHeaderBytes = 6 * sizeof(qint32);
constexpr qint64 kRenderOverviewRecordBytes = 5;
constexpr qint64 kRenderLineRecordBytes = 8;

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

quint8 quantizeUnit(float value)
{
    return static_cast<quint8>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

float dequantizeUnit(quint8 value)
{
    return static_cast<float>(value) / 255.0f;
}

bool readRenderHeader(QFile& file, int expectedPointsPerSecond,
                      qint32& totalLines, qint32& chunkSize,
                      qint32& overviewCount)
{
    if (!file.isOpen() || !file.seek(0))
        return false;
    QDataStream in(&file);
    in.setVersion(kDataStreamVersion);
    quint32 magic = 0;
    qint32 version = 0;
    qint32 pointsPerSecond = 0;
    in >> magic >> version >> pointsPerSecond >> totalLines
       >> chunkSize >> overviewCount;
    if (in.status() != QDataStream::Ok
        || magic != kRenderMagic || version != kRenderVersion
        || pointsPerSecond != expectedPointsPerSecond
        || totalLines <= 0 || totalLines > kMaxCachedBins
        || chunkSize != static_cast<qint32>(WaveformLineStore::kChunkSize)
        || overviewCount <= 0 || overviewCount > kRenderOverviewBins) {
        return false;
    }
    const qint64 expectedSize = kRenderHeaderBytes
        + static_cast<qint64>(overviewCount) * kRenderOverviewRecordBytes
        + static_cast<qint64>(totalLines) * kRenderLineRecordBytes;
    return file.size() == expectedSize;
}

bool saveRenderCache(const QString& path, const WaveformCache::Payload& payload)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&file);
    out.setVersion(kDataStreamVersion);

    const int total = payload.rgb.size();
    const int overviewCount = std::min(kRenderOverviewBins, total);
    out << static_cast<quint32>(kRenderMagic)
        << static_cast<qint32>(kRenderVersion)
        << static_cast<qint32>(payload.pointsPerSecond)
        << static_cast<qint32>(total)
        << static_cast<qint32>(WaveformLineStore::kChunkSize)
        << static_cast<qint32>(overviewCount);

    for (int bin = 0; bin < overviewCount; ++bin) {
        const int begin = static_cast<int>(
            (static_cast<qint64>(bin) * total) / overviewCount);
        const int end = std::max(begin + 1, static_cast<int>(
            (static_cast<qint64>(bin + 1) * total) / overviewCount));
        float rms = 0.0f, low = 0.0f, lowMid = 0.0f, mid = 0.0f, high = 0.0f;
        for (int index = begin; index < std::min(end, total); ++index) {
            const auto& frame = payload.rgb[index];
            rms = std::max(rms, frame.rms);
            low = std::max(low, frame.low);
            lowMid = std::max(lowMid, frame.lowMid);
            mid = std::max(mid, frame.mid);
            high = std::max(high, frame.high);
        }
        out << quantizeUnit(rms) << quantizeUnit(low)
            << quantizeUnit(lowMid) << quantizeUnit(mid)
            << quantizeUnit(high);
    }

    for (int index = 0; index < total; ++index) {
        const auto line = waveform::makeCanonicalLine(
            payload.rgb, payload.peakMip, index);
        out << static_cast<qint16>(line.minimum)
            << static_cast<qint16>(line.maximum)
            << static_cast<quint8>(line.red)
            << static_cast<quint8>(line.green)
            << static_cast<quint8>(line.blue)
            << static_cast<quint8>(line.flags);
    }
    if (out.status() != QDataStream::Ok)
        return false;
    return file.commit();
}

} // namespace

QString WaveformCache::cachePathFor(const QString& filePath, int pointsPerSecond)
{
    const QString base = cacheBaseDir();
    const QString key = cacheKeyFor(filePath, pointsPerSecond);
    return QDir(base).filePath(key + ".bin");
}

QString WaveformCache::renderCachePathFor(const QString& filePath, int pointsPerSecond)
{
    const QString base = cacheBaseDir();
    const QString key = cacheKeyFor(filePath, pointsPerSecond);
    return QDir(base).filePath(key + ".lines");
}

bool WaveformCache::inspectRenderCache(const QString& filePath,
                                       int pointsPerSecond,
                                       RenderInfo* out)
{
    if (!out)
        return false;
    QFile file(renderCachePathFor(filePath, pointsPerSecond));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    qint32 totalLines = 0;
    qint32 chunkSize = 0;
    qint32 overviewCount = 0;
    if (!readRenderHeader(file, pointsPerSecond, totalLines,
                          chunkSize, overviewCount)) {
        return false;
    }

    QDataStream in(&file);
    in.setVersion(kDataStreamVersion);
    RenderInfo info;
    info.pointsPerSecond = pointsPerSecond;
    info.totalLines = totalLines;
    info.overview.reserve(overviewCount);
    for (int index = 0; index < overviewCount; ++index) {
        quint8 rms = 0, low = 0, lowMid = 0, mid = 0, high = 0;
        in >> rms >> low >> lowMid >> mid >> high;
        TrackData::RgbWaveformFrame frame;
        frame.rms = dequantizeUnit(rms);
        frame.low = dequantizeUnit(low);
        frame.lowMid = dequantizeUnit(lowMid);
        frame.mid = dequantizeUnit(mid);
        frame.high = dequantizeUnit(high);
        info.overview.push_back(frame);
    }
    if (in.status() != QDataStream::Ok)
        return false;
    *out = std::move(info);
    return true;
}

bool WaveformCache::streamRenderCache(
    const QString& filePath,
    int pointsPerSecond,
    const std::function<bool()>& shouldCancel,
    const std::function<double()>& seekHintSeconds,
    const RenderChunkCallback& publishChunk)
{
    if (!publishChunk)
        return false;
    QFile file(renderCachePathFor(filePath, pointsPerSecond));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    qint32 totalLines = 0;
    qint32 chunkSize = 0;
    qint32 overviewCount = 0;
    if (!readRenderHeader(file, pointsPerSecond, totalLines,
                          chunkSize, overviewCount)) {
        return false;
    }

    const int chunkCount = (totalLines + chunkSize - 1) / chunkSize;
    std::vector<bool> loaded(static_cast<size_t>(chunkCount), false);
    int loadedCount = 0;
    int sequentialCursor = 0;
    constexpr int kPriorityForwardChunks = 8;
    const qint64 linesOffset = kRenderHeaderBytes
        + static_cast<qint64>(overviewCount) * kRenderOverviewRecordBytes;

    while (loadedCount < chunkCount) {
        if (shouldCancel && shouldCancel())
            return false;

        const double hintSeconds = seekHintSeconds ? seekHintSeconds() : 0.0;
        const int hintLine = std::clamp(
            static_cast<int>(std::max(0.0, hintSeconds) * pointsPerSecond),
            0, totalLines - 1);
        const int hintChunk = hintLine / chunkSize;
        int selected = -1;
        if (!loaded[static_cast<size_t>(hintChunk)])
            selected = hintChunk;
        for (int offset = 1;
             selected < 0 && offset < kPriorityForwardChunks;
             ++offset) {
            const int candidate = hintChunk + offset;
            if (candidate < chunkCount
                && !loaded[static_cast<size_t>(candidate)]) {
                selected = candidate;
            }
        }
        const int behind = hintChunk - 1;
        if (selected < 0 && behind >= 0
            && !loaded[static_cast<size_t>(behind)]) {
            selected = behind;
        }
        while (selected < 0 && sequentialCursor < chunkCount) {
            if (!loaded[static_cast<size_t>(sequentialCursor)])
                selected = sequentialCursor;
            ++sequentialCursor;
        }
        if (selected < 0)
            break;

        const int firstLine = selected * chunkSize;
        const int count = std::min(chunkSize, totalLines - firstLine);
        const qint64 offset = linesOffset
            + static_cast<qint64>(firstLine) * kRenderLineRecordBytes;
        if (!file.seek(offset))
            return false;
        QDataStream in(&file);
        in.setVersion(kDataStreamVersion);
        auto lines = std::make_shared<std::vector<WaveformLine>>(
            static_cast<size_t>(count));
        for (int local = 0; local < count; ++local) {
            qint16 minimum = 0, maximum = 0;
            quint8 red = 0, green = 0, blue = 0, flags = 0;
            in >> minimum >> maximum >> red >> green >> blue >> flags;
            auto& line = (*lines)[static_cast<size_t>(local)];
            line.minimum = minimum;
            line.maximum = maximum;
            line.red = red;
            line.green = green;
            line.blue = blue;
            line.flags = flags;
        }
        if (in.status() != QDataStream::Ok)
            return false;

        loaded[static_cast<size_t>(selected)] = true;
        ++loadedCount;
        publishChunk(firstLine, totalLines, std::move(lines));
        // Do not flood the Qt owner queue when a warm SSD can read thousands of
        // small chunks per second. The current/playhead window is still first.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return loadedCount == chunkCount;
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
    qint32 peakCount = 0;

    in >> magic >> version >> pps >> totalExpected >> globalMaxPeak >> wfCount >> rgbCount >> peakCount;
    if (in.status() != QDataStream::Ok)
        return false;
    if (magic != kMagic || (version != 5 && version != kVersion) || pps != pointsPerSecond)
        return false;
    if (totalExpected <= 0 || totalExpected > kMaxCachedBins
        || wfCount != totalExpected || rgbCount != totalExpected
        || peakCount < 0 || peakCount > kMaxCachedBins
        || !std::isfinite(globalMaxPeak) || globalMaxPeak <= 0.0f) {
        return false;
    }

    Payload payload;
    payload.pointsPerSecond = pps;
    payload.totalExpected = totalExpected;
    payload.globalMaxPeak = globalMaxPeak;
    payload.waveform.reserve(wfCount);
    payload.rgb.reserve(rgbCount);
    payload.peakMip.reserve(peakCount);

    int wfRead = 0;
    while (wfRead < wfCount) {
        const int n = std::min(kBlockSize, wfCount - wfRead);
        for (int i = 0; i < n; ++i) {
            float low = 0.0f;
            float lowMid = 0.0f;
            float mid = 0.0f;
            float high = 0.0f;
            in >> low >> lowMid >> mid >> high;

            TrackData::WaveformBin d;
            d.low = low;
            d.lowMid = lowMid;
            d.mid = mid;
            d.high = high;
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
            float lowMid = 0.0f;
            float mid = 0.0f;
            float high = 0.0f;
            quint8 r = 255;
            quint8 g = 255;
            quint8 b = 255;
            in >> rms >> low >> lowMid >> mid >> high >> r >> g >> b;

            TrackData::RgbWaveformFrame frame;
            frame.rms    = rms;
            frame.low    = low;
            frame.lowMid = lowMid;
            frame.mid    = mid;
            frame.high   = high;
            frame.color  = QColor(r, g, b, 230);
            payload.rgb.push_back(frame);
        }
        rgbRead += n;
    }

    int peakRead = 0;
    while (peakRead < peakCount) {
        const int n = std::min(kBlockSize, peakCount - peakRead);
        for (int i = 0; i < n; ++i) {
            qint8 minV = 0;
            qint8 maxV = 0;
            in >> minV >> maxV;
            payload.peakMip.push_back(TrackData::PeakFrame{ minV, maxV });
        }
        peakRead += n;
    }

    if (in.status() != QDataStream::Ok || !f.atEnd())
        return false;

    *out = std::move(payload);
    return true;
}

bool WaveformCache::saveForFile(const QString& filePath, const Payload& payload)
{
    if (payload.waveform.isEmpty() || payload.rgb.isEmpty()
        || payload.pointsPerSecond <= 0
        || payload.totalExpected <= 0
        || payload.totalExpected != payload.waveform.size()
        || payload.totalExpected != payload.rgb.size()
        || !std::isfinite(payload.globalMaxPeak)
        || payload.globalMaxPeak <= 0.0f) {
        return false;
    }

    const QString renderPath = renderCachePathFor(filePath, payload.pointsPerSecond);
    if (!saveRenderCache(renderPath, payload))
        return false;

    // The full analysis payload stores the same spectral data twice as floats.
    // For long sets it can grow to hundreds of MiB and is never needed for
    // rendering after library analysis has been persisted. Keep the compact,
    // progressively readable line cache instead.
    const qint64 fullPayloadMaximumBins = kFullPayloadMaximumDurationSeconds
        * static_cast<qint64>(payload.pointsPerSecond);
    if (payload.totalExpected > fullPayloadMaximumBins) {
        QFile::remove(cachePathFor(filePath, payload.pointsPerSecond));
        return true;
    }

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
        << static_cast<qint32>(payload.rgb.size())
        << static_cast<qint32>(payload.peakMip.size());

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
                << fr.lowMid
                << fr.mid
                << fr.high
                << static_cast<quint8>(fr.color.red())
                << static_cast<quint8>(fr.color.green())
                << static_cast<quint8>(fr.color.blue());
        }
        rgbWritten += n;
    }

    const qsizetype peakTotal = payload.peakMip.size();
    qsizetype peakWritten = 0;
    while (peakWritten < peakTotal) {
        const int n = std::min(kBlockSize, static_cast<int>(peakTotal - peakWritten));
        for (int i = 0; i < n; ++i) {
            const auto& pf = payload.peakMip[peakWritten + i];
            out << pf.minSample << pf.maxSample;
        }
        peakWritten += n;
    }

    if (out.status() != QDataStream::Ok)
        return false;

    return f.commit();
}
