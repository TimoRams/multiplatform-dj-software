// A cache file that exists but does not validate must be removed, not merely
// rejected. The cache key already covers the source path, its size, its
// modification time and the analysis resolution, so a file that fails to read
// back can never become usable again — leaving it on disk meant every later
// load re-read and re-rejected the same bytes forever.

#include "waveform/WaveformCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

constexpr int kPointsPerSecond = 1200;
constexpr int kBins = 4096;

WaveformCache::Payload makeValidPayload()
{
    WaveformCache::Payload payload;
    payload.pointsPerSecond = kPointsPerSecond;
    payload.totalExpected = kBins;
    payload.globalMaxPeak = 0.75f;
    payload.waveform.reserve(kBins);
    payload.rgb.reserve(kBins);
    for (int i = 0; i < kBins; ++i) {
        const float amount = static_cast<float>(i % 64) / 64.0f;
        TrackData::WaveformBin bin;
        bin.low = amount;
        bin.lowMid = amount * 0.5f;
        bin.mid = amount * 0.25f;
        bin.high = amount * 0.125f;
        payload.waveform.push_back(bin);

        TrackData::RgbWaveformFrame frame;
        frame.rms = amount;
        frame.low = bin.low;
        frame.lowMid = bin.lowMid;
        frame.mid = bin.mid;
        frame.high = bin.high;
        frame.color = QColor(200, 150, 100, 230);
        payload.rgb.push_back(frame);
    }
    return payload;
}

bool writeSourceStub(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QByteArray(1024, '\x01'));
    return true;
}

// Both files of a pair are written together, so a repair must drop both.
struct CachePaths {
    QString payload;
    QString render;

    [[nodiscard]] bool bothExist() const
    { return QFile::exists(payload) && QFile::exists(render); }

    [[nodiscard]] bool neitherExists() const
    { return !QFile::exists(payload) && !QFile::exists(render); }
};

CachePaths pathsFor(const QString& sourcePath)
{
    return {WaveformCache::cachePathFor(sourcePath, kPointsPerSecond),
            WaveformCache::renderCachePathFor(sourcePath, kPointsPerSecond)};
}

bool seedValidCache(const QString& sourcePath, CachePaths& paths)
{
    if (!WaveformCache::saveForFile(sourcePath, makeValidPayload()))
        return false;
    paths = pathsFor(sourcePath);
    return paths.bothExist();
}

bool truncateTo(const QString& path, qint64 size)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    return file.resize(size);
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir configDir;
    if (!require(configDir.isValid(), "temporary config directory is available"))
        return 1;

    // Keep the cache written by this test inside its temporary fixture. This
    // also makes the test independent of the developer's real Qt config path
    // and works in read-only/sandboxed home directories.
    qputenv("XDG_CONFIG_HOME", configDir.path().toUtf8());
    QCoreApplication app(argc, argv);

    QTemporaryDir sourceDir;
    if (!require(sourceDir.isValid(), "temporary source directory is available"))
        return 1;

    bool ok = true;

    // A valid cache round-trips and is left alone.
    {
        const QString source = sourceDir.filePath(QStringLiteral("intact.audio"));
        ok &= require(writeSourceStub(source), "source stub is writable");
        CachePaths paths;
        ok &= require(seedValidCache(source, paths), "a valid cache can be written");

        WaveformCache::Payload loaded;
        ok &= require(WaveformCache::loadForFile(source, kPointsPerSecond, &loaded),
                      "a valid cache loads back");
        ok &= require(loaded.totalExpected == kBins,
                      "a valid cache round-trips its bin count");
        ok &= require(paths.bothExist(),
                      "a valid cache is not removed by a successful load");
    }

    // A truncated payload cache is discarded together with its render sibling.
    {
        const QString source = sourceDir.filePath(QStringLiteral("truncated.audio"));
        ok &= require(writeSourceStub(source), "source stub is writable");
        CachePaths paths;
        ok &= require(seedValidCache(source, paths), "a valid cache can be written");
        ok &= require(truncateTo(paths.payload, 64),
                      "the payload cache can be truncated");

        WaveformCache::Payload loaded;
        ok &= require(!WaveformCache::loadForFile(source, kPointsPerSecond, &loaded),
                      "a truncated payload cache does not load");
        ok &= require(paths.neitherExists(),
                      "a truncated payload cache is removed with its render sibling");
    }

    // Garbage in place of a header is discarded rather than rejected forever.
    {
        const QString source = sourceDir.filePath(QStringLiteral("garbage.audio"));
        ok &= require(writeSourceStub(source), "source stub is writable");
        CachePaths paths;
        ok &= require(seedValidCache(source, paths), "a valid cache can be written");
        {
            QFile file(paths.payload);
            ok &= require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                          "the payload cache can be overwritten");
            file.write(QByteArray(512, '\x7f'));
        }

        WaveformCache::Payload loaded;
        ok &= require(!WaveformCache::loadForFile(source, kPointsPerSecond, &loaded),
                      "a payload cache with a foreign header does not load");
        ok &= require(paths.neitherExists(),
                      "a payload cache with a foreign header is removed");
    }

    // A damaged render cache also invalidates the pair: a surviving payload
    // cache would make the analyzer skip the very pass that rewrites the
    // render cache, leaving the half-cache in place indefinitely.
    {
        const QString source = sourceDir.filePath(QStringLiteral("render.audio"));
        ok &= require(writeSourceStub(source), "source stub is writable");
        CachePaths paths;
        ok &= require(seedValidCache(source, paths), "a valid cache can be written");
        ok &= require(truncateTo(paths.render, 16),
                      "the render cache can be truncated");

        WaveformCache::RenderInfo info;
        ok &= require(!WaveformCache::inspectRenderCache(source, kPointsPerSecond, &info),
                      "a truncated render cache does not inspect");
        ok &= require(paths.neitherExists(),
                      "a truncated render cache is removed with its payload sibling");
    }

    // A cache written for another resolution belongs to a different key, so it
    // is simply absent rather than invalid; nothing must be removed or crash.
    {
        const QString source = sourceDir.filePath(QStringLiteral("absent.audio"));
        ok &= require(writeSourceStub(source), "source stub is writable");
        WaveformCache::Payload loaded;
        ok &= require(!WaveformCache::loadForFile(source, kPointsPerSecond, &loaded),
                      "a missing cache does not load");
        const auto paths = pathsFor(source);
        ok &= require(paths.neitherExists(), "a missing cache stays missing");
    }

    if (!ok)
        return 1;
    std::cout << "waveform cache repair tests passed\n";
    return 0;
}
