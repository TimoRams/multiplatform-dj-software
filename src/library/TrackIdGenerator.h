#pragma once

#include <QString>
#include <QCryptographicHash>
#include <QFileInfo>

#include <juce_audio_formats/juce_audio_formats.h>

// Hybrid Track-ID generator.
// Primary:  SHA-256 of (artist + title + duration_sec) when metadata is available.
// Fallback: SHA-256 of (file_size + file_name) when tags are missing.
// The result is a 64-char hex string used as the PRIMARY KEY in the Tracks table.
namespace TrackIdGenerator
{

inline QString generate(const QString& artist, const QString& title,
                        int durationSec, const QString& filePath)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);

    bool hasMeta = !artist.isEmpty() && !title.isEmpty() && durationSec > 0;

    if (hasMeta) {
        hash.addData(artist.toUtf8());
        hash.addData(title.toUtf8());
        hash.addData(QByteArray::number(durationSec));
    } else {
        QFileInfo fi(filePath);
        hash.addData(QByteArray::number(fi.size()));
        hash.addData(fi.fileName().toUtf8());
    }

    return QString::fromLatin1(hash.result().toHex());
}

} // namespace TrackIdGenerator
