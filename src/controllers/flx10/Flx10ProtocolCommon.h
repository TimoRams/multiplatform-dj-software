#pragma once

#include <QByteArray>
#include <QBuffer>
#include <QChar>
#include <QImage>
#include <QIODevice>
#include <QList>
#include <QPainter>
#include <QRegularExpression>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>

class DjEngine;

namespace flx10_protocol {

constexpr uint16_t kVid = 0x2B73;
constexpr uint16_t kPid = 0x0041;
constexpr int kScreenInterface = 5;
constexpr int kHidPacketSize = 128;
constexpr double kPreviewDurationSeconds = 30.0;
constexpr double kJogWaveformEntriesPerSecond = 150.0;
constexpr int kMaxWaveformEntries = 0x7FF00;
// One 19-entry window per tick keeps the USB interrupt endpoint comfortably
// below saturation while both decks are uploading analysis data.
constexpr int kUploadWindowsPerTick = 1;
constexpr int kUploadTickIntervalMs = 10;
constexpr std::size_t kHidWriteQueueCapacity = 1024;
constexpr int kHidTransferTimeoutMs = 250;
constexpr int kHidTransientRetries = 3;
constexpr int kAlbumArtMaxBytes = 119 + 122 * 254;
constexpr double kJogRingWarningSeconds = 30.0;
constexpr qint64 kJogRingBlinkIntervalMs = 500;
constexpr int kJogRingOnValue = 0x7F;
constexpr int kXx2fSampleRate = 22050;
constexpr int kXx2fRecordsPerPacket = 30;
constexpr uint32_t kXx2fMaximumSample = 0x00FFFFFFu;
constexpr int kXx36TrickleIntervalMs = 50;
constexpr double kJogRevolutionSeconds = 1.8;
constexpr int kJogPhaseTicksPerSecond = 2000;
constexpr int kJogPhaseTicksPerRevolution = 3600;
constexpr int kMaximumDisplayMinutes = 255;
constexpr std::array<uint8_t, 4> kXx2fBeatTypes = {0x03, 0x04, 0x00, 0x02};
constexpr std::array<uint8_t, 4> kXx2fStartMarker = {0x80, 0x02, 0x01, 0x00};

struct VendorUnlockCommand
{
    uint16_t value;
    uint16_t index;
};

struct JogPhaseBytes
{
    uint8_t low = 0;
    uint8_t high = 0;

    [[nodiscard]] constexpr uint16_t value() const noexcept
    {
        return static_cast<uint16_t>(low)
            | (static_cast<uint16_t>(high) << 8);
    }
};

inline JogPhaseBytes jogPhaseBytes(double fileElapsedSeconds) noexcept
{
    if (!std::isfinite(fileElapsedSeconds) || fileElapsedSeconds <= 0.0)
        return {};

    double revolutionSeconds = std::fmod(fileElapsedSeconds, kJogRevolutionSeconds);
    if (revolutionSeconds < 0.0)
        revolutionSeconds += kJogRevolutionSeconds;
    const int ticks = std::clamp(
        static_cast<int>(std::floor(revolutionSeconds * kJogPhaseTicksPerSecond)),
        0,
        kJogPhaseTicksPerRevolution - 1);
    return {
        static_cast<uint8_t>(ticks & 0xFF),
        static_cast<uint8_t>((ticks >> 8) & 0xFF)
    };
}

inline bool xx2fSampleForMilliseconds(double milliseconds, uint32_t& sample) noexcept
{
    if (!std::isfinite(milliseconds) || milliseconds < 0.0)
        return false;
    const double sampleValue = milliseconds * static_cast<double>(kXx2fSampleRate) / 1000.0;
    if (sampleValue > static_cast<double>(kXx2fMaximumSample))
        return false;
    sample = static_cast<uint32_t>(std::llround(sampleValue));
    return sample <= kXx2fMaximumSample;
}

constexpr VendorUnlockCommand kVendorUnlockCommands[] = {
    {0x0100, 0xC028},
    {0x0000, 0xC029},
    {0x0200, 0xC013},
    {0x0000, 0xC02B},
    {0x0100, 0xC026},
    {0x0000, 0xC01D},
    {0x0100, 0xC027},
};

constexpr const char* kSessionStart = "F0 00 20 7F 01 02 01 01 22 0F 0C 06 08 04 0A 02 02 05 00 00 0E 0A 0E 03 04 F7";
constexpr const char* kEnterHid = "F0 00 40 05 00 00 04 01 00 03 01 F7";
constexpr const char* kKeepAlive = "F0 00 40 05 00 00 04 01 00 50 31 F7";
constexpr const char* kDeckInit[] = {
    "",
    "F0 00 40 05 00 00 04 01 00 11 00 00 02 0E 0E 05 F7",
    "F0 00 40 05 00 00 04 01 00 12 00 00 02 0E 0E 05 F7",
    "F0 00 40 05 00 00 04 01 00 13 00 00 02 0E 0E 05 F7",
    "F0 00 40 05 00 00 04 01 00 14 00 00 02 0E 0E 05 F7",
};
constexpr const char* kGlobalB = "F0 00 40 05 00 00 04 01 00 0B 31 00 00 00 00 00 F7";
constexpr const char* kGlobalC = "F0 00 40 05 00 00 04 01 00 0C 00 00 02 0E 0E 05 00 01 F7";

inline const QList<QByteArray> kXx39Packets = {
    QByteArrayLiteral("10390100030000484f54204355450000000000000000000000000000000000000000000000003f000000000000000000000000000000000000000000000000000000000000023f000000000000000000000000000000000000000000000000000000000000023f00000000000000000000000000000000000000000000000000"),
    QByteArrayLiteral("1039020003000000000000023f000000000000000000000000000000000000000000000000000000000000023f000000000000000000000000000000000000000000000000000000000000023f000000000000000000000000000000000000000000000000000000000000023f00000000000000000000000000000000000000"),
    QByteArrayLiteral("1039030003000000000000000000000000023f00000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"),
};

inline uint8_t deckByte(int deck)
{
    return static_cast<uint8_t>(std::clamp(deck, 1, 4) * 0x10);
}

inline uint8_t displayDeckState(uint8_t db)
{
    switch (db) {
    case 0x10: return 0x02;
    case 0x20: return 0x01;
    case 0x30: return 0x04;
    case 0x40: return 0x03;
    default: return 0x02;
    }
}

inline QByteArray packet()
{
    return QByteArray(kHidPacketSize, char(0));
}

inline uint8_t camelotKeyByte(int number, QChar side)
{
    static constexpr std::array<uint8_t, 13> kCamelotA = {
        0x80, 0x98, 0x8E, 0x84, 0x92, 0x88, 0x96,
        0x8C, 0x82, 0x90, 0x86, 0x94, 0x8A
    };
    static constexpr std::array<uint8_t, 13> kCamelotB = {
        0x99, 0x97, 0x8D, 0x83, 0x91, 0x87, 0x95,
        0x8B, 0x81, 0x8F, 0x85, 0x93, 0x89
    };

    if (number < 1 || number > 12)
        return 0x80;

    return side.toUpper() == QLatin1Char('B') ? kCamelotB[number] : kCamelotA[number];
}

inline uint8_t musicalKeyByte(QString key)
{
    const QString original = key.trimmed();
    if (original.isEmpty())
        return 0x80;

    static const QRegularExpression camelotRe(
        QStringLiteral("\\b(1[0-2]|[1-9])\\s*([AB])\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch camelotMatch = camelotRe.match(original);
    if (camelotMatch.hasMatch())
        return camelotKeyByte(camelotMatch.captured(1).toInt(), camelotMatch.captured(2).at(0));

    static const QRegularExpression noteRe(
        QStringLiteral("^\\s*([A-GH])\\s*([#♯b♭]?)(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch noteMatch = noteRe.match(original);
    if (!noteMatch.hasMatch())
        return 0x80;

    QString note = noteMatch.captured(1).toUpper();
    QString accidental = noteMatch.captured(2);
    const QString rest = noteMatch.captured(3).trimmed().toLower();
    accidental.replace(QStringLiteral("♯"), QStringLiteral("#"));
    accidental.replace(QStringLiteral("♭"), QStringLiteral("b"));
    accidental = accidental.toLower();

    int semitone = -1;
    if (note == QLatin1String("C")) semitone = 0;
    else if (note == QLatin1String("D")) semitone = 2;
    else if (note == QLatin1String("E")) semitone = 4;
    else if (note == QLatin1String("F")) semitone = 5;
    else if (note == QLatin1String("G")) semitone = 7;
    else if (note == QLatin1String("A")) semitone = 9;
    else if (note == QLatin1String("B") || note == QLatin1String("H")) semitone = 11;

    if (semitone < 0)
        return 0x80;
    if (accidental == QLatin1String("#"))
        semitone = (semitone + 1) % 12;
    else if (accidental == QLatin1String("b"))
        semitone = (semitone + 11) % 12;

    const bool minor = rest.startsWith(QLatin1Char('m')) && !rest.startsWith(QLatin1String("maj"));
    static constexpr std::array<int, 12> kMajorCamelot = {8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1};
    static constexpr std::array<int, 12> kMinorCamelot = {5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10};
    return minor ? camelotKeyByte(kMinorCamelot[semitone], QLatin1Char('A'))
                 : camelotKeyByte(kMajorCamelot[semitone], QLatin1Char('B'));
}

inline QByteArray bytesFromHexString(QString hex)
{
    hex.remove(QLatin1Char(' '));
    hex.remove(QLatin1Char('\n'));
    hex.remove(QLatin1Char('\r'));
    hex.remove(QLatin1Char('\t'));
    return QByteArray::fromHex(hex.toLatin1());
}

inline void put8(QByteArray& p, int index, int value)
{
    if (index < 0 || index >= p.size())
        return;
    p[index] = static_cast<char>(value & 0xFF);
}

inline QByteArray encodePwv5Entry(int height, int red, int green, int blue)
{
    height = std::clamp(height, 0, 31);
    red = std::clamp(red, 0, 7);
    green = std::clamp(green, 0, 7);
    blue = std::clamp(blue, 0, 7);
    const int value = (red << 13) | (green << 10) | (blue << 7) | (height << 2);
    QByteArray out;
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    return out;
}

inline QByteArray encodeCoverJpeg(const QImage& source, int side, int quality)
{
    if (source.isNull())
        return {};

    QImage canvas(side, side, QImage::Format_RGB888);
    canvas.fill(Qt::black);

    const QImage scaled = source.convertToFormat(QImage::Format_RGB888)
                             .scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QPainter painter(&canvas);
    painter.drawImage((side - scaled.width()) / 2, (side - scaled.height()) / 2, scaled);
    painter.end();

    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    canvas.save(&buffer, "JPEG", quality);
    return out;
}

} // namespace flx10_protocol
