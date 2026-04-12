#include "CoverArtExtractor.h"

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4coverart.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/wavfile.h>

#include <QDebug>

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

std::pair<QByteArray, QString> CoverArtExtractor::extractCoverArt(const QString& filePath)
{
    QString ext = filePath.section('.', -1).toLower();

    if (ext == "mp3")
        return extractFromMP3(filePath);
    if (ext == "flac")
        return extractFromFLAC(filePath);
    if (ext == "m4a" || ext == "mp4" || ext == "aac")
        return extractFromMP4(filePath);
    if (ext == "ogg")
        return extractFromOGG(filePath);
    if (ext == "wav")
        return extractFromWAV(filePath);

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Bilddaten → Format-String
// ─────────────────────────────────────────────────────────────────────────────

QString CoverArtExtractor::detectImageFormat(const QByteArray& data)
{
    if (data.size() < 4)
        return {};

    auto u = [&](int i) { return static_cast<unsigned char>(data[i]); };

    // JPEG: FF D8 FF
    if (u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF)
        return QStringLiteral("JPEG");

    // PNG: 89 50 4E 47
    if (u(0) == 0x89 && u(1) == 0x50 && u(2) == 0x4E && u(3) == 0x47)
        return QStringLiteral("PNG");

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Format-spezifische Extraktoren
// ─────────────────────────────────────────────────────────────────────────────

std::pair<QByteArray, QString> CoverArtExtractor::extractFromMP3(const QString& filePath)
{
    TagLib::MPEG::File file(filePath.toUtf8().constData());
    if (!file.isValid()) return {};

    auto* tag = file.ID3v2Tag();
    if (!tag) return {};

    auto frameList = tag->frameListMap()["APIC"];
    if (frameList.isEmpty()) return {};

    auto* frame = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(frameList.front());
    if (!frame) return {};

    TagLib::ByteVector pic = frame->picture();
    QByteArray data(pic.data(), static_cast<qsizetype>(pic.size()));
    QString fmt = detectImageFormat(data);

    if (!fmt.isEmpty()) {
        qDebug() << "[CoverArt] MP3:" << data.size() << "bytes" << fmt;
        return {data, fmt};
    }
    return {};
}

std::pair<QByteArray, QString> CoverArtExtractor::extractFromFLAC(const QString& filePath)
{
    TagLib::FLAC::File file(filePath.toUtf8().constData());
    if (!file.isValid()) return {};

    auto pictures = file.pictureList();
    if (pictures.isEmpty()) return {};

    auto* picture = pictures.front();
    if (!picture) return {};

    TagLib::ByteVector pic = picture->data();
    QByteArray data(pic.data(), static_cast<qsizetype>(pic.size()));
    QString fmt = detectImageFormat(data);

    if (!fmt.isEmpty()) {
        qDebug() << "[CoverArt] FLAC:" << data.size() << "bytes" << fmt;
        return {data, fmt};
    }
    return {};
}

std::pair<QByteArray, QString> CoverArtExtractor::extractFromMP4(const QString& filePath)
{
    TagLib::MP4::File file(filePath.toUtf8().constData());
    if (!file.isValid()) return {};

    auto* tag = file.tag();
    if (!tag) return {};

    auto items = tag->itemMap();
    if (!items.contains("covr")) return {};

    auto coverList = items["covr"].toCoverArtList();
    if (coverList.isEmpty()) return {};

    TagLib::ByteVector pic = coverList.front().data();
    QByteArray data(pic.data(), static_cast<qsizetype>(pic.size()));
    QString fmt = detectImageFormat(data);

    if (!fmt.isEmpty()) {
        qDebug() << "[CoverArt] MP4:" << data.size() << "bytes" << fmt;
        return {data, fmt};
    }
    return {};
}

std::pair<QByteArray, QString> CoverArtExtractor::extractFromOGG(const QString& filePath)
{
    TagLib::Ogg::Vorbis::File file(filePath.toUtf8().constData());
    if (!file.isValid()) return {};

    auto* tag = dynamic_cast<TagLib::Ogg::XiphComment*>(file.tag());
    if (!tag) return {};

    auto fieldMap = tag->fieldListMap();
    if (!fieldMap.contains("METADATA_BLOCK_PICTURE")) return {};

    auto field = fieldMap["METADATA_BLOCK_PICTURE"];
    if (field.isEmpty()) return {};

    TagLib::ByteVector base64 = field.front().data(TagLib::String::UTF8);
    TagLib::ByteVector picBlock = TagLib::ByteVector::fromBase64(base64);

// FLAC picture block: parse header to locate the raw image bytes.
// Format: type(4) + mime_len(4) + mime + desc_len(4) + desc + w(4) + h(4) + depth(4) + colors(4) + data_len(4) + data
// Simpler approach: scan for JPEG/PNG magic bytes directly.
    if (picBlock.size() > 32) {
        const char* raw = picBlock.data();
        int sz = static_cast<int>(picBlock.size());
        for (int i = 0; i < sz - 4; ++i) {
            auto u = [&](int off) { return static_cast<unsigned char>(raw[off]); };
            bool isJpeg = (u(i) == 0xFF && u(i+1) == 0xD8 && u(i+2) == 0xFF);
            bool isPng  = (u(i) == 0x89 && u(i+1) == 0x50 && u(i+2) == 0x4E && u(i+3) == 0x47);
            if (isJpeg || isPng) {
                QByteArray data(raw + i, sz - i);
                QString fmt = detectImageFormat(data);
                if (!fmt.isEmpty()) {
                    qDebug() << "[CoverArt] OGG:" << data.size() << "bytes" << fmt;
                    return {data, fmt};
                }
            }
        }
    }
    return {};
}

std::pair<QByteArray, QString> CoverArtExtractor::extractFromWAV(const QString& filePath)
{
    TagLib::RIFF::WAV::File file(filePath.toUtf8().constData());
    if (!file.isValid()) return {};

    auto* tag = file.ID3v2Tag();
    if (!tag) return {};

    auto frameList = tag->frameListMap()["APIC"];
    if (frameList.isEmpty()) return {};

    auto* frame = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(frameList.front());
    if (!frame) return {};

    TagLib::ByteVector pic = frame->picture();
    QByteArray data(pic.data(), static_cast<qsizetype>(pic.size()));
    QString fmt = detectImageFormat(data);

    if (!fmt.isEmpty()) {
        qDebug() << "[CoverArt] WAV:" << data.size() << "bytes" << fmt;
        return {data, fmt};
    }
    return {};
}
