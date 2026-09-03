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
    for (int i = 0; i < kBins; ++i) {
        const float amount = static_cast<float>(i % 64) / 64.0f;
        TrackData::WaveformBin bin;
        bin.minimum = -amount;
        bin.maximum = amount;
        bin.peak = amount;
        bin.rms = amount * 0.7f;
        payload.waveform.push_back(bin);
    }
    constexpr int spectralBins = kBins * TrackData::SPECTRAL_POINTS_PER_SECOND
        / kPointsPerSecond;
    payload.spectral.reserve(spectralBins);
    for (int i = 0; i < spectralBins; ++i) {
        const float amount = static_cast<float>(i % 64) / 64.0f;
        TrackData::RgbWaveformFrame frame;
        frame.peak = amount;
        frame.rms = amount;
        frame.bass = amount;
        frame.mid = amount * 0.5f;
        frame.treble = amount * 0.25f;
        payload.spectral.push_back(frame);
    }
    payload.overview = TrackData::downsampleOverview(payload.spectral);
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
        ok &= require(loaded.sections.waveformCurrent(),
                      "a valid cache records current geometry/spectral/overview sections");
        const auto expected = makeValidPayload();
        ok &= require(loaded.waveform.size() == expected.waveform.size()
                         && loaded.spectral.size() == expected.spectral.size()
                         && loaded.overview == expected.overview,
                      "neutral cache changed section sizes");
        if (loaded.waveform.size() == expected.waveform.size()
            && loaded.spectral.size() == expected.spectral.size()) {
            for (int i = 0; i < loaded.waveform.size(); ++i) {
                const auto& actual = loaded.waveform[i];
                const auto& wanted = expected.waveform[i];
                ok &= require(actual.minimum == wanted.minimum
                                 && actual.maximum == wanted.maximum
                                 && actual.peak == wanted.peak
                                 && actual.rms == wanted.rms,
                             "geometry cache round-trip changed canonical data");
            }
            for (int i = 0; i < loaded.spectral.size(); ++i) {
                const auto& actual = loaded.spectral[i];
                const auto& wanted = expected.spectral[i];
                ok &= require(actual.peak == wanted.peak
                                 && actual.rms == wanted.rms
                                 && actual.bass == wanted.bass
                                 && actual.mid == wanted.mid
                                 && actual.treble == wanted.treble,
                             "spectral cache round-trip changed canonical data");
            }
        }
        ok &= require(paths.bothExist(),
                      "a valid cache is not removed by a successful load");
    }

    // Section versions are explicit: a stale canonical section is rejected
    // before it can be mixed with current geometry or rendered artifacts.
    {
        const QString source = sourceDir.filePath(QStringLiteral("sections.audio"));
        ok &= require(writeSourceStub(source), "source stub is writable");
        auto stale = makeValidPayload();
        ++stale.sections.spectralWaveform;
        ok &= require(!WaveformCache::saveForFile(source, stale),
                      "a stale spectral section cannot be written as current cache data");
        const auto current = analysis::AnalysisSectionVersions::current();
        const auto restored = analysis::AnalysisSectionVersions::fromStorageString(
            current.toStorageString());
        ok &= require(restored.geometry == current.geometry
                          && restored.spectralWaveform == current.spectralWaveform
                          && restored.overview == current.overview
                          && restored.beatGrid == current.beatGrid
                          && restored.key == current.key
                          && restored.phrase == current.phrase,
                      "section versions round-trip independently");
        ok &= require(!analysis::AnalysisSectionVersions::fromStorageString({}).waveformCurrent(),
                      "missing section metadata is distinguishable from current data");
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
