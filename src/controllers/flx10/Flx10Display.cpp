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
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
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
    if (deck < 0 || deck >= static_cast<int>(m_deckStateCells.size()))
        return;
    // The dedup state now lives on the display thread, so this can only ask it
    // to stop suppressing. The flag is consumed by the next tick there.
    m_deckStateCells[static_cast<std::size_t>(deck)].forceResend.store(
        true, std::memory_order_release);
}
void DDJFLX10Controller::pushDeckJogDisplay(int deck)
{
    if (deck < 1 || deck > 2)
        return;
    if (m_shuttingDown.load(std::memory_order_acquire) || !m_connected)
        return;

    // Owner thread side: publish the state and drive the jog ring. The xx27
    // packet itself is produced by the display thread, which is why a stall
    // here no longer stops the stream.
    publishDeckState(deck);
    const DeckStateFeed feed = readDeckState(deck);
    updateJogRingWarning(deck,
                         deckDisplayPosition(deck),
                         feed.trackDurationSec,
                         feed.playing);
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

    // PWV5 carries waveform amplitude and colour, not native beatgrid overlay
    // geometry, and this HID mode exposes no separate grid command. An earlier
    // pass therefore dropped the grid entirely, because the only way to draw it
    // was to replace audio columns with full-height spikes — which invents
    // transients the track does not have. Tinting solves that: the column keeps
    // its measured height and only changes colour, so the grid sits behind the
    // audio. It is drawn first so cues and loops overwrite it, not the reverse.
    if (const TrackData* trackData = engine->getTrackData()) {
        const auto beats = trackData->getBeatGridSnapshot();
        const int entryCount = waveform.size() / 2;
        if (beats && !beats->empty() && entryCount > 0 && duration > 0.0) {
            // One entry is ~6.7 ms at the 150 entries/s the jog screen uses, so
            // a beat is a single column and a downbeat is three — wide enough
            // to pick the "1" out of the bar at a glance while scratching.
            for (const auto& beat : *beats) {
                if (!beat.isBeat)
                    continue;
                const int centre = waveformEntryForTimeline(
                    beat.positionSec, duration, entryCount);
                if (centre < 0)
                    continue;
                const int radius = beat.isDownbeat ? 1 : 0;
                const int level = beat.isDownbeat ? 7 : 4;
                const int floorHeight = beat.isDownbeat ? 8 : 5;
                for (int entry = centre - radius; entry <= centre + radius; ++entry)
                    tintPwv5Entry(waveform, entry, level, level, level, floorHeight);
            }
        }
    }

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
bool DDJFLX10Controller::sendXx27(int deck, const DeckDisplaySnapshot& snapshot,
                                  qint64 nowMs)
{
    const QByteArray p = encodeXx27Packet(deck, snapshot);

    // Suppressing byte-identical packets indefinitely made the firmware treat
    // the state stream as stopped: while scratching the position changes every
    // tick so packets differ and flow, but on a paused or idle deck the packet
    // is identical forever and nothing was sent at all. That is exactly the
    // "display only updates while I scratch" behaviour. Dedup still bounds the
    // rate, but an unchanged state is refreshed as a heartbeat.
    if (deck < 0 || deck >= static_cast<int>(m_displayLastPacket.size()))
        return false;

    if (m_displayLastPacket[deck] == p
        && nowMs - m_displayLastSentMs[deck] < kXx27HeartbeatIntervalMs) {
        return true;
    }

    // Keep the latest position, but never turn a burst into an unbounded
    // stream of USB interrupt reports.
    if (nowMs - m_displayLastSentMs[deck] < kJogStateIntervalMs)
        return true;

    const bool ok = writePacket(p);
    if (ok) {
        m_displayLastPacket[deck] = p;
        m_displayLastSentMs[deck] = nowMs;
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
DDJFLX10Controller::DeckStateFeed
DDJFLX10Controller::captureDeckStateFeed(int deck) const
{
    DeckStateFeed feed;
    if (deck < 1 || deck > 2)
        return feed;

    const DjEngine* engine = deckEngine(deck);
    feed.trackDurationSec = deckDisplayDuration(deck);
    if (!engine)
        return feed;

    feed.hasEngine = true;
    feed.bpm = std::max(0.0, engine->getCurrentBpm());
    feed.tempoPercent = engine->getTempoPercent();
    feed.playing = engine->isPlaying();
    feed.scratching = engine->isScratchVisualActive();
    feed.reverse = engine->isReverse();
    feed.latencyCompensationSec = engine->visualLatencyCompensationSeconds();
    feed.keyByte = m_cachedDeckKeyBytes[deck];
    return feed;
}

void DDJFLX10Controller::publishDeckState(int deck)
{
    if (deck < 1 || deck > 2)
        return;
    const DeckStateFeed feed = captureDeckStateFeed(deck);
    auto& cell = m_deckStateCells[static_cast<std::size_t>(deck)];
    const std::lock_guard lock(cell.mutex);
    cell.value = feed;
}

DDJFLX10Controller::DeckStateFeed DDJFLX10Controller::readDeckState(int deck) const
{
    if (deck < 0 || deck >= static_cast<int>(m_deckStateCells.size()))
        return {};
    const auto& cell = m_deckStateCells[static_cast<std::size_t>(deck)];
    const std::lock_guard lock(cell.mutex);
    return cell.value;
}

void DDJFLX10Controller::startDisplayThread()
{
    stopDisplayThread();
    m_displayThreadStopping.store(false, std::memory_order_release);
    m_displayLastPacket.fill({});
    m_displayLastSentMs.fill(-kJogStateIntervalMs);
    m_displayThread = std::thread([this] { displayThreadLoop(); });
}

void DDJFLX10Controller::stopDisplayThread() noexcept
{
    m_displayThreadStopping.store(true, std::memory_order_release);
    if (m_displayThread.joinable())
        m_displayThread.join();
}

void DDJFLX10Controller::displayThreadLoop()
{
    // A fixed wake-up grid rather than "sleep 5 ms after the work": the period
    // is what the firmware watches, and letting it drift with the cost of a
    // packet is what turns a busy moment into a visible stutter. A tick that
    // arrives after its slot is counted and the grid is re-based, so the loop
    // never tries to catch up by sending a burst.
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::milliseconds(kJogStateIntervalMs);
    const auto epoch = clock::now();
    auto next = epoch + period;

    while (!m_displayThreadStopping.load(std::memory_order_acquire)) {
        std::this_thread::sleep_until(next);
        const auto now = clock::now();
        if (now >= next + period) {
            m_displayTicksLate.fetch_add(1, std::memory_order_relaxed);
            next = now + period;
        } else {
            next += period;
        }
        if (m_displayThreadStopping.load(std::memory_order_acquire))
            break;
        if (!m_displayFeedActive.load(std::memory_order_acquire))
            continue;

        m_displayTicks.fetch_add(1, std::memory_order_relaxed);
        const auto nowMs = static_cast<qint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - epoch)
                .count());

        for (int deck = 1; deck <= 2; ++deck) {
            auto& cell = m_deckStateCells[static_cast<std::size_t>(deck)];
            if (cell.forceResend.exchange(false, std::memory_order_acq_rel))
                m_displayLastPacket[deck].clear();

            const DeckStateFeed feed = readDeckState(deck);
            DeckDisplaySnapshot snapshot;
            snapshot.trackDurationSec = feed.trackDurationSec;
            snapshot.bpm = feed.bpm;
            snapshot.tempoPercent = feed.tempoPercent;
            snapshot.playing = feed.playing;
            snapshot.scratching = feed.scratching;
            snapshot.reverse = feed.reverse;
            snapshot.keyByte = feed.keyByte;

            const std::atomic<double>* sink =
                m_deckPlayheadSinks[static_cast<std::size_t>(deck)].load(
                    std::memory_order_acquire);
            if (!feed.hasEngine || sink == nullptr) {
                // No deck attached, so there is no position to show. An earlier
                // pass free-ran a synthetic sweep here to keep an unbound screen
                // looking alive; that turned a wiring fault into a platter that
                // spun smoothly and meant nothing, which is far harder to spot
                // than a platter that stands still. Stand still.
                snapshot.sourcePositionSec = 0.0;
                snapshot.playing = false;
            } else {
                // The audio playhead, not the interpolated visual one. It is
                // the position the deck is actually rendering, it is written
                // by the audio thread every block, and reading it needs
                // nothing from the engine that could be mid-update.
                double position = sink->load(std::memory_order_acquire);
                if (!std::isfinite(position)) {
                    position = 0.0;
                } else if (feed.playing && !feed.scratching) {
                    // That cursor is one output buffer ahead of the ear. The
                    // desktop waveform subtracts the same term; a jog ring that
                    // did not would sit visibly ahead of the beat it marks.
                    position += feed.reverse ? feed.latencyCompensationSec
                                             : -feed.latencyCompensationSec;
                }
                snapshot.sourcePositionSec =
                    std::clamp(position, 0.0, snapshot.trackDurationSec);
            }

            sendXx27(deck, snapshot, nowMs);
        }
    }
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
