#include "WaveformCache.h"
#include "waveform/WaveformLodPyramid.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace {

constexpr quint32 kMagic = 0x52574631; // RWF1
constexpr qint32 kVersion = 8;
constexpr quint32 kRenderMagic = 0x52574c31; // RWL1
constexpr qint32 kRenderVersion = WaveformCache::kRenderCacheVersion;
constexpr quint32 kLodMagic = 0x4c4f4432; // LOD2
constexpr int kBlockSize = 4096;
constexpr qint32 kMaxCachedBins = 100'000'000;
constexpr int kRenderOverviewBins = TrackData::kOverviewBins;
constexpr qint64 kFullPayloadMaximumDurationSeconds = 10LL * 60LL;
constexpr qint64 kRenderHeaderBytes = 6 * sizeof(qint32);
constexpr qint64 kRenderOverviewRecordBytes = 5;
constexpr qint64 kRenderLineRecordBytes = 10;
constexpr qint32 kPersistedLodTileSamples = 4096;

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
constexpr auto kDataStreamVersion = QDataStream::Qt_6_5;
#elif QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
constexpr auto kDataStreamVersion = QDataStream::Qt_6_4;
#else
constexpr auto kDataStreamVersion = QDataStream::Qt_6_0;
#endif

QString cacheBaseDir()
{
    // Honouring XDG_CONFIG_HOME before Qt's platform-native location keeps
    // cache fixtures contained and makes headless/sandboxed load paths
    // deterministic. Normal installs still use AppConfigLocation.
    QString configDir = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (configDir.isEmpty())
        configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
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

// A cache file that exists but fails validation can never become usable again.
// The key already covers the source path, its size, its modification time and
// the analysis resolution, so a mismatch here means the file is truncated,
// corrupt, or was written by a format this build no longer reads. Leaving it in
// place meant every later load re-read and re-rejected the same bytes and
// nothing ever cleaned them up.
//
// Both files of a pair are dropped together. They are only ever written
// together, and discarding just one leaves a half-cache that the analyzer will
// not complete: a surviving payload cache makes the analyzer skip the envelope
// pass, and the missing render cache is written by that very pass. Removing
// both forces one fresh analysis, which then restores a consistent pair.
void discardUnusableCache(const QString& filePath, int pointsPerSecond,
                          const char* reason)
{
    const QString payloadPath =
        WaveformCache::cachePathFor(filePath, pointsPerSecond);
    const QString renderPath =
        WaveformCache::renderCachePathFor(filePath, pointsPerSecond);

    bool removedAny = false;
    for (const QString& path : {payloadPath, renderPath}) {
        if (QFile::exists(path) && QFile::remove(path))
            removedAny = true;
    }
    if (removedAny) {
        qWarning() << "[WaveformCache] Discarded unusable cache for" << filePath
                   << "- reason:" << reason
                   << "- it will be rebuilt by the next analysis";
    }
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
                      qint32& overviewCount, qint32& renderVersion)
{
    if (!file.isOpen() || !file.seek(0))
        return false;
    QDataStream in(&file);
    in.setVersion(kDataStreamVersion);
    quint32 magic = 0;
    qint32 pointsPerSecond = 0;
    in >> magic >> renderVersion >> pointsPerSecond >> totalLines
       >> chunkSize >> overviewCount;
    if (in.status() != QDataStream::Ok
        || magic != kRenderMagic
        || renderVersion != kRenderVersion
        || pointsPerSecond != expectedPointsPerSecond
        || totalLines <= 0 || totalLines > kMaxCachedBins
        || chunkSize != static_cast<qint32>(WaveformLineStore::kChunkSize)
        || overviewCount <= 0 || overviewCount > kRenderOverviewBins) {
        return false;
    }
    const qint64 expectedSize = kRenderHeaderBytes
        + static_cast<qint64>(overviewCount) * kRenderOverviewRecordBytes
        + static_cast<qint64>(totalLines) * kRenderLineRecordBytes;
    return file.size() > expectedSize;
}

void writeRenderLine(QDataStream& out, const WaveformLine& line)
{
    out << static_cast<qint16>(line.minimum)
        << static_cast<qint16>(line.maximum)
        << static_cast<quint8>(line.rms)
        << static_cast<quint8>(line.bass)
        << static_cast<quint8>(line.mid)
        << static_cast<quint8>(line.treble)
        << static_cast<quint8>(line.flags)
        << static_cast<quint8>(line.reserved);
}

bool preparedLinesAreComplete(const WaveformCache::Payload& payload)
{
    const auto& prepared = payload.preparedLines;
    if (!prepared || prepared->totalLineCount == 0
        || prepared->totalLineCount
            != static_cast<std::uint32_t>(payload.totalExpected)) {
        return false;
    }
    std::uint32_t covered = 0;
    for (const auto& chunk : prepared->chunks) {
        if (!chunk || chunk->empty()
            || chunk->size() > WaveformLineStore::kChunkSize
            || covered + chunk->size() > prepared->totalLineCount) {
            return false;
        }
        covered += static_cast<std::uint32_t>(chunk->size());
    }
    return covered == prepared->totalLineCount;
}

WaveformLine payloadLineAt(const WaveformCache::Payload& payload, int index)
{
    if (payload.preparedLines) {
        const auto chunkIndex = static_cast<std::uint32_t>(index)
            / WaveformLineStore::kChunkSize;
        const auto local = static_cast<std::uint32_t>(index)
            % WaveformLineStore::kChunkSize;
        if (chunkIndex < payload.preparedLines->chunks.size()) {
            const auto& chunk = payload.preparedLines->chunks[chunkIndex];
            if (chunk && local < chunk->size())
                return (*chunk)[local];
        }
        return {};
    }
    return waveform::makeCanonicalLine(
        payload.waveform[index],
        waveform::interpolateSpectral(
            payload.spectral, static_cast<std::uint32_t>(index),
            static_cast<std::uint32_t>(payload.waveform.size())));
}

WaveformLine foldPayloadLines(const WaveformCache::Payload& payload,
                              int begin, int end)
{
    WaveformLine result;
    std::uint64_t rms = 0, bass = 0, mid = 0, treble = 0, weight = 0;
    std::uint8_t flags = 0xff;
    bool hasExtrema = false;
    for (int index = begin; index < end; ++index) {
        const auto line = payloadLineAt(payload, index);
        if (!hasExtrema) {
            result.minimum = line.minimum;
            result.maximum = line.maximum;
            hasExtrema = true;
        } else {
            result.minimum = std::min(result.minimum, line.minimum);
            result.maximum = std::max(result.maximum, line.maximum);
        }
        const auto magnitude = static_cast<std::uint32_t>(std::max(
            std::abs(static_cast<int>(line.minimum)),
            std::abs(static_cast<int>(line.maximum))));
        const auto lineWeight = std::max(1u, magnitude);
        rms += static_cast<std::uint64_t>(line.rms) * lineWeight;
        bass += static_cast<std::uint64_t>(line.bass) * lineWeight;
        mid += static_cast<std::uint64_t>(line.mid) * lineWeight;
        treble += static_cast<std::uint64_t>(line.treble) * lineWeight;
        weight += lineWeight;
        flags &= line.flags;
    }
    if (weight > 0) {
        result.rms = static_cast<std::uint8_t>(rms / weight);
        result.bass = static_cast<std::uint8_t>(bass / weight);
        result.mid = static_cast<std::uint8_t>(mid / weight);
        result.treble = static_cast<std::uint8_t>(treble / weight);
        result.flags = flags;
    }
    return result;
}

bool saveRenderCache(const QString& path, const WaveformCache::Payload& payload)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&file);
    out.setVersion(kDataStreamVersion);

    const int total = payload.preparedLines
        ? static_cast<int>(payload.preparedLines->totalLineCount)
        : static_cast<int>(payload.waveform.size());
    const bool hasPreparedOverview = !payload.overview.isEmpty();
    const int overviewSourceCount = hasPreparedOverview
        ? static_cast<int>(payload.overview.size())
        : static_cast<int>(payload.spectral.size());
    const int overviewCount = std::min(kRenderOverviewBins, overviewSourceCount);
    out << static_cast<quint32>(kRenderMagic)
        << static_cast<qint32>(kRenderVersion)
        << static_cast<qint32>(payload.pointsPerSecond)
        << static_cast<qint32>(total)
        << static_cast<qint32>(WaveformLineStore::kChunkSize)
        << static_cast<qint32>(overviewCount);

    for (int bin = 0; bin < overviewCount; ++bin) {
        const int begin = static_cast<int>((static_cast<qint64>(bin)
            * overviewSourceCount) / overviewCount);
        const int end = std::max(begin + 1, static_cast<int>(
            (static_cast<qint64>(bin + 1) * overviewSourceCount)
                / overviewCount));
        float peak = 0.0f, rms = 0.0f, bass = 0.0f, mid = 0.0f, treble = 0.0f;
        const auto& overviewSource = hasPreparedOverview
            ? payload.overview : payload.spectral;
        for (int index = begin;
             index < std::min(end, overviewSourceCount); ++index) {
            const auto& frame = overviewSource[index];
            peak = std::max(peak, frame.peak);
            rms = std::max(rms, frame.rms);
            bass = std::max(bass, frame.bass);
            mid = std::max(mid, frame.mid);
            treble = std::max(treble, frame.treble);
        }
        out << quantizeUnit(peak) << quantizeUnit(rms)
            << quantizeUnit(bass) << quantizeUnit(mid)
            << quantizeUnit(treble);
    }

    for (int index = 0; index < total; ++index) {
        const auto line = payloadLineAt(payload, index);
        writeRenderLine(out, line);
    }

    out << static_cast<quint32>(kLodMagic)
        << static_cast<qint32>(waveform::WaveformLodPyramid::kLevels.size() - 1);
    for (std::size_t levelIndex = 1;
         levelIndex < waveform::WaveformLodPyramid::kLevels.size(); ++levelIndex) {
        const auto level = waveform::WaveformLodPyramid::kLevels[levelIndex];
        const int stride = level.canonicalLineStride;
        const int sampleCount = (total + stride - 1) / stride;
        const int tileCount = (sampleCount + kPersistedLodTileSamples - 1)
            / kPersistedLodTileSamples;
        out << static_cast<qint32>(level.index)
            << static_cast<qint32>(stride)
            << static_cast<qint32>(sampleCount)
            << static_cast<qint32>(kPersistedLodTileSamples)
            << static_cast<qint32>(tileCount);
        for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
            const int firstSample = tileIndex * kPersistedLodTileSamples;
            const int count = std::min(kPersistedLodTileSamples,
                                       sampleCount - firstSample);
            out << static_cast<qint32>(firstSample)
                << static_cast<qint32>(count);
            for (int local = 0; local < count; ++local) {
                const int sample = firstSample + local;
                const int begin = sample * stride;
                const int end = std::min(total, begin + stride);
                writeRenderLine(out, foldPayloadLines(payload, begin, end));
            }
        }
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
    const auto discard = [&](const char* reason) {
        file.close();
        discardUnusableCache(filePath, pointsPerSecond, reason);
        return false;
    };

    qint32 totalLines = 0;
    qint32 chunkSize = 0;
    qint32 overviewCount = 0;
    qint32 renderVersion = 0;
    if (!readRenderHeader(file, pointsPerSecond, totalLines,
                          chunkSize, overviewCount, renderVersion)) {
        return discard("render header failed validation");
    }

    QDataStream in(&file);
    in.setVersion(kDataStreamVersion);
    RenderInfo info;
    info.pointsPerSecond = pointsPerSecond;
    info.totalLines = totalLines;
    info.cacheVersion = renderVersion;
    info.overview.reserve(overviewCount);
    for (int index = 0; index < overviewCount; ++index) {
        quint8 peak = 0, rms = 0, bass = 0, mid = 0, treble = 0;
        in >> peak >> rms >> bass >> mid >> treble;
        TrackData::RgbWaveformFrame frame;
        frame.peak = dequantizeUnit(peak);
        frame.rms = dequantizeUnit(rms);
        frame.bass = dequantizeUnit(bass);
        frame.mid = dequantizeUnit(mid);
        frame.treble = dequantizeUnit(treble);
        info.overview.push_back(frame);
    }
    if (in.status() != QDataStream::Ok)
        return discard("render overview is truncated");
    if (renderVersion >= 2) {
        const qint64 lodOffset = kRenderHeaderBytes
            + static_cast<qint64>(overviewCount) * kRenderOverviewRecordBytes
            + static_cast<qint64>(totalLines) * kRenderLineRecordBytes;
        if (!file.seek(lodOffset))
            return discard("render cache is shorter than its own header claims");
        quint32 lodMagic = 0;
        qint32 lodLevelCount = 0;
        in >> lodMagic >> lodLevelCount;
        if (in.status() != QDataStream::Ok || lodMagic != kLodMagic
            || lodLevelCount != 4) {
            return discard("render cache is missing its LOD trailer");
        }
        info.lodLevelCount = lodLevelCount;
    }
    *out = std::move(info);
    return true;
}

bool WaveformCache::streamRenderCache(
    const QString& filePath,
    int pointsPerSecond,
    const std::function<bool()>& shouldCancel,
    const std::function<waveform::WaveformDemand()>& demandSnapshot,
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
    qint32 renderVersion = 0;
    const auto discard = [&](const char* reason) {
        file.close();
        discardUnusableCache(filePath, pointsPerSecond, reason);
        return false;
    };
    if (!readRenderHeader(file, pointsPerSecond, totalLines,
                          chunkSize, overviewCount, renderVersion)) {
        return discard("render header failed validation");
    }

    const int chunkCount = (totalLines + chunkSize - 1) / chunkSize;
    std::vector<bool> loaded(static_cast<size_t>(chunkCount), false);
    int loadedCount = 0;
    constexpr std::size_t kInteractiveBatchChunks = 4;
    constexpr std::size_t kBackgroundBatchChunks = 32;
    bool publishedFirstPlayheadChunk = false;
    const qint64 linesOffset = kRenderHeaderBytes
        + static_cast<qint64>(overviewCount) * kRenderOverviewRecordBytes;

    const auto readChunk = [&](int chunkIndex)
        -> std::optional<WaveformLineBlock> {
        const int firstLine = chunkIndex * chunkSize;
        const int count = std::min(chunkSize, totalLines - firstLine);
        const qint64 offset = linesOffset
            + static_cast<qint64>(firstLine) * kRenderLineRecordBytes;
        if (!file.seek(offset))
            return std::nullopt;
        QDataStream in(&file);
        in.setVersion(kDataStreamVersion);
        auto lines = std::make_shared<std::vector<WaveformLine>>(
            static_cast<size_t>(count));
        for (int local = 0; local < count; ++local) {
            qint16 minimum = 0, maximum = 0;
            quint8 rms = 0, bass = 0, mid = 0, treble = 0, flags = 0, reserved = 0;
            in >> minimum >> maximum >> rms >> bass >> mid >> treble >> flags >> reserved;
            auto& line = (*lines)[static_cast<size_t>(local)];
            line.minimum = minimum;
            line.maximum = maximum;
            line.rms = rms;
            line.bass = bass;
            line.mid = mid;
            line.treble = treble;
            // Persisted render-cache lines are authoritative analysis output,
            // including caches written before the explicit Final flag existed.
            line.flags = flags | waveform_line_flags::kFinal;
            line.reserved = reserved;
        }
        if (in.status() != QDataStream::Ok)
            return std::nullopt;
        return WaveformLineBlock{firstLine, std::move(lines)};
    };

    while (loadedCount < chunkCount) {
        if (shouldCancel && shouldCancel())
            return false;

        const auto demand = demandSnapshot
            ? demandSnapshot() : waveform::WaveformDemand{};
        struct Candidate final {
            int chunkIndex = 0;
            waveform::WaveformPriorityScore score;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(static_cast<std::size_t>(chunkCount - loadedCount));
        for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            if (loaded[static_cast<std::size_t>(chunkIndex)])
                continue;
            const double beginSec = static_cast<double>(chunkIndex * chunkSize)
                / static_cast<double>(pointsPerSecond);
            const double endSec = static_cast<double>(std::min(
                totalLines, (chunkIndex + 1) * chunkSize))
                / static_cast<double>(pointsPerSecond);
            auto score = waveform::priorityForRange(
                demand, beginSec, endSec);
            if (!demand.valid()) {
                score.priority = waveform::WaveformPriority::BackgroundRest;
                score.distanceSec = static_cast<double>(chunkIndex);
            }
            candidates.push_back({chunkIndex, score});
        }
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                if (waveform::higherPriority(left.score, right.score))
                    return true;
                if (waveform::higherPriority(right.score, left.score))
                    return false;
                return left.chunkIndex < right.chunkIndex;
            });
        if (candidates.empty())
            break;

        // The first cache publication after load is deliberately one immutable
        // source chunk.  Reading a 32-chunk guard batch before notifying the
        // owner thread delayed first paint and let a subsequent seek wait
        // behind stale disk work.  Later interactive batches stay small so the
        // demand snapshot is re-evaluated frequently; only background fill is
        // amortised into larger reads.
        const bool interactive = demand.valid()
            && candidates.front().score.expansionRank <= 2;
        const std::size_t requestedBatch = !publishedFirstPlayheadChunk
            && demand.valid()
            ? 1
            : (interactive ? kInteractiveBatchChunks
                           : kBackgroundBatchChunks);
        const std::size_t batchCount = std::min(
            candidates.size(), requestedBatch);

        WaveformLineBatch batch;
        batch.reserve(batchCount);
        for (std::size_t candidateIndex = 0;
             candidateIndex < batchCount; ++candidateIndex) {
            const int chunkIndex = candidates[candidateIndex].chunkIndex;
            auto chunk = readChunk(chunkIndex);
            if (!chunk) {
                // The header validated but a line block did not read back, so
                // the file is damaged past its header.
                return discard("render line block could not be read");
            }
            loaded[static_cast<size_t>(chunkIndex)] = true;
            ++loadedCount;
            batch.push_back(std::move(*chunk));
        }
        publishChunk(totalLines, std::move(batch));
        publishedFirstPlayheadChunk = true;
    }
    return loadedCount == chunkCount;
}

bool WaveformCache::streamRenderLodCache(
    const QString& filePath,
    int pointsPerSecond,
    int requestedLevel,
    const std::function<bool()>& shouldCancel,
    const std::function<void(LodTile)>& publishTile)
{
    if (!publishTile || requestedLevel < 1 || requestedLevel > 4)
        return false;
    QFile file(renderCachePathFor(filePath, pointsPerSecond));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    qint32 totalLines = 0, chunkSize = 0, overviewCount = 0, renderVersion = 0;
    if (!readRenderHeader(file, pointsPerSecond, totalLines, chunkSize,
                          overviewCount, renderVersion)
        || renderVersion < 2) {
        return false;
    }
    const qint64 lodOffset = kRenderHeaderBytes
        + static_cast<qint64>(overviewCount) * kRenderOverviewRecordBytes
        + static_cast<qint64>(totalLines) * kRenderLineRecordBytes;
    if (!file.seek(lodOffset))
        return false;
    QDataStream in(&file);
    in.setVersion(kDataStreamVersion);
    quint32 lodMagic = 0;
    qint32 levelCount = 0;
    in >> lodMagic >> levelCount;
    if (in.status() != QDataStream::Ok || lodMagic != kLodMagic
        || levelCount != 4) {
        return false;
    }

    bool found = false;
    for (int storedLevel = 0; storedLevel < levelCount; ++storedLevel) {
        qint32 level = 0, stride = 0, sampleCount = 0;
        qint32 tileSize = 0, tileCount = 0;
        in >> level >> stride >> sampleCount >> tileSize >> tileCount;
        if (in.status() != QDataStream::Ok
            || level < 1 || level > 4
            || stride != waveform::WaveformLodPyramid::level(level).canonicalLineStride
            || sampleCount != (totalLines + stride - 1) / stride
            || tileSize != kPersistedLodTileSamples
            || tileCount != (sampleCount + tileSize - 1) / tileSize) {
            return false;
        }
        for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
            if (shouldCancel && shouldCancel())
                return false;
            qint32 firstSample = 0, count = 0;
            in >> firstSample >> count;
            if (in.status() != QDataStream::Ok
                || firstSample != tileIndex * tileSize
                || count != std::min(tileSize, sampleCount - firstSample)) {
                return false;
            }
            if (level != requestedLevel) {
                if (!file.seek(file.pos()
                               + static_cast<qint64>(count)
                                   * kRenderLineRecordBytes)) {
                    return false;
                }
                continue;
            }

            auto lines = std::make_shared<std::vector<WaveformLine>>(
                static_cast<std::size_t>(count));
            for (auto& line : *lines) {
                qint16 minimum = 0, maximum = 0;
                quint8 rms = 0, bass = 0, mid = 0, treble = 0, flags = 0, reserved = 0;
                in >> minimum >> maximum >> rms >> bass >> mid >> treble >> flags >> reserved;
                line.minimum = minimum;
                line.maximum = maximum;
                line.rms = rms;
                line.bass = bass;
                line.mid = mid;
                line.treble = treble;
                line.flags = flags | waveform_line_flags::kFinal;
                line.reserved = reserved;
            }
            if (in.status() != QDataStream::Ok)
                return false;
            publishTile({level, stride, firstSample, sampleCount, std::move(lines)});
            found = true;
        }
    }
    return found;
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
    qint32 spectralPps = 0;
    qint32 totalExpected = 0;
    float globalMaxPeak = 0.001f;
    qint32 geometryVersion = 0;
    qint32 spectralVersion = 0;
    qint32 overviewVersion = 0;
    qint32 wfCount = 0;
    qint32 rgbCount = 0;
    qint32 overviewCount = 0;
    qint32 peakCount = 0;

    const auto discard = [&](const char* reason) {
        f.close();
        discardUnusableCache(filePath, pointsPerSecond, reason);
        return false;
    };

    in >> magic >> version >> pps >> spectralPps >> totalExpected
       >> globalMaxPeak >> geometryVersion >> spectralVersion >> overviewVersion
       >> wfCount >> rgbCount >> overviewCount >> peakCount;
    if (in.status() != QDataStream::Ok)
        return discard("payload header is truncated");
    if (magic != kMagic)
        return discard("payload magic does not match");
    if (version != kVersion)
        return discard("payload was written by another cache version");
    const auto expectedSections = analysis::AnalysisSectionVersions::current();
    if (pps != pointsPerSecond || spectralPps <= 0 || spectralPps > pps)
        return discard("payload resolution does not match the request");
    if (geometryVersion != static_cast<qint32>(expectedSections.geometry)
        || spectralVersion != static_cast<qint32>(expectedSections.spectralWaveform)
        || overviewVersion != static_cast<qint32>(expectedSections.overview)) {
        return discard("one or more canonical cache sections are stale");
    }
    if (totalExpected <= 0 || totalExpected > kMaxCachedBins
        || wfCount != totalExpected || rgbCount <= 0
        || rgbCount != static_cast<qint32>(
            (static_cast<qint64>(totalExpected) * spectralPps + pps - 1)
            / pps)
        || overviewCount < 0 || overviewCount > TrackData::kOverviewBins
        || peakCount < 0 || peakCount > kMaxCachedBins
        || !std::isfinite(globalMaxPeak) || globalMaxPeak <= 0.0f) {
        return discard("payload header describes an impossible waveform");
    }

    Payload payload;
    payload.pointsPerSecond = pps;
    payload.spectralPointsPerSecond = spectralPps;
    payload.totalExpected = totalExpected;
    payload.globalMaxPeak = globalMaxPeak;
    payload.sections = expectedSections;
    payload.waveform.reserve(wfCount);
    payload.spectral.reserve(rgbCount);
    payload.overview.reserve(overviewCount);
    payload.peakMip.reserve(peakCount);

    int wfRead = 0;
    while (wfRead < wfCount) {
        const int n = std::min(kBlockSize, wfCount - wfRead);
        for (int i = 0; i < n; ++i) {
            float minimum = 0.0f;
            float maximum = 0.0f;
            float peak = 0.0f;
            float rms = 0.0f;
            in >> minimum >> maximum >> peak >> rms;

            TrackData::WaveformBin d;
            d.minimum = minimum;
            d.maximum = maximum;
            d.peak = peak;
            d.rms = rms;
            payload.waveform.push_back(d);
        }
        wfRead += n;
    }

    int rgbRead = 0;
    while (rgbRead < rgbCount) {
        const int n = std::min(kBlockSize, rgbCount - rgbRead);
        for (int i = 0; i < n; ++i) {
            float peak = 0.0f;
            float rms = 0.0f;
            float bass = 0.0f;
            float mid = 0.0f;
            float treble = 0.0f;
            in >> peak >> rms >> bass >> mid >> treble;

            TrackData::RgbWaveformFrame frame;
            frame.peak = peak;
            frame.rms = rms;
            frame.bass = bass;
            frame.mid = mid;
            frame.treble = treble;
            payload.spectral.push_back(frame);
        }
        rgbRead += n;
    }

    int overviewRead = 0;
    while (overviewRead < overviewCount) {
        const int n = std::min(kBlockSize, overviewCount - overviewRead);
        for (int i = 0; i < n; ++i) {
            TrackData::SpectralWaveformPoint frame;
            in >> frame.peak >> frame.rms >> frame.bass
               >> frame.mid >> frame.treble;
            payload.overview.push_back(frame);
        }
        overviewRead += n;
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

    if (in.status() != QDataStream::Ok)
        return discard("payload body is truncated");
    if (!f.atEnd())
        return discard("payload body has trailing bytes");

    *out = std::move(payload);
    return true;
}

bool WaveformCache::saveForFile(const QString& filePath, const Payload& payload)
{
    if (payload.pointsPerSecond <= 0
        || payload.spectralPointsPerSecond <= 0
        || payload.totalExpected <= 0
        || !std::isfinite(payload.globalMaxPeak)
        || payload.globalMaxPeak <= 0.0f) {
        return false;
    }
    const auto expectedSections = analysis::AnalysisSectionVersions::current();
    if (payload.sections.geometry != expectedSections.geometry
        || payload.sections.spectralWaveform != expectedSections.spectralWaveform
        || payload.sections.overview != expectedSections.overview) {
        return false;
    }

    const qint64 fullPayloadMaximumBins = kFullPayloadMaximumDurationSeconds
        * static_cast<qint64>(payload.pointsPerSecond);
    const int effectiveSpectralPps = std::min(
        payload.spectralPointsPerSecond, payload.pointsPerSecond);
    const qint64 expectedSpectralBins =
        (static_cast<qint64>(payload.totalExpected) * effectiveSpectralPps
            + payload.pointsPerSecond - 1)
        / payload.pointsPerSecond;
    const bool longTrack = payload.totalExpected > fullPayloadMaximumBins;
    const bool hasLegacyVectors = !payload.waveform.isEmpty()
        && !payload.spectral.isEmpty()
        && payload.totalExpected == payload.waveform.size()
        && expectedSpectralBins == payload.spectral.size();
    const bool hasPreparedLines = preparedLinesAreComplete(payload);
    if ((!longTrack && !hasLegacyVectors)
        || (longTrack && !hasLegacyVectors && !hasPreparedLines)
        || (payload.spectral.isEmpty() && payload.overview.isEmpty())) {
        return false;
    }

    const QString renderPath = renderCachePathFor(filePath, payload.pointsPerSecond);
    if (!saveRenderCache(renderPath, payload))
        return false;

    // The full analysis payload stores the same spectral data twice as floats.
    // For long sets it can grow to hundreds of MiB and is never needed for
    // rendering after library analysis has been persisted. Keep the compact,
    // progressively readable line cache instead.
    if (longTrack) {
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
        << static_cast<qint32>(std::min(
            payload.spectralPointsPerSecond, payload.pointsPerSecond))
        << static_cast<qint32>(payload.totalExpected)
        << payload.globalMaxPeak
        << static_cast<qint32>(payload.sections.geometry)
        << static_cast<qint32>(payload.sections.spectralWaveform)
        << static_cast<qint32>(payload.sections.overview)
        << static_cast<qint32>(payload.waveform.size())
        << static_cast<qint32>(payload.spectral.size())
        << static_cast<qint32>(payload.overview.size())
        << static_cast<qint32>(payload.peakMip.size());

    const qsizetype wfTotal = payload.waveform.size();
    qsizetype wfWritten = 0;
    while (wfWritten < wfTotal) {
        const int n = std::min(kBlockSize, static_cast<int>(wfTotal - wfWritten));
        for (int i = 0; i < n; ++i) {
            const auto& d = payload.waveform[wfWritten + i];
            out << d.minimum << d.maximum << d.peak << d.rms;
        }
        wfWritten += n;
    }

    const qsizetype rgbTotal = payload.spectral.size();
    qsizetype rgbWritten = 0;
    while (rgbWritten < rgbTotal) {
        const int n = std::min(kBlockSize, static_cast<int>(rgbTotal - rgbWritten));
        for (int i = 0; i < n; ++i) {
            const auto& fr = payload.spectral[rgbWritten + i];
            out << fr.peak
                << fr.rms
                << fr.bass
                << fr.mid
                << fr.treble;
        }
        rgbWritten += n;
    }

    const qsizetype overviewTotal = payload.overview.size();
    qsizetype overviewWritten = 0;
    while (overviewWritten < overviewTotal) {
        const int n = std::min(
            kBlockSize, static_cast<int>(overviewTotal - overviewWritten));
        for (int i = 0; i < n; ++i) {
            const auto& frame = payload.overview[overviewWritten + i];
            out << frame.peak << frame.rms << frame.bass
                << frame.mid << frame.treble;
        }
        overviewWritten += n;
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
