#pragma once

#include "waveform/WaveformAggregator.h"

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
#include <cstdint>
#include <optional>

class DjEngine;

namespace flx10_protocol {

constexpr uint16_t kVid = 0x2B73;
constexpr uint16_t kPid = 0x0041;
constexpr int kScreenInterface = 5;
constexpr int kHidPacketSize = 128;
constexpr double kPreviewDurationSeconds = 30.0;
constexpr double kJogWaveformEntriesPerSecond = 150.0;
constexpr int kMaxWaveformEntries = 0x7FF00;
// Each xx36 packet carries 19 PWV5 entries (38 payload bytes).
constexpr int kXx36EntriesPerWindow = 19;
// A 19-entry window every 10 ms fills a 5-minute track (2368 windows) in
// about 24 s. An earlier pass throttled this to one window per 95 ms after
// reading a "~200 entries/sec" figure in a prose protocol summary; at that
// rate the same track needs nearly four minutes, so the display only ever
// showed scattered half-filled segments around the playhead. The captures
// show Serato pushing its ~420-490 xx36 packets per deck in a burst at
// roughly 1 ms spacing, so 10 ms per window remains well inside what the
// endpoint sustains while still leaving room for xx27 state packets.
// Windows pushed per upload tick. At one window per 10 ms tick a five-minute
// track took over half a minute to reach the screen; four keeps the transfer
// under ten seconds while staying far below what the interrupt endpoint can
// carry, and the platter state has its own priority slot ahead of this queue.
// Keep enough endpoint headroom for the two 200 Hz xx27 streams. Two windows
// per tick still fills a five-minute waveform in about 17 seconds while idle,
// without pushing total traffic close to the one-report-per-USB-frame ceiling.
constexpr int kUploadWindowsPerTick = 2;
constexpr int kUploadTickIntervalMs = 10;
constexpr int kKeepAliveIntervalMs = 250;
constexpr std::size_t kHidWriteQueueCapacity = 1024;
// A screen report normally completes in one USB frame. The previous 250 ms x
// four-attempt policy let one stuck bulk packet freeze the priority platter
// stream for over a second. Eight frames plus one retry tolerate a transient
// miss while bounding a stall to a duration the jog LCD cannot visibly hold.
constexpr int kHidTransferTimeoutMs = 8;
constexpr int kHidTransientRetries = 1;
constexpr int kAlbumArtMaxBytes = 119 + 122 * 254;
constexpr double kJogRingWarningSeconds = 30.0;
constexpr qint64 kJogRingBlinkIntervalMs = 500;
constexpr int kJogRingOnValue = 0x7F;
// The working FLX10 path drives xx27 at 200 Hz. Keeping this independent from
// the 60 Hz UI/display clock is important: applying a 20 ms limiter to a 16.7 ms
// display callback discarded every second callback, so normal playback only
// reached the jog marker at about 30 Hz. Scratch progress signals happened
// between display ticks and masked that scheduling alias.
constexpr int kJogStateIntervalMs = 5;
// Even when the encoded state has not changed, resend it at least this often.
// The firmware needs a continuous stream; permanent dedup on an idle or paused
// deck stops the display updating entirely until something moves again.
constexpr int kXx27HeartbeatIntervalMs = 100;
// 20 Hz playhead keepalive window: the firmware drops the waveform after
// about a minute without it. Independent of the bulk fill above.
constexpr int kXx36TrickleIntervalMs = 50;
constexpr double kJogRevolutionSeconds = 1.8;
constexpr int kJogPhaseTicksPerSecond = 2000;
constexpr int kJogPhaseTicksPerRevolution = 3600;
constexpr int kMaximumDisplayMinutes = 255;

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

// One control-tick-consistent source-timeline snapshot. Fast display fields
// are encoded from this value object instead of re-reading DjEngine between
// individual bytes while MIDI/transport state may be changing.
struct DeckDisplaySnapshot
{
    double sourcePositionSec = 0.0;
    double trackDurationSec = 0.0;
    double bpm = 0.0;
    double tempoPercent = 0.0;
    bool playing = false;
    bool scratching = false;
    bool reverse = false;
    uint8_t keyByte = 0x80;
};

struct Xx27TimelineEncoding
{
    qint64 elapsedMilliseconds = 0;
    qint64 durationMilliseconds = 1000;

    [[nodiscard]] double progress() const noexcept
    {
        return durationMilliseconds > 0
            ? std::clamp(static_cast<double>(elapsedMilliseconds)
                             / static_cast<double>(durationMilliseconds),
                         0.0,
                         1.0)
            : 0.0;
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

inline Xx27TimelineEncoding xx27TimelineEncoding(double sourcePositionSeconds,
                                                 double sourceDurationSeconds) noexcept
{
    const double duration = std::isfinite(sourceDurationSeconds)
        ? std::max(1.0, sourceDurationSeconds) : 1.0;
    const double position = std::clamp(
        std::isfinite(sourcePositionSeconds) ? sourcePositionSeconds : 0.0,
        0.0,
        duration);
    constexpr qint64 maximumDisplayMs = kMaximumDisplayMinutes * 60000LL + 59999LL;
    return {
        std::min(static_cast<qint64>(std::floor(position * 1000.0)), maximumDisplayMs),
        std::min(static_cast<qint64>(std::floor(duration * 1000.0)), maximumDisplayMs)
    };
}

// Resolve one indexed xx36 window inside a sweep whose origin was captured
// once at start. Re-evaluating the origin from a moving playhead between
// packets skips windows and leaves a blank region on the jog display.
inline int waveformSweepWindow(int startWindow, int windowsSent,
                               int totalWindows) noexcept
{
    if (totalWindows <= 0)
        return 0;
    const int normalizedStart = ((startWindow % totalWindows) + totalWindows)
        % totalWindows;
    const int normalizedOffset = std::max(0, windowsSent) % totalWindows;
    return (normalizedStart + normalizedOffset) % totalWindows;
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

inline QByteArray encodeXx27Packet(int deck, const DeckDisplaySnapshot& snapshot)
{
    const uint8_t db = deckByte(deck);
    QByteArray p = packet();
    put8(p, 0, db);
    put8(p, 1, 0x27);
    put8(p, 2, 0xB4);
    put8(p, 3, 0x80);
    put8(p, 4, 0x01);

    const Xx27TimelineEncoding timeline = xx27TimelineEncoding(
        snapshot.sourcePositionSec, snapshot.trackDurationSec);
    const qint64 elapsedSeconds = timeline.elapsedMilliseconds / 1000;
    const int elapsedSubMs = static_cast<int>(timeline.elapsedMilliseconds % 1000);
    put8(p, 5, (elapsedSeconds / 60) & 0xFF);
    put8(p, 6, (elapsedSeconds % 60) & 0xFF);
    put8(p, 7, elapsedSubMs & 0xFF);
    put8(p, 8, (elapsedSubMs >> 8) & 0x03);

    // Track progress is always sourcePosition / sourceDuration. Tempo affects
    // how quickly sourcePosition evolves, never the absolute timeline scale.
    put8(p, 9, timeline.durationMilliseconds / 60000);
    const int remainingDurationMs = static_cast<int>(
        timeline.durationMilliseconds % 60000);
    put8(p, 10, remainingDurationMs / 1000);
    put8(p, 11, remainingDurationMs % 1000);
    put8(p, 12, (remainingDurationMs % 1000) >> 8);

    const double bpm = std::isfinite(snapshot.bpm)
        ? std::clamp(snapshot.bpm, 0.0, 255.9) : 0.0;
    const int bpmInteger = static_cast<int>(bpm);
    put8(p, 13, bpmInteger);
    put8(p, 14,
         (static_cast<int>(std::round((bpm - bpmInteger) * 10.0)) & 0x0F) << 4);
    put8(p, 15, 0x01);

    const double tempoPercent = std::isfinite(snapshot.tempoPercent)
        ? std::clamp(snapshot.tempoPercent, -100.0, 100.0) : 0.0;
    const int tempoEncoded = std::clamp(
        static_cast<int>(std::llround(tempoPercent * 100.0)), -32768, 32767);
    const auto tempoWire = static_cast<uint16_t>(tempoEncoded & 0xFFFF);
    put8(p, 16, tempoWire & 0xFF);
    put8(p, 17, (tempoWire >> 8) & 0xFF);
    put8(p, 20, 0x0E);

    const JogPhaseBytes phase = jogPhaseBytes(snapshot.sourcePositionSec);
    put8(p, 21, phase.low);
    put8(p, 22, phase.high);
    put8(p, 25, 0x80);
    put8(p, 29, snapshot.keyByte);
    put8(p, 30, 0x0D);
    put8(p, 31, displayDeckState(db));
    put8(p, 32, 0xFF);
    put8(p, 33, 0xFF);
    put8(p, 34, 0xFF);
    return p;
}

struct PendingDisplayPacket
{
    QByteArray bytes;
    std::uint64_t sequence = 0;
};

// Fixed-size latest-state slots. Callers provide synchronization; keeping this
// policy separate makes the actual coalescing/backlog contract testable without
// USB hardware.
class LatestDisplayPacketSlots
{
public:
    [[nodiscard]] bool publish(const QByteArray& bytes, std::uint64_t sequence)
    {
        const int deck = packetDeck(bytes);
        if (deck < 1 || deck > 4)
            return false;
        const bool replaced = m_pending[static_cast<std::size_t>(deck)].has_value();
        m_pending[static_cast<std::size_t>(deck)] = PendingDisplayPacket {bytes, sequence};
        return replaced;
    }

    [[nodiscard]] std::optional<PendingDisplayPacket> takeNext()
    {
        for (int offset = 0; offset < 4; ++offset) {
            const int deck = 1 + ((m_nextDeck - 1 + offset) % 4);
            auto& slot = m_pending[static_cast<std::size_t>(deck)];
            if (!slot)
                continue;
            auto result = std::move(slot);
            slot.reset();
            m_nextDeck = 1 + (deck % 4);
            return result;
        }
        return std::nullopt;
    }

    void clear()
    {
        for (auto& slot : m_pending)
            slot.reset();
        m_nextDeck = 1;
    }

    void clearDeck(int deck)
    {
        if (deck >= 1 && deck <= 4)
            m_pending[static_cast<std::size_t>(deck)].reset();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return std::none_of(m_pending.begin(), m_pending.end(),
                            [](const auto& slot) { return slot.has_value(); });
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            m_pending.begin(), m_pending.end(),
            [](const auto& slot) { return slot.has_value(); }));
    }

private:
    [[nodiscard]] static int packetDeck(const QByteArray& bytes) noexcept
    {
        if (bytes.size() != kHidPacketSize || bytes.at(1) != char(0x27))
            return 0;
        const int value = static_cast<unsigned char>(bytes.at(0));
        return value % 0x10 == 0 ? value / 0x10 : 0;
    }

    std::array<std::optional<PendingDisplayPacket>, 5> m_pending;
    int m_nextDeck = 1;
};

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

// Draw a marker into the waveform data itself. The FLX10's native cue/beat
// overlay commands are not active in the Serato HID waveform mode used here,
// but PWV5 is. Encoding the markers as bright waveform columns therefore uses
// the one rendering path known to be visible on the hardware.
inline int waveformEntryForTimeline(double positionSeconds,
                                    double durationSeconds,
                                    int entryCount) noexcept
{
    if (entryCount <= 0 || !std::isfinite(positionSeconds)
        || !std::isfinite(durationSeconds) || durationSeconds <= 0.0
        || positionSeconds < 0.0 || positionSeconds > durationSeconds) {
        return -1;
    }

    if (entryCount == 1)
        return 0;
    const double progress = std::clamp(positionSeconds / durationSeconds, 0.0, 1.0);
    return std::clamp(static_cast<int>(std::llround(progress * (entryCount - 1))),
                      0, entryCount - 1);
}

inline bool overlayPwv5Marker(QByteArray& waveform, double positionSeconds,
                              double durationSeconds,
                              int radiusEntries, int height,
                              int red, int green, int blue) noexcept
{
    const int entryCount = waveform.size() / 2;
    const int marker = waveformEntryForTimeline(
        positionSeconds, durationSeconds, entryCount);
    if (marker < 0) {
        return false;
    }

    const QByteArray encoded = encodePwv5Entry(height, red, green, blue);
    const int radius = std::max(0, radiusEntries);
    const int first = std::max(0, marker - radius);
    const int last = std::min(entryCount - 1, marker + radius);
    for (int entry = first; entry <= last; ++entry) {
        waveform[entry * 2] = encoded[0];
        waveform[entry * 2 + 1] = encoded[1];
    }
    return true;
}

// Recolour an entry without touching its height. A beat line drawn with
// overlayPwv5Marker() would replace the audio column with a flat bar, which
// invents a transient where the track has none and makes the grid read as part
// of the waveform. Keeping the measured height and changing only the colour
// puts the grid behind the audio instead of on top of it. minimumHeight lifts
// a beat that falls in near-silence just far enough to stay legible; it never
// lowers a loud column.
inline bool tintPwv5Entry(QByteArray& waveform, int entry,
                          int red, int green, int blue,
                          int minimumHeight) noexcept
{
    const int entryCount = waveform.size() / 2;
    if (entry < 0 || entry >= entryCount)
        return false;

    const int value = (static_cast<unsigned char>(waveform[entry * 2]))
        | (static_cast<unsigned char>(waveform[entry * 2 + 1]) << 8);
    const int height = std::max((value >> 2) & 0x1F,
                                std::clamp(minimumHeight, 0, 31));
    const QByteArray encoded = encodePwv5Entry(height, red, green, blue);
    waveform[entry * 2] = encoded[0];
    waveform[entry * 2 + 1] = encoded[1];
    return true;
}

// Adapter only: quantises one shared WaveformColumn into a PWV5 entry. It
// performs no aggregation, no LOD selection and no colour analysis of its own
// — every one of those decisions was already made by
// waveform::aggregateWaveformColumn(), which the desktop renderer uses too.
inline QByteArray encodePwv5Column(const waveform::WaveformColumn& column)
{
    if (!column.hasData)
        return encodePwv5Entry(1, 0, 0, 0);
    // Square root keeps quiet passages legible on the small 5-bit height axis.
    const int height = std::clamp(static_cast<int>(std::lround(
        std::sqrt(column.amplitude()) * 31.0f)), 1, 31);
    const auto to3Bit = [](std::uint8_t value) {
        return std::clamp((static_cast<int>(value) + 15) / 32, 0, 7);
    };
    return encodePwv5Entry(height, to3Bit(column.red), to3Bit(column.green),
                           to3Bit(column.blue));
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
