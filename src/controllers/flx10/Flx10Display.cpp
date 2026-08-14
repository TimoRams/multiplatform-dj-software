#include "DDJFLX10Controller.h"

#include "deck/DjEngine.h"
#include "domain/TrackData.h"
#include "waveform/WaveformAggregator.h"
#include "waveform/WaveformTypes.h"

#include <QDateTime>
#include <QDebug>
#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "Flx10Protocol.h"

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

bool waveformCompareLoggingEnabled()
{
    static const bool enabled = [] {
        bool ok = false;
        const int value = qEnvironmentVariableIntValue(
            "BROCKDJ_WAVEFORM_COMPARE_LOG", &ok);
        return ok && value != 0;
    }();
    return enabled;
}

const char* chunkStateName(std::uint8_t state)
{
    switch (static_cast<WaveformChunkState>(state)) {
    case WaveformChunkState::Missing:
        return "Missing";
    case WaveformChunkState::Loading:
        return "Loading";
    case WaveformChunkState::PreviewReady:
        return "PreviewReady";
    case WaveformChunkState::FinalReady:
        return "FinalReady";
    }
    return "Unknown";
}

void logFlx10WaveformComparison(int deck,
                                std::uint32_t playheadChunkIndex,
                                std::uint8_t playheadChunkState,
                                std::uint32_t sourceLineBegin,
                                std::uint32_t sourceLineEnd,
                                std::uint32_t outputWidth,
                                std::uint32_t generatedColumns,
                                std::uint32_t columnsWithData,
                                std::uint32_t completeColumns,
                                std::uint64_t trackGeneration,
                                std::uint64_t dataGeneration)
{
    if (!waveformCompareLoggingEnabled())
        return;
    static std::array<qint64, 5> lastLogMs {0, 0, 0, 0, 0};
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (deck < 0 || deck >= static_cast<int>(lastLogMs.size()))
        return;
    if (nowMs - lastLogMs[deck] < 700)
        return;
    lastLogMs[deck] = nowMs;

    qInfo().nospace()
        << "[WaveformCompare][FLX10] deck=" << deck
        << " source=shared-aggregator"
        << " chunk=" << playheadChunkIndex
        << " state=" << chunkStateName(playheadChunkState)
        << " sourceLineRange=[" << sourceLineBegin << "," << sourceLineEnd << ")"
        << " displayWaveformWidth=" << outputWidth
        << " generatedColumns=" << generatedColumns
        << " columnsWithData=" << columnsWithData
        << " completeColumns=" << completeColumns
        << " trackGeneration=" << trackGeneration
        << " dataGeneration=" << dataGeneration;
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

void DDJFLX10Controller::decorateWaveformMarkers(
    int deck, QByteArray& waveform) const
{
    if (deck < 1 || deck > 2 || waveform.isEmpty())
        return;

    const DjEngine* engine = deckEngine(deck);
    const double duration = deckDisplayDuration(deck);
    if (!engine)
        return;

    // PWV5 contains waveform amplitude and colour, not native beatgrid
    // overlay geometry. Replacing audio columns with full-height beat spikes
    // creates fake transients and a misleading grid. Keep the audio waveform
    // intact until the real FLX10 overlay command is capture-proven.

    // The Serato HID mode does not expose a separate loop overlay command.
    // Put loop boundaries into the proven PWV5 render path: saved loops are
    // cyan, while an active loop gets distinct green IN and amber OUT lines.
    const QVariantList savedLoops = engine->savedLoops();
    for (const QVariant& value : savedLoops) {
        const QVariantMap loop = value.toMap();
        if (!loop.value(QStringLiteral("set")).toBool())
            continue;
        overlayPwv5Marker(waveform, loop.value(QStringLiteral("inSec")).toDouble(),
                          duration, 2, 31, 0, 7, 7);
        overlayPwv5Marker(waveform, loop.value(QStringLiteral("outSec")).toDouble(),
                          duration, 2, 31, 0, 7, 7);
    }
    if (engine->loopActive()) {
        overlayPwv5Marker(waveform, engine->loopInPosition(), duration,
                          3, 31, 0, 7, 1);
        overlayPwv5Marker(waveform, engine->loopOutPosition(), duration,
                          3, 31, 7, 4, 0);
    }

    const QVariantList hotCues = engine->hotCues();
    for (const QVariant& value : hotCues) {
        const QVariantMap cue = value.toMap();
        if (!cue.value(QStringLiteral("set")).toBool())
            continue;
        const double positionSec = cue.value(QStringLiteral("positionSec")).toDouble();
        QColor color(cue.value(QStringLiteral("color")).toString());
        if (!color.isValid())
            color = QColor(224, 64, 64);
        const auto to3Bit = [](int channel) {
            return std::clamp((channel + 15) / 32, 0, 7);
        };
        overlayPwv5Marker(waveform, positionSec, duration, 3, 31,
                          to3Bit(color.red()), to3Bit(color.green()),
                          to3Bit(color.blue()));
    }
}

void DDJFLX10Controller::refreshWaveformMarkersAndUpload(int deck)
{
    if (deck < 1 || deck > 2 || m_baseWaveforms[deck].isEmpty())
        return;

    m_waveforms[deck] = m_baseWaveforms[deck];
    decorateWaveformMarkers(deck, m_waveforms[deck]);
    if (!m_connected || m_shuttingDown.load(std::memory_order_acquire))
        return;

    // Never rewind the indexed transfer halfway through. The next sweep reads
    // the decorated bytes and replaces both newly added and deleted markers.
    if (m_uploadActive[deck]) {
        m_uploadResweepPending[deck] = true;
    } else {
        beginWaveformSweep(deck);
        if (!m_uploadTimer.isActive())
            m_uploadTimer.start(kUploadTickIntervalMs);
    }
}
bool DDJFLX10Controller::uploadDeck(int deck, bool startWindowSweep)
{
    if (m_waveforms[deck].isEmpty())
        return true;

    // The init sequence is what takes the screen out of its "not loaded" state,
    // so it always goes out as soon as a track exists. Only the window sweep is
    // allowed to wait for the analysis to produce something worth sending.
    bool ok = true;
    ok = sendXx30(deck) && ok;
    ok = sendXx39(deck) && ok;
    ok = uploadCoverArt(deck) && ok;
    ok = sendXx35(deck) && ok;

    if (!startWindowSweep)
        return ok;

    beginWaveformSweep(deck);
    if (!m_uploadTimer.isActive())
        m_uploadTimer.start(kUploadTickIntervalMs);
    return ok;
}

void DDJFLX10Controller::beginWaveformSweep(int deck)
{
    if (deck < 1 || deck > 2 || m_waveforms[deck].isEmpty())
        return;

    const int entries = m_waveforms[deck].size() / 2;
    const int totalWindows = (entries + kXx36EntriesPerWindow - 1)
        / kXx36EntriesPerWindow;
    m_uploadWindowsSent[deck] = 0;
    m_uploadStartWindows[deck] = totalWindows > 0
        ? (currentWaveformEntry(deck) / kXx36EntriesPerWindow) % totalWindows
        : 0;
    m_uploadActive[deck] = totalWindows > 0;
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
bool DDJFLX10Controller::sendXx35(int deck)
{
    // Verified against PioneerDDJFLX10-screen.js v1.0 (_sendInit35): one
    // all-zero packet followed by two packets carrying the CONSTANTS
    // 0x0e/0xe3 at [2]/[3]. An earlier pass here reinterpreted those two
    // bytes as a little-endian PWV5 entry count (0xe30e == 58126 == 387.5 s
    // at 150 entries/s looked like a captured track length). The working
    // implementation sends them unchanged for every track, so they are a
    // fixed header, not a length.
    const auto db = deckByte(deck);
    QByteArray first = packet();
    put8(first, 0, db);
    put8(first, 1, 0x35);
    bool ok = writePacket(first);

    for (int i = 0; i < 2; ++i) {
        QByteArray p = packet();
        put8(p, 0, db);
        put8(p, 1, 0x35);
        put8(p, 2, 0x0E);
        put8(p, 3, 0xE3);
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
    // Byte layout verified against PioneerDDJFLX10-screen.js v1.0
    // (_uploadWaveform), which is byte-for-byte checked against Serato
    // captures: [2]=0x01 constant (NOT a segment counter), [4]=0x01,
    // [6]=0x13 (19 entries), LE32 entry position at [10..13], payload at
    // [14]. An earlier pass moved the position to [8..11] on the strength of
    // a prose summary; the working implementation disagrees and wins.
    put8(p, 10, entry);
    put8(p, 11, entry >> 8);
    put8(p, 12, entry >> 16);
    put8(p, 13, entry >> 24);
    p.replace(14, take * 2, waveform.mid(entry * 2, take * 2));
    return writePacket(p);
}
bool DDJFLX10Controller::sendXx2f(int deck)
{
    // Serato sends one small cue-data terminator after xx36. Treating its
    // unknown payload as a beatgrid and expanding it to hundreds of packets
    // produced no visible markers on real hardware and needlessly loaded EP5.
    QByteArray p = packet();
    put8(p, 0, deckByte(deck));
    put8(p, 1, 0x2F);
    put8(p, 2, 0x01);
    put8(p, 4, 0x01);
    return writePacket(p);
}
bool DDJFLX10Controller::clearDeckDisplay(int deck)
{
    m_uploadActive[deck] = false;
    m_uploadWindowsSent[deck] = 0;
    m_uploadStartWindows[deck] = 0;

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
    const qint64 nowMs = m_hidTrafficClock.isValid() ? m_hidTrafficClock.elapsed() : 0;

    // Suppressing byte-identical packets indefinitely made the firmware treat
    // the state stream as stopped: while scratching the position changes every
    // tick so packets differ and flow, but on a paused or idle deck the packet
    // is identical forever and nothing was sent at all. That is exactly the
    // "display only updates while I scratch" behaviour. Dedup still bounds the
    // rate, but an unchanged state is refreshed as a heartbeat.
    if (deck >= 0 && deck < static_cast<int>(m_lastXx27Packet.size())
        && m_lastXx27Packet[deck] == p
        && deck < static_cast<int>(m_lastXx27SentMs.size())
        && nowMs - m_lastXx27SentMs[deck] < kXx27HeartbeatIntervalMs) {
        return true;
    }

    // ProgressChanged can fire much faster than the display clock while
    // scratching.  Keep the latest position, but never turn that burst into an
    // unbounded stream of USB interrupt reports.
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
    if (!engine || !engine->hasTrack())
        return {};
    if (!engine->hasCoverArt()) {
        // The deck never reported artwork for this track. Distinguish that from
        // "artwork present but unusable" so the log names the actual stage that
        // dropped the picture instead of leaving the whole path silent.
        qInfo() << "[DDJ-FLX10] Deck" << deck
                << "reports no cover art for" << engine->trackTitle();
        return {};
    }

    const QImage cover = engine->currentCoverImage();
    if (cover.isNull())
        return {};

    for (const auto [side, quality] : {std::pair{240, 86}, std::pair{240, 74}, std::pair{180, 72}, std::pair{160, 66}}) {
        const QByteArray jpeg = encodeCoverJpeg(cover, side, quality);
        if (!jpeg.isEmpty() && jpeg.size() <= kAlbumArtMaxBytes)
            return jpeg;
    }

    // Every attempt produced nothing or something too large for the xx33
    // segment stream. Say so rather than failing silently: a bail-out here is
    // indistinguishable from "this track has no artwork" in the log.
    qWarning() << "[DDJ-FLX10] Deck" << deck
               << "has cover art" << cover.size()
               << "but no encoding fit the" << kAlbumArtMaxBytes
               << "byte album-art budget";
    return {};
}
bool DDJFLX10Controller::uploadCoverArt(int deck)
{
    const QByteArray jpeg = generateCoverJpeg(deck);
    if (jpeg.isEmpty()) {
        // No artwork yet — either the file carries none, or extraction has not
        // finished. Leave the remembered URL empty so the metadata handler
        // retries once a picture becomes available.
        return true;
    }

    qInfo() << "[DDJ-FLX10] Deck" << deck << "uploading cover art bytes" << jpeg.size();
    if (!sendXx33Album(deck, jpeg))
        return false;

    if (const DjEngine* engine = deckEngine(deck))
        m_lastCoverUrls[deck] = engine->coverArtUrl();
    return true;
}
QByteArray DDJFLX10Controller::generatePreviewWaveform(
    int deck, WaveformPreviewRenderInfo* outInfo) const
{
    const DjEngine* engine = deck == 1 ? m_deckA : m_deckB;
    if (!engine || engine->getDuration() <= 0.0f)
        return {};

    TrackData* trackData = engine->getTrackData();
    if (!trackData)
        return {};

    const auto snapshot = trackData->getWaveformLineStoreSnapshot();
    if (!snapshot || snapshot->trackGeneration == 0
        || snapshot->linesPerSecond == 0
        || snapshot->totalLineCount == 0
        || snapshot->chunkSize == 0
        || !snapshot->chunks) {
        return {};
    }

    const int targetEntries = std::clamp(
        static_cast<int>(std::ceil(deckDisplayDuration(deck) * kJogWaveformEntriesPerSecond)),
        150,
        kMaxWaveformEntries);
    if (targetEntries <= 0)
        return {};

    QByteArray out;
    out.reserve(targetEntries * 2);
    std::uint32_t columnsWithData = 0;
    std::uint32_t completeColumns = 0;

    // The FLX10 is just another consumer of the shared column semantics: it
    // differs from the desktop waveform only in target resolution (a fixed
    // 150 entries per second) and in quantising the result to PWV5's 5-bit
    // height / 3-bit RGB. It no longer selects an LOD level or aggregates
    // source lines itself — that belongs to aggregateWaveformColumn() so the
    // hardware can never disagree with the screen about what the track looks
    // like.
    for (int i = 0; i < targetEntries; ++i) {
        const auto range = waveform::sourceLineRangeForColumn(
            snapshot->totalLineCount, i, targetEntries);
        const auto column = waveform::aggregateWaveformColumn(*snapshot, range);

        if (!column.hasData) {
            out += encodePwv5Entry(1, 0, 0, 0);
            continue;
        }
        ++columnsWithData;
        if (column.complete)
            ++completeColumns;
        out += encodePwv5Column(column);
    }

    if (outInfo) {
        const double duration = validTrackDuration(engine);
        const double playheadSec = validTrackPosition(engine, duration);
        const auto playheadLine = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::llround(
                playheadSec * static_cast<double>(snapshot->linesPerSecond))),
            0, static_cast<std::int64_t>(snapshot->totalLineCount - 1)));
        const auto playheadChunkIndex = snapshot->chunkSize > 0
            ? playheadLine / snapshot->chunkSize : 0u;
        const auto playheadChunk = snapshot->chunkAt(playheadChunkIndex);
        outInfo->trackGeneration = snapshot->trackGeneration;
        outInfo->dataGeneration = snapshot->dataGeneration;
        outInfo->sourceLineBegin = 0;
        outInfo->sourceLineEnd = snapshot->totalLineCount;
        outInfo->outputWidth = static_cast<std::uint32_t>(targetEntries);
        outInfo->generatedColumns = static_cast<std::uint32_t>(targetEntries);
        outInfo->columnsWithData = columnsWithData;
        outInfo->completeColumns = completeColumns;
        outInfo->playheadChunkIndex = playheadChunkIndex;
        outInfo->playheadChunkState = static_cast<std::uint8_t>(
            playheadChunk ? playheadChunk->state : WaveformChunkState::Missing);
        logFlx10WaveformComparison(deck,
                                   outInfo->playheadChunkIndex,
                                   outInfo->playheadChunkState,
                                   outInfo->sourceLineBegin,
                                   outInfo->sourceLineEnd,
                                   outInfo->outputWidth,
                                   outInfo->generatedColumns,
                                   outInfo->columnsWithData,
                                   outInfo->completeColumns,
                                   outInfo->trackGeneration,
                                   outInfo->dataGeneration);
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
    snapshot.keyByte = m_cachedDeckKeyBytes[deck];
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
        const int entry = waveformEntryForTimeline(position, duration, entries);
        return std::clamp(entry, 0, entries - 19);
    }

    const double elapsed = (QDateTime::currentMSecsSinceEpoch() - m_clockStartMs) / 1000.0;
    const double duration = std::max(1.0, m_waveformDurations[deck]);
    const double fraction = std::fmod(std::max(0.0, elapsed), duration) / duration;
    return std::clamp(static_cast<int>(fraction * entries), 0, entries - 19);
}
