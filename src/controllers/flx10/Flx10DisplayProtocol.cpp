#include "DDJFLX10Controller.h"

#include "DjEngine.h"
#include "domain/TrackData.h"

#include <QDateTime>
#include <QDebug>
#include <QBuffer>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "Flx10ProtocolCommon.h"

using namespace flx10_protocol;

namespace {

double validTrackDuration(const DjEngine* engine)
{
    if (!engine || !std::isfinite(engine->getDuration()) || engine->getDuration() <= 0.0f)
        return kPreviewDurationSeconds;

    return std::max(1.0, static_cast<double>(engine->getDuration()));
}

double validTrackPosition(const DjEngine* engine, double duration)
{
    if (!engine)
        return 0.0;

    // Match TurntableIndicator / Waveform: atomic during scratch & release glide,
    // interpolated visual position while playing, frozen atomic when paused.
    const double pos = engine->isScratchVisualActive()
        ? engine->getPlayheadPositionAtomic()
        : (engine->isPlaying() ? engine->getVisualPosition()
                               : engine->getPlayheadPositionAtomic());
    if (!std::isfinite(pos))
        return 0.0;
    return std::clamp(pos, 0.0, duration);
}

} // namespace

void DDJFLX10Controller::resetDisplayPacketState(int deck)
{
    if (deck < 0 || deck >= static_cast<int>(m_lastXx27Packet.size()))
        return;
    m_lastXx27Packet[deck].clear();
    m_lastXx27SentMs[deck] = -kJogStateIntervalMs;
}
void DDJFLX10Controller::pushDeckJogDisplay(int deck)
{
    if (deck < 1 || deck > 2)
        return;
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;

    const DeckDisplaySnapshot snapshot = captureDeckDisplaySnapshot(deck);
    sendXx27(deck, snapshot);
    updateJogRingWarning(deck,
                         snapshot.sourcePositionSec,
                         snapshot.trackDurationSec,
                         snapshot.playing);
}
bool DDJFLX10Controller::uploadDeck(int deck)
{
    if (m_waveforms[deck].isEmpty())
        return true;

    bool ok = true;
    ok = sendXx30(deck) && ok;
    ok = sendXx39(deck) && ok;
    ok = uploadCoverArt(deck) && ok;
    ok = sendXx35(deck, m_waveforms[deck].size() / 2) && ok;

    m_uploadEntries[deck] = 0;
    m_uploadActive[deck] = true;
    if (!m_uploadTimer.isActive())
        m_uploadTimer.start(kUploadTickIntervalMs);
    return ok;
}
bool DDJFLX10Controller::sendXx30(int deck)
{
    QByteArray p = packet();
    put8(p, 0, deckByte(deck));
    put8(p, 1, 0x30);
    put8(p, 2, 0x01);
    put8(p, 4, 0x01);
    for (int index : {10, 16, 22, 28, 34, 40, 46, 52})
        put8(p, index, 0xFF);
    return writePacket(p);
}
bool DDJFLX10Controller::sendXx39(int deck)
{
    bool ok = true;
    for (int packetIndex = 0; packetIndex < kXx39Packets.size(); ++packetIndex) {
        const QByteArray& hex = kXx39Packets[packetIndex];
        QByteArray p = QByteArray::fromHex(hex);
        if (p.size() < kHidPacketSize)
            p.resize(kHidPacketSize);
        if (p.size() > kHidPacketSize)
            p.truncate(kHidPacketSize);
        put8(p, 0, deckByte(deck));
        ok = writePacket(p) && ok;
    }
    return ok;
}
bool DDJFLX10Controller::sendXx33Album(int deck, const QByteArray& jpeg)
{
    if (jpeg.isEmpty())
        return true;

    const int firstCapacity = 119;
    const int nextCapacity = 122;
    const int maxBytes = kAlbumArtMaxBytes;
    const int jpegSize = std::min(static_cast<int>(jpeg.size()), maxBytes);
    const int totalPackets = jpegSize <= firstCapacity
                                 ? 1
                                 : 1 + ((jpegSize - firstCapacity + nextCapacity - 1) / nextCapacity);

    int offset = 0;
    bool ok = true;
    for (int segment = 1; segment <= totalPackets; ++segment) {
        QByteArray p = packet();
        put8(p, 0, deckByte(deck));
        put8(p, 1, 0x33);
        put8(p, 2, segment);
        put8(p, 4, totalPackets);

        if (segment == 1) {
            put8(p, 6, jpegSize);
            put8(p, 7, jpegSize >> 8);
            const int take = std::min(firstCapacity, jpegSize - offset);
            p.replace(9, take, jpeg.constData() + offset, take);
            offset += take;
        } else {
            const int take = std::min(nextCapacity, jpegSize - offset);
            p.replace(6, take, jpeg.constData() + offset, take);
            offset += take;
        }

        ok = writePacket(p) && ok;
    }
    return ok;
}
bool DDJFLX10Controller::sendXx35(int deck, int entryCount)
{
    QByteArray clear = packet();
    put8(clear, 0, deckByte(deck));
    put8(clear, 1, 0x35);
    bool ok = writePacket(clear);

    for (int i = 0; i < 2; ++i) {
        QByteArray p = packet();
        put8(p, 0, deckByte(deck));
        put8(p, 1, 0x35);
        put8(p, 2, entryCount);
        put8(p, 3, entryCount >> 8);
        put8(p, 4, entryCount >> 16);
        put8(p, 5, entryCount >> 24);
        ok = writePacket(p) && ok;
    }
    return ok;
}
bool DDJFLX10Controller::sendXx36Window(int deck, const QByteArray& waveform, int entry)
{
    const int entryCount = waveform.size() / 2;
    if (entryCount <= 0)
        return false;

    entry = std::clamp(entry, 0, std::max(0, entryCount - 19));
    const int take = std::min(19, entryCount - entry);

    QByteArray p = packet();
    put8(p, 0, deckByte(deck));
    put8(p, 1, 0x36);
    put8(p, 2, 0x01);
    put8(p, 4, 0x01);
    put8(p, 6, 0x13);
    put8(p, 10, entry);
    put8(p, 11, entry >> 8);
    put8(p, 12, entry >> 16);
    put8(p, 13, entry >> 24);
    p.replace(14, take * 2, waveform.mid(entry * 2, take * 2));
    return writePacket(p);
}
bool DDJFLX10Controller::sendXx2f(int deck)
{
    struct Xx2fRecord {
        uint8_t type = 0;
        uint32_t samples = 0;
    };

    std::vector<Xx2fRecord> records;
    records.reserve(1 + 512);
    records.push_back({kXx2fStartMarker[0],
                       static_cast<uint32_t>(kXx2fStartMarker[1]
                                             | (kXx2fStartMarker[2] << 8)
                                             | (kXx2fStartMarker[3] << 16))});

    const std::vector<double> beatTimesMs = deckBeatTimesMs(deck);
    for (size_t i = 0; i < beatTimesMs.size(); ++i) {
        uint32_t samples = 0;
        if (!xx2fSampleForMilliseconds(beatTimesMs[i], samples))
            continue;
        records.push_back({kXx2fBeatTypes[i % kXx2fBeatTypes.size()], samples});
    }

    const int totalPackets = std::max(1, static_cast<int>(
        (records.size() + kXx2fRecordsPerPacket - 1) / kXx2fRecordsPerPacket));
    const int packetsToSend = std::min(totalPackets, 255);

    bool ok = true;
    for (int packetIndex = 0; packetIndex < packetsToSend; ++packetIndex) {
        QByteArray p = packet();
        put8(p, 0, deckByte(deck));
        put8(p, 1, 0x2F);
        put8(p, 2, packetIndex + 1);
        put8(p, 3, 0x00);
        put8(p, 4, 0x15);
        put8(p, 5, 0x00);

        int offset = 6;
        const size_t firstRecord = static_cast<size_t>(packetIndex * kXx2fRecordsPerPacket);
        const size_t endRecord = std::min(records.size(), firstRecord + kXx2fRecordsPerPacket);
        for (size_t recordIndex = firstRecord; recordIndex < endRecord; ++recordIndex) {
            if (offset + 3 >= kHidPacketSize)
                break;
            const Xx2fRecord& record = records[recordIndex];
            put8(p, offset, record.type);
            put8(p, offset + 1, record.samples & 0xFF);
            put8(p, offset + 2, (record.samples >> 8) & 0xFF);
            put8(p, offset + 3, (record.samples >> 16) & 0xFF);
            offset += 4;
        }

        ok = writePacket(p) && ok;
    }

    qInfo() << "[DDJ-FLX10] Deck" << deck
            << "sent xx2F beatgrid records" << static_cast<int>(records.size() - 1)
            << "packets" << packetsToSend;
    return ok;
}
bool DDJFLX10Controller::clearDeckDisplay(int deck)
{
    m_uploadActive[deck] = false;
    m_uploadEntries[deck] = 0;

    bool ok = true;
    for (int command : {0x27, 0x30, 0x33, 0x35, 0x36, 0x2F}) {
        QByteArray p = packet();
        put8(p, 0, deckByte(deck));
        put8(p, 1, command);
        ok = writePacket(p) && ok;
    }
    return ok;
}
bool DDJFLX10Controller::sendXx27(int deck, const DeckDisplaySnapshot& snapshot)
{
    const QByteArray p = encodeXx27Packet(deck, snapshot);

    if (deck >= 0 && deck < static_cast<int>(m_lastXx27Packet.size())
        && m_lastXx27Packet[deck] == p) {
        return true;
    }

    // ProgressChanged can fire much faster than the display clock while
    // scratching.  Keep the latest position, but never turn that burst into an
    // unbounded stream of USB interrupt reports.
    const qint64 nowMs = m_hidTrafficClock.isValid() ? m_hidTrafficClock.elapsed() : 0;
    if (deck >= 0 && deck < static_cast<int>(m_lastXx27SentMs.size())
        && nowMs - m_lastXx27SentMs[deck] < kJogStateIntervalMs) {
        return true;
    }

    const bool ok = writePacket(p);
    if (ok && deck >= 0 && deck < static_cast<int>(m_lastXx27Packet.size())) {
        m_lastXx27Packet[deck] = p;
        m_lastXx27SentMs[deck] = nowMs;
    }
    return ok;
}
QByteArray DDJFLX10Controller::generateCoverJpeg(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    if (!engine || !engine->hasTrack() || !engine->hasCoverArt())
        return {};

    const QImage cover = engine->currentCoverImage();
    if (cover.isNull())
        return {};

    for (const auto [side, quality] : {std::pair{240, 86}, std::pair{240, 74}, std::pair{180, 72}, std::pair{160, 66}}) {
        const QByteArray jpeg = encodeCoverJpeg(cover, side, quality);
        if (!jpeg.isEmpty() && jpeg.size() <= kAlbumArtMaxBytes)
            return jpeg;
    }

    return {};
}
bool DDJFLX10Controller::uploadCoverArt(int deck)
{
    const QByteArray jpeg = generateCoverJpeg(deck);
    if (jpeg.isEmpty())
        return true;

    qInfo() << "[DDJ-FLX10] Deck" << deck << "uploading cover art bytes" << jpeg.size();
    return sendXx33Album(deck, jpeg);
}
QByteArray DDJFLX10Controller::generatePreviewWaveform(int deck) const
{
    const DjEngine* engine = deck == 1 ? m_deckA : m_deckB;
    if (!engine || engine->getDuration() <= 0.0f)
        return {};

    TrackData* trackData = engine->getTrackData();
    if (!trackData)
        return {};

    QVector<TrackData::RgbWaveformFrame> frames = trackData->getRgbWaveformData();
    if (frames.isEmpty())
        frames = trackData->getOverviewRgbData();
    if (frames.isEmpty())
        frames = trackData->getProgressiveOvrData();
    if (frames.isEmpty())
        return {};

    const int targetEntries = std::clamp(
        static_cast<int>(std::ceil(deckDisplayDuration(deck) * kJogWaveformEntriesPerSecond)),
        150,
        kMaxWaveformEntries);
    QByteArray out;
    out.reserve(targetEntries * 2);

    for (int i = 0; i < targetEntries; ++i) {
        const double startFraction = static_cast<double>(i) / static_cast<double>(targetEntries);
        const double endFraction = static_cast<double>(i + 1) / static_cast<double>(targetEntries);
        const int startIndex = std::clamp(static_cast<int>(std::floor(startFraction * frames.size())), 0, static_cast<int>(frames.size() - 1));
        const int endIndex = std::clamp(static_cast<int>(std::ceil(endFraction * frames.size())), startIndex + 1, static_cast<int>(frames.size()));

        float rms = 0.0f;
        float low = 0.0f;
        float lowMid = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
        int fallbackRed = 0;
        int fallbackGreen = 0;
        int fallbackBlue = 0;
        int colorCount = 0;

        for (int src = startIndex; src < endIndex; ++src) {
            const auto& frame = frames[src];
            rms = std::max(rms, std::max(0.0f, frame.rms));
            low = std::max(low, std::max(0.0f, frame.low));
            lowMid = std::max(lowMid, std::max(0.0f, frame.lowMid));
            mid = std::max(mid, std::max(0.0f, frame.mid));
            high = std::max(high, std::max(0.0f, frame.high));
            fallbackRed += frame.color.red();
            fallbackGreen += frame.color.green();
            fallbackBlue += frame.color.blue();
            ++colorCount;
        }

        const int height = std::clamp(static_cast<int>(std::sqrt(rms) * 31.0f), 1, 31);
        const float bandMax = std::max({low, lowMid, mid, high, 0.001f});

        int red = std::clamp(static_cast<int>(std::round(7.0f * (0.90f * low + 0.45f * lowMid) / bandMax)), 0, 7);
        int green = std::clamp(static_cast<int>(std::round(7.0f * (0.75f * mid + 0.35f * lowMid) / bandMax)), 0, 7);
        int blue = std::clamp(static_cast<int>(std::round(7.0f * (0.90f * high + 0.25f * mid) / bandMax)), 0, 7);

        if (red == 0 && green == 0 && blue == 0) {
            const QColor color(
                colorCount > 0 ? fallbackRed / colorCount : 255,
                colorCount > 0 ? fallbackGreen / colorCount : 255,
                colorCount > 0 ? fallbackBlue / colorCount : 255);
            red = std::clamp((color.red() + 15) / 32, 0, 7);
            green = std::clamp((color.green() + 15) / 32, 0, 7);
            blue = std::clamp((color.blue() + 15) / 32, 0, 7);
        }

        out += encodePwv5Entry(height, red, green, blue);
    }

    return out;
}
double DDJFLX10Controller::deckDisplayDuration(int deck) const
{
    return validTrackDuration(deckEngine(deck));
}
double DDJFLX10Controller::deckDisplayPosition(int deck) const
{
    return validTrackPosition(deckEngine(deck), deckDisplayDuration(deck));
}
DeckDisplaySnapshot DDJFLX10Controller::captureDeckDisplaySnapshot(int deck)
{
    DeckDisplaySnapshot snapshot;
    if (deck < 1 || deck > 2)
        return snapshot;

    snapshot.generation = ++m_displaySnapshotSequence[deck];
    const DjEngine* engine = deckEngine(deck);
    snapshot.trackDurationSec = deckDisplayDuration(deck);
    if (!engine) {
        snapshot.sourcePositionSec = std::fmod(
            (QDateTime::currentMSecsSinceEpoch() - m_clockStartMs) / 1000.0,
            snapshot.trackDurationSec);
        snapshot.playing = true;
        return snapshot;
    }

    snapshot.sourcePositionSec = validTrackPosition(engine, snapshot.trackDurationSec);
    snapshot.bpm = std::max(0.0, engine->getCurrentBpm());
    snapshot.tempoPercent = engine->getTempoPercent();
    snapshot.playing = engine->isPlaying();
    snapshot.scratching = engine->isScratchVisualActive();
    snapshot.reverse = engine->isReverse();
    snapshot.title = engine->trackTitle();
    snapshot.artist = engine->trackArtist();
    snapshot.keyByte = deckKeyByte(deck);
    return snapshot;
}
double DDJFLX10Controller::deckTempoRangePercent(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    return engine ? engine->tempoRangePercent() : 8.0;
}
QString DDJFLX10Controller::deckKey(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    if (!engine)
        return {};

    if (const TrackData* trackData = engine->getTrackData()) {
        const QString detectedKey = trackData->getDetectedKey().trimmed();
        if (!detectedKey.isEmpty())
            return detectedKey;
    }

    return engine->trackKey().trimmed();
}
uint8_t DDJFLX10Controller::deckKeyByte(int deck) const
{
    return musicalKeyByte(deckKey(deck));
}
std::vector<double> DDJFLX10Controller::deckBeatTimesMs(int deck) const
{
    const DjEngine* engine = deckEngine(deck);
    const TrackData* trackData = engine ? engine->getTrackData() : nullptr;
    const double duration = deckDisplayDuration(deck);
    std::vector<double> timesMs;

    if (trackData) {
        const std::vector<TrackData::BeatMarker> grid = trackData->getBeatGrid();
        timesMs.reserve(grid.size());
        for (const TrackData::BeatMarker& marker : grid) {
            if (!marker.isBeat || marker.positionSec < 0.0 || marker.positionSec > duration)
                continue;
            timesMs.push_back(marker.positionSec * 1000.0);
        }

        if (!timesMs.empty())
            return timesMs;

        const double bpm = trackData->getBpm();
        if (bpm > 0.0 && duration > 0.0) {
            const double sampleRate = trackData->getSampleRate();
            const double firstBeatSec = sampleRate > 0.0
                                            ? static_cast<double>(trackData->getFirstBeatSample()) / sampleRate
                                            : 0.0;
            const double beatLengthSec = 60.0 / bpm;
            if (beatLengthSec > 0.001) {
                double firstVisibleBeat = firstBeatSec;
                while (firstVisibleBeat > 0.0)
                    firstVisibleBeat -= beatLengthSec;
                while (firstVisibleBeat < 0.0)
                    firstVisibleBeat += beatLengthSec;

                for (double sec = firstVisibleBeat; sec <= duration; sec += beatLengthSec)
                    timesMs.push_back(sec * 1000.0);
            }
        }
    }

    return timesMs;
}
int DDJFLX10Controller::currentWaveformEntry(int deck) const
{
    const QByteArray& waveform = m_waveforms[deck];
    const int entries = waveform.size() / 2;
    if (entries <= 19 || m_clockStartMs <= 0)
        return 0;

    const DjEngine* engine = deckEngine(deck);
    if (engine && engine->getDuration() > 0.0f) {
        const double duration = validTrackDuration(engine);
        const double position = validTrackPosition(engine, duration);
        const double fraction = duration > 0.0 ? std::clamp(position / duration, 0.0, 1.0) : 0.0;
        return std::clamp(static_cast<int>(fraction * entries), 0, entries - 19);
    }

    const double elapsed = (QDateTime::currentMSecsSinceEpoch() - m_clockStartMs) / 1000.0;
    const double duration = std::max(1.0, m_waveformDurations[deck]);
    const double fraction = std::fmod(std::max(0.0, elapsed), duration) / duration;
    return std::clamp(static_cast<int>(fraction * entries), 0, entries - 19);
}
