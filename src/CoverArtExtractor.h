#pragma once

#include <QString>
#include <QByteArray>
#include <utility>

/**
 * Extrahiert eingebettete Cover-Art aus Audiodateien via TagLib.
 * Unterstützt: MP3 (ID3v2 APIC), FLAC (PICTURE), MP4/M4A (covr),
 *              OGG Vorbis (METADATA_BLOCK_PICTURE), WAV (ID3v2 APIC).
 *
 * Rückgabe: {Bilddaten, Format ("JPEG"/"PNG")} oder leeres Paar.
 */
class CoverArtExtractor
{
public:
    static std::pair<QByteArray, QString> extractCoverArt(const QString& filePath);

private:
    static QString detectImageFormat(const QByteArray& data);

    static std::pair<QByteArray, QString> extractFromMP3 (const QString& filePath);
    static std::pair<QByteArray, QString> extractFromFLAC(const QString& filePath);
    static std::pair<QByteArray, QString> extractFromMP4 (const QString& filePath);
    static std::pair<QByteArray, QString> extractFromOGG (const QString& filePath);
    static std::pair<QByteArray, QString> extractFromWAV (const QString& filePath);
};
