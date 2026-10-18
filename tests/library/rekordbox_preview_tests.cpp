#include "library/devices/rekordbox/RekordboxDeviceSource.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

namespace {
bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

void be32(QByteArray& bytes, qsizetype offset, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        bytes[offset + i] = char(value >> (24 - i * 8));
}

QByteArray section(const char* tag, quint32 header, const QByteArray& body)
{
    QByteArray bytes(12, '\0');
    bytes.replace(qsizetype(0), 4, tag, 4);
    be32(bytes, 4, header);
    be32(bytes, 8, 12 + body.size());
    return bytes + body;
}

QByteArray preview(const QByteArray& columns)
{
    QByteArray body(8, '\0');
    be32(body, 0, columns.size());
    return section("PWAV", 20, body + columns);
}

QByteArray fileBytes(const QByteArray& sections)
{
    QByteArray bytes(28, '\0');
    bytes.replace(qsizetype(0), 4, "PMAI", 4);
    be32(bytes, 4, 28);
    be32(bytes, 8, 28 + sections.size());
    return bytes + sections;
}

bool save(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
} // namespace

bool runRekordboxPreviewTests()
{
    bool ok = true;
    QTemporaryDir directory(QDir::current().filePath(QStringLiteral("rekordbox-preview-XXXXXX")));
    if (!check(directory.isValid(), "preview fixture directory"))
        return false;
    const QString dat = directory.filePath(QStringLiteral("ANLZ0000.DAT"));
    const QString ext = directory.filePath(QStringLiteral("ANLZ0000.EXT"));
    const QString twoEx = directory.filePath(QStringLiteral("ANLZ0000.2EX"));
    const QByteArray original = fileBytes(
        section("PWV3", 12, QByteArray(1024 * 1024, '\x7f'))
        + preview(QByteArray::fromHex("00ff81a4")));
    ok &= check(save(dat, original), "write PWAV fixture");
    rekordbox::AnalysisReader reader;
    auto result = reader.readReadOnly(dat);
    ok &= check(result.ok
                    && result.analysis.waveformPreview == QByteArray::fromHex("001f0104"),
                "PWAV masks intensity bits and skips scrolling waveform payload");
    ok &= check(result.analysis.beats.isEmpty(), "preview does not fabricate beatgrid");

    auto index = std::make_shared<rekordbox::DeviceIndex>();
    index->deviceId = QStringLiteral("preview-source");
    index->mountPath = directory.path();
    rekordbox::Track track;
    track.sourceAwareId = QStringLiteral("preview-track");
    track.analysisPath = dat;
    index->tracks.append(track);
    index->trackBySourceAwareId.insert(track.sourceAwareId, 0);
    rekordbox::AnalysisCache cache;
    const auto first = cache.readReadOnly(index, track.sourceAwareId);
    const auto second = cache.readReadOnly(index, track.sourceAwareId);
    ok &= check(first.ok && second.ok && cache.entryCount() == 1
                    && first.analysis.waveformPreview.constData()
                        == second.analysis.waveformPreview.constData(),
                "unchanged repeated load reuses immutable parsed preview");
    {
        QFile file(dat);
        ok &= check(file.open(QIODevice::ReadOnly) && file.readAll() == original,
                    "parsing and cache hits leave device bytes unchanged");
    }

    ok &= check(save(dat, fileBytes(preview(QByteArray::fromHex("01020304")))),
                "replace source analysis");
    auto changed = cache.readReadOnly(index, track.sourceAwareId);
    ok &= check(changed.ok
                    && changed.analysis.waveformPreview == QByteArray::fromHex("01020304"),
                "source modification invalidates cached analysis");
    const QDateTime originalTime = QFileInfo(dat).lastModified();
    {
        QFile file(dat);
        ok &= check(file.open(QIODevice::ReadWrite), "open metadata replacement fixture");
        const QByteArray sameSize = fileBytes(preview(QByteArray::fromHex("05060708")));
        ok &= check(file.write(sameSize) == sameSize.size(), "same-size source rewrite");
        ok &= check(file.setFileTime(originalTime, QFileDevice::FileModificationTime),
                    "restore old modification time");
    }
    changed = cache.readReadOnly(index, track.sourceAwareId);
    ok &= check(changed.ok
                    && changed.analysis.waveformPreview == QByteArray::fromHex("05060708"),
                "same-size rewrite with restored mtime invalidates cache");

    ok &= check(save(ext, fileBytes(preview(QByteArray::fromHex("090a0b0c0d")))),
                "create new companion analysis");
    changed = cache.readReadOnly(index, track.sourceAwareId);
    ok &= check(changed.ok && changed.analysis.waveformPreview.size() == 5,
                "new companion invalidates cache and richer preview wins");
    ok &= check(QFile::remove(ext), "remove companion");
    changed = cache.readReadOnly(index, track.sourceAwareId);
    ok &= check(changed.ok && changed.analysis.waveformPreview.size() == 4,
                "removed companion invalidates cache");

    auto replacementIndex = std::make_shared<rekordbox::DeviceIndex>(*index);
    const auto remounted = cache.readReadOnly(replacementIndex, track.sourceAwareId);
    ok &= check(remounted.ok
                    && remounted.analysis.waveformPreview.constData()
                        != changed.analysis.waveformPreview.constData(),
                "fresh mounted-source index never reuses old source cache entry");
    replacementIndex.reset();
    index.reset();
    ok &= check(!cache.readReadOnly({}, track.sourceAwareId).ok && cache.entryCount() == 0,
                "ejected index expires and its cached analysis is discarded");
    rekordbox::AnalysisCache tinyCache(1);
    index = std::make_shared<rekordbox::DeviceIndex>();
    index->deviceId = QStringLiteral("bounded-source");
    index->mountPath = directory.path();
    index->tracks.append(track);
    index->trackBySourceAwareId.insert(track.sourceAwareId, 0);
    ok &= check(tinyCache.readReadOnly(index, track.sourceAwareId).ok
                    && tinyCache.entryCount() == 0,
                "oversize entries bypass bounded cache without blocking load");

    QByteArray malformed = preview(QByteArray::fromHex("01020304"));
    be32(malformed, 12, 4097);
    ok &= check(save(dat, fileBytes(malformed)), "write malformed optional preview");
    result = reader.readReadOnly(dat);
    ok &= check(result.ok && result.analysis.waveformPreview.isEmpty(),
                "malformed optional preview safely falls back to own waveform");
    ok &= check(save(dat, fileBytes(preview(QByteArray::fromHex("0102"))))
                    && save(ext, QByteArrayLiteral("corrupt"))
                    && save(twoEx, fileBytes(section("PWV7", 12, QByteArray(64, '\0')))),
                "write partial companion fixture");
    result = reader.readRelatedReadOnly(dat, directory.path());
    ok &= check(result.ok && result.analysis.waveformPreview.size() == 2,
                "corrupt optional companion does not discard valid DAT preview");
    ok &= check(QFile::remove(ext), "remove corrupt companion");
#ifndef Q_OS_WIN
    QTemporaryDir outside(QDir::current().filePath(QStringLiteral("rekordbox-outside-XXXXXX")));
    const QString outsideFile = outside.filePath(QStringLiteral("outside.EXT"));
    ok &= check(save(outsideFile, fileBytes(preview(QByteArray(10, '\x1f'))))
                    && QFile::link(outsideFile, ext),
                "create escaping companion symlink");
    result = reader.readRelatedReadOnly(dat, directory.path());
    ok &= check(result.ok && result.analysis.waveformPreview.size() == 2,
                "escaping sidecar is never read into device analysis");
#endif
    cache.clear();
    ok &= check(cache.entryCount() == 0, "explicit cache clearing");
    return ok;
}
