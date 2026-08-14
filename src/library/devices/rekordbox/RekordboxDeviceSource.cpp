#include "library/devices/rekordbox/RekordboxDeviceSource.h"

#include <QDir>
#include <QFileInfo>

namespace rekordbox {
QString DeviceSource::resolveContainedPath(const QString& mountPath,
                                           const QString& devicePath,
                                           bool requireFile)
{
    const QFileInfo rootInfo(mountPath);
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || !rootInfo.isDir())
        return {};

    QString relative = QDir::fromNativeSeparators(devicePath.trimmed());
    while (relative.startsWith(QLatin1Char('/')))
        relative = relative.mid(1);
    relative = QDir::cleanPath(relative);
    if (relative.isEmpty() || relative == QStringLiteral(".")
        || relative == QStringLiteral("..")
        || relative.startsWith(QStringLiteral("../"))
        || QDir::isAbsolutePath(relative)) {
        return {};
    }

    const QFileInfo candidateInfo(QDir(canonicalRoot).filePath(relative));
    QString candidate = candidateInfo.canonicalFilePath();
    if (candidate.isEmpty()) {
        if (requireFile)
            return {};
        candidate = QDir::cleanPath(candidateInfo.absoluteFilePath());
    }
    const QString rootPrefix = canonicalRoot.endsWith(QLatin1Char('/'))
        ? canonicalRoot : canonicalRoot + QLatin1Char('/');
    if (candidate != canonicalRoot && !candidate.startsWith(rootPrefix))
        return {};
    if (requireFile && (!candidateInfo.isFile() || !candidateInfo.isReadable()))
        return {};
    return candidate;
}

DeviceSource::Result DeviceSource::readIndexReadOnly(const QString& mountPath,
                                                      const QString& deviceId) const
{
    Result result;
    const QString databasePath = resolveContainedPath(
        mountPath, QStringLiteral("PIONEER/rekordbox/export.pdb"));
    if (databasePath.isEmpty()) {
        result.error = QStringLiteral("Legacy Rekordbox export.pdb is unavailable");
        return result;
    }
    const auto parsed = PdbReader{}.readReadOnly(databasePath);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    result.index.deviceId = deviceId;
    result.index.mountPath = QFileInfo(mountPath).canonicalFilePath();
    result.index.tracks = parsed.tracks;
    result.index.playlists = parsed.playlists;
    result.index.trackById.reserve(result.index.tracks.size());
    result.index.trackBySourceAwareId.reserve(result.index.tracks.size());
    for (qsizetype i = 0; i < result.index.tracks.size(); ++i) {
        auto& track = result.index.tracks[i];
        track.sourceAwareId = QStringLiteral("rekordbox:%1:%2")
                                  .arg(deviceId, QString::number(track.id));
        track.audioPath = resolveContainedPath(mountPath, track.relativeAudioPath);
        track.analysisPath = resolveContainedPath(mountPath, track.relativeAnalysisPath);
        if (!track.relativeArtworkPath.isEmpty())
            track.artworkPath = resolveContainedPath(mountPath, track.relativeArtworkPath);
        result.index.trackById.insert(track.id, i);
        result.index.trackBySourceAwareId.insert(track.sourceAwareId, i);
    }
    result.ok = true;
    return result;
}

AnalysisReader::Result DeviceSource::readAnalysisReadOnly(
    const DeviceIndex& index, const QString& sourceAwareTrackId) const
{
    const auto found = index.trackBySourceAwareId.constFind(sourceAwareTrackId);
    if (found == index.trackBySourceAwareId.cend()) {
        AnalysisReader::Result missing;
        missing.error = QStringLiteral("Rekordbox track is not indexed");
        return missing;
    }
    const Track& track = index.tracks.at(*found);
    if (track.analysisPath.isEmpty()) {
        AnalysisReader::Result missing;
        missing.error = QStringLiteral("Rekordbox analysis path is unavailable");
        return missing;
    }
    return AnalysisReader{}.readRelatedReadOnly(track.analysisPath);
}

} // namespace rekordbox
