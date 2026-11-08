#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QString>

namespace brockdj::library {

// Stable library id: tagged tracks hash artist/title/duration so the same
// recording at another path maps to the same row; untagged files fall back
// to file size and name.
inline QString generateTrackId(const QString& artist,
                               const QString& title,
                               int durationSeconds,
                               const QString& filePath)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!artist.isEmpty() && !title.isEmpty() && durationSeconds > 0) {
        hash.addData(artist.toUtf8());
        hash.addData(title.toUtf8());
        hash.addData(QByteArray::number(durationSeconds));
    } else {
        const QFileInfo file(filePath);
        hash.addData(QByteArray::number(file.size()));
        hash.addData(file.fileName().toUtf8());
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace brockdj::library
