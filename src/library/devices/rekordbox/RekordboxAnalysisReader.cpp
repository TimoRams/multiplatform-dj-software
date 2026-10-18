#include "library/devices/rekordbox/RekordboxDeviceSource.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QDateTime>
#include <QCborArray>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cmath>

namespace rekordbox {
namespace {

constexpr qsizetype kMaxAnalysisBytes = 128 * 1024 * 1024;
constexpr quint32 kMaxSections = 8192;
constexpr quint32 kMaxBeats = 2'000'000;
constexpr quint32 kMaxCues = 2048;

bool rangeFits(qsizetype offset, qsizetype length, qsizetype size)
{
    return offset >= 0 && length >= 0 && offset <= size && length <= size - offset;
}

bool readU8(const QByteArray& data, qsizetype offset, quint8* value)
{
    if (!value || !rangeFits(offset, 1, data.size()))
        return false;
    *value = static_cast<quint8>(data.at(offset));
    return true;
}

bool readBe16(const QByteArray& data, qsizetype offset, quint16* value)
{
    if (!value || !rangeFits(offset, 2, data.size()))
        return false;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    *value = static_cast<quint16>((static_cast<quint16>(p[0]) << 8) | p[1]);
    return true;
}

bool readBe32(const QByteArray& data, qsizetype offset, quint32* value)
{
    if (!value || !rangeFits(offset, 4, data.size()))
        return false;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    *value = (static_cast<quint32>(p[0]) << 24)
        | (static_cast<quint32>(p[1]) << 16)
        | (static_cast<quint32>(p[2]) << 8)
        | static_cast<quint32>(p[3]);
    return true;
}

QString rgbColor(quint8 red, quint8 green, quint8 blue)
{
    if (red == 0 && green == 0 && blue == 0)
        return {};
    return QStringLiteral("#%1%2%3")
        .arg(red, 2, 16, QLatin1Char('0'))
        .arg(green, 2, 16, QLatin1Char('0'))
        .arg(blue, 2, 16, QLatin1Char('0'))
        .toUpper();
}

QString decodeUtf16Be(const QByteArray& data, qsizetype offset, qsizetype bytes)
{
    if ((bytes & 1) != 0 || !rangeFits(offset, bytes, data.size()))
        return {};
    QString output;
    output.reserve(bytes / 2);
    for (qsizetype i = 0; i < bytes; i += 2) {
        quint16 unit = 0;
        if (!readBe16(data, offset + i, &unit) || unit == 0)
            break;
        output.append(QChar(unit));
    }
    return output;
}

bool parseBeatGrid(const QByteArray& data, qsizetype body, qsizetype sectionEnd,
                   Analysis* analysis, QString* error)
{
    quint32 count = 0;
    if (!rangeFits(body, 12, sectionEnd) || !readBe32(data, body + 8, &count)
        || count > kMaxBeats
        || static_cast<quint64>(count) * 8
               > static_cast<quint64>(sectionEnd - body - 12)) {
        *error = QStringLiteral("Invalid PQTZ beat count");
        return false;
    }
    analysis->beats.clear();
    analysis->beats.reserve(static_cast<qsizetype>(count));
    double previous = -1.0;
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype entry = body + 12 + static_cast<qsizetype>(i) * 8;
        quint16 beatNumber = 0;
        quint16 tempo = 0;
        quint32 timeMs = 0;
        if (!readBe16(data, entry, &beatNumber)
            || !readBe16(data, entry + 2, &tempo)
            || !readBe32(data, entry + 4, &timeMs)) {
            *error = QStringLiteral("Truncated PQTZ entry");
            return false;
        }
        Beat beat;
        beat.positionSec = static_cast<double>(timeMs) / 1000.0;
        beat.bpm = static_cast<double>(tempo) / 100.0;
        beat.beatInBar = std::clamp<int>(beatNumber, 1, 4);
        if (!std::isfinite(beat.positionSec) || beat.positionSec < previous
            || beat.bpm < 0.0 || beat.bpm > 1000.0) {
            *error = QStringLiteral("Invalid PQTZ beat values");
            return false;
        }
        previous = beat.positionSec;
        analysis->beats.append(beat);
    }
    return true;
}

bool parseLegacyCueList(const QByteArray& data, qsizetype body, qsizetype sectionEnd,
                        Analysis* analysis, QString* error)
{
    quint32 listType = 0;
    quint16 count = 0;
    if (!rangeFits(body, 12, sectionEnd)
        || !readBe32(data, body, &listType)
        || !readBe16(data, body + 6, &count)
        || count > kMaxCues) {
        *error = QStringLiteral("Invalid PCOB header");
        return false;
    }

    qsizetype entry = body + 12;
    for (quint16 i = 0; i < count; ++i) {
        if (!rangeFits(entry, 40, sectionEnd)
            || QByteArrayView(data.constData() + entry, 4) != QByteArrayView("PCPT", 4)) {
            *error = QStringLiteral("Invalid PCOB cue entry");
            return false;
        }
        quint32 entryLength = 0;
        quint32 hotCue = 0;
        quint8 type = 0;
        quint32 timeMs = 0;
        quint32 loopMs = 0;
        if (!readBe32(data, entry + 8, &entryLength)
            || entryLength < 40 || !rangeFits(entry, entryLength, sectionEnd)
            || !readBe32(data, entry + 12, &hotCue)
            || !readU8(data, entry + 28, &type)
            || !readBe32(data, entry + 32, &timeMs)
            || !readBe32(data, entry + 36, &loopMs)) {
            *error = QStringLiteral("Truncated PCOB cue entry");
            return false;
        }
        Cue cue;
        cue.positionSec = static_cast<double>(timeMs) / 1000.0;
        cue.hotCueIndex = hotCue > 0 && hotCue <= 16 ? static_cast<int>(hotCue - 1) : -1;
        if (type == 2) {
            cue.kind = CueKind::Loop;
            cue.loopEndSec = loopMs == 0xffffffffU
                ? -1.0 : static_cast<double>(loopMs) / 1000.0;
            if (cue.loopEndSec > cue.positionSec)
                analysis->loops.append(cue);
        } else if (listType == 1 || hotCue > 0) {
            cue.kind = CueKind::HotCue;
            analysis->hotCues.append(cue);
        } else {
            cue.kind = CueKind::MemoryCue;
            analysis->memoryCues.append(cue);
        }
        entry += entryLength;
    }
    return true;
}

bool parseExtendedCueList(const QByteArray& data, qsizetype body, qsizetype sectionEnd,
                          Analysis* analysis, QString* error)
{
    quint32 listType = 0;
    quint16 count = 0;
    if (!rangeFits(body, 8, sectionEnd)
        || !readBe32(data, body, &listType)
        || !readBe16(data, body + 4, &count)
        || count > kMaxCues) {
        *error = QStringLiteral("Invalid PCO2 header");
        return false;
    }

    qsizetype entry = body + 8;
    for (quint16 i = 0; i < count; ++i) {
        if (!rangeFits(entry, 44, sectionEnd)
            || QByteArrayView(data.constData() + entry, 4) != QByteArrayView("PCP2", 4)) {
            *error = QStringLiteral("Invalid PCO2 cue entry");
            return false;
        }
        quint32 entryLength = 0;
        quint32 hotCue = 0;
        quint8 type = 0;
        quint32 timeMs = 0;
        quint32 loopMs = 0;
        quint32 commentLength = 0;
        if (!readBe32(data, entry + 8, &entryLength)
            || entryLength < 44 || !rangeFits(entry, entryLength, sectionEnd)
            || !readBe32(data, entry + 12, &hotCue)
            || !readU8(data, entry + 16, &type)
            || !readBe32(data, entry + 20, &timeMs)
            || !readBe32(data, entry + 24, &loopMs)
            || !readBe32(data, entry + 40, &commentLength)
            || commentLength > entryLength - 44 || (commentLength & 1U) != 0) {
            *error = QStringLiteral("Truncated PCO2 cue entry");
            return false;
        }

        Cue cue;
        cue.positionSec = static_cast<double>(timeMs) / 1000.0;
        cue.hotCueIndex = hotCue > 0 && hotCue <= 16 ? static_cast<int>(hotCue - 1) : -1;
        cue.label = decodeUtf16Be(data, entry + 44, commentLength).trimmed();
        const qsizetype colorOffset = entry + 44 + commentLength;
        if (entryLength >= 48 + commentLength) {
            quint8 red = 0;
            quint8 green = 0;
            quint8 blue = 0;
            if (readU8(data, colorOffset + 1, &red)
                && readU8(data, colorOffset + 2, &green)
                && readU8(data, colorOffset + 3, &blue)) {
                cue.color = rgbColor(red, green, blue);
            }
        }

        if (type == 2) {
            cue.kind = CueKind::Loop;
            cue.loopEndSec = loopMs == 0xffffffffU
                ? -1.0 : static_cast<double>(loopMs) / 1000.0;
            if (cue.loopEndSec > cue.positionSec)
                analysis->loops.append(cue);
        } else if (listType == 1 || hotCue > 0) {
            cue.kind = CueKind::HotCue;
            analysis->hotCues.append(cue);
        } else {
            cue.kind = CueKind::MemoryCue;
            analysis->memoryCues.append(cue);
        }
        entry += entryLength;
    }
    return true;
}

QString cueIdentity(const Cue& cue)
{
    return QStringLiteral("%1:%2:%3:%4")
        .arg(static_cast<int>(cue.kind))
        .arg(cue.hotCueIndex)
        .arg(qRound64(cue.positionSec * 1000.0))
        .arg(qRound64(cue.loopEndSec * 1000.0));
}

void mergeCueList(QVector<Cue>* target, const QVector<Cue>& incoming)
{
    QHash<QString, qsizetype> known;
    known.reserve(target->size() + incoming.size());
    for (qsizetype i = 0; i < target->size(); ++i)
        known.insert(cueIdentity(target->at(i)), i);
    for (const auto& cue : incoming) {
        const QString identity = cueIdentity(cue);
        const auto found = known.constFind(identity);
        if (found == known.cend()) {
            known.insert(identity, target->size());
            target->append(cue);
        } else if (!cue.label.isEmpty() || !cue.color.isEmpty()) {
            (*target)[*found] = cue;
        }
    }
}

void mergeAnalysis(Analysis* target, const Analysis& incoming)
{
    if (target->beats.isEmpty() && !incoming.beats.isEmpty())
        target->beats = incoming.beats;
    mergeCueList(&target->hotCues, incoming.hotCues);
    mergeCueList(&target->memoryCues, incoming.memoryCues);
    mergeCueList(&target->loops, incoming.loops);
    if (incoming.waveformPreview.size() > target->waveformPreview.size())
        target->waveformPreview = incoming.waveformPreview;
}

QStringList relatedPaths(const QString& path)
{
    const QFileInfo info(path);
    const QString base = info.dir().filePath(info.completeBaseName());
    return {path, base + QStringLiteral(".EXT"), base + QStringLiteral(".2EX")};
}

QByteArray sourceFingerprint(const QString& path)
{
    QCborArray fields;
    for (const auto& candidate : relatedPaths(path)) {
        const QFileInfo info(candidate);
        fields.append(candidate);
        fields.append(info.canonicalFilePath());
        fields.append(info.exists());
        fields.append(info.size());
        fields.append(info.lastModified().toMSecsSinceEpoch());
        fields.append(info.metadataChangeTime().toMSecsSinceEpoch());
#ifdef Q_OS_UNIX
        struct stat status {};
        if (::stat(QFile::encodeName(candidate).constData(), &status) == 0) {
            fields.append(qint64(status.st_dev));
            fields.append(qint64(status.st_ino));
#ifdef Q_OS_MACOS
            fields.append(qint64(status.st_mtimespec.tv_sec));
            fields.append(qint64(status.st_mtimespec.tv_nsec));
            fields.append(qint64(status.st_ctimespec.tv_sec));
            fields.append(qint64(status.st_ctimespec.tv_nsec));
#else
            fields.append(qint64(status.st_mtim.tv_sec));
            fields.append(qint64(status.st_mtim.tv_nsec));
            fields.append(qint64(status.st_ctim.tv_sec));
            fields.append(qint64(status.st_ctim.tv_nsec));
#endif
        }
#endif
    }
    return QCborValue(fields).toCbor();
}

} // namespace

AnalysisReader::Result AnalysisReader::readReadOnly(const QString& path) const
{
    Result result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot open ANLZ read-only: %1")
                           .arg(file.errorString());
        return result;
    }
    if (file.size() < 12 || file.size() > kMaxAnalysisBytes) {
        result.error = QStringLiteral("ANLZ size is outside safe limits");
        return result;
    }
    const QByteArray data = file.read(12);
    if (data.size() != 12
        || QByteArrayView(data.constData(), 4) != QByteArrayView("PMAI", 4)) {
        result.error = QStringLiteral("Invalid ANLZ file header");
        return result;
    }

    quint32 headerLength = 0;
    quint32 declaredLength = 0;
    if (!readBe32(data, 4, &headerLength) || !readBe32(data, 8, &declaredLength)
        || headerLength < 12 || headerLength > static_cast<quint32>(file.size())
        || declaredLength < headerLength
        || declaredLength != static_cast<quint32>(file.size())) {
        result.error = QStringLiteral("Invalid ANLZ lengths");
        return result;
    }

    qsizetype section = headerLength;
    quint32 sectionCount = 0;
    while (section < declaredLength) {
        if (++sectionCount > kMaxSections || !rangeFits(section, 12, declaredLength)) {
            result.error = QStringLiteral("Invalid ANLZ section list");
            return result;
        }
        quint32 sectionHeader = 0;
        quint32 sectionLength = 0;
        if (!file.seek(section)) {
            result.error = QStringLiteral("Cannot seek ANLZ section");
            return result;
        }
        QByteArray sectionData = file.read(12);
        if (!readBe32(sectionData, 4, &sectionHeader)
            || !readBe32(sectionData, 8, &sectionLength)
            || sectionHeader < 12 || sectionLength < sectionHeader
            || !rangeFits(section, sectionLength, declaredLength)) {
            result.error = QStringLiteral("Invalid ANLZ section length");
            return result;
        }
        const QByteArray tag = sectionData.first(4);
        const bool supported = tag == "PQTZ" || tag == "PCOB" || tag == "PCO2"
            || (tag == "PWAV" && sectionLength <= 20 + 4096);
        if (!supported) {
            section += sectionLength;
            continue;
        }
        sectionData += file.read(sectionLength - 12);
        if (sectionData.size() != sectionLength) {
            result.error = QStringLiteral("Truncated ANLZ section");
            return result;
        }
        const qsizetype body = 12;
        const qsizetype sectionEnd = sectionData.size();
        QString parseError;
        bool parsed = true;
        if (tag == QByteArrayView("PQTZ", 4))
            parsed = parseBeatGrid(sectionData, body, sectionEnd, &result.analysis, &parseError);
        else if (tag == QByteArrayView("PCOB", 4))
            parsed = parseLegacyCueList(sectionData, body, sectionEnd, &result.analysis, &parseError);
        else if (tag == QByteArrayView("PCO2", 4))
            parsed = parseExtendedCueList(sectionData, body, sectionEnd, &result.analysis, &parseError);
        else if (tag == "PWAV") {
            quint32 columns = 0;
            // Documented PWAV header is 20 bytes; low five bits are height,
            // upper three are display intensity, not frequency information.
            if (sectionHeader == 20 && readBe32(sectionData, 12, &columns)
                && columns > 0 && columns <= 4096
                && rangeFits(20, columns, sectionEnd)) {
                result.analysis.waveformPreview = sectionData.mid(20, columns);
                for (char& value : result.analysis.waveformPreview)
                    value = static_cast<char>(static_cast<quint8>(value) & 0x1fU);
            }
        }
        if (!parsed) {
            result.error = parseError;
            return result;
        }
        section += sectionLength;
    }
    if (section != declaredLength) {
        result.error = QStringLiteral("ANLZ sections do not end at declared length");
        return result;
    }
    result.ok = true;
    return result;
}

AnalysisReader::Result AnalysisReader::readRelatedReadOnly(
    const QString& datPath, const QString& mountPath) const
{
    Result combined;
    const QStringList candidates = relatedPaths(datPath);
    bool found = false;
    for (const QString& candidate : candidates) {
        if (!QFileInfo::exists(candidate))
            continue;
        found = true;
        if (!mountPath.isEmpty()) {
            const QString root = QFileInfo(mountPath).canonicalFilePath();
            const QString canonical = QFileInfo(candidate).canonicalFilePath();
            if (root.isEmpty() || !canonical.startsWith(root + QLatin1Char('/'))) {
                combined.error = QStringLiteral("ANLZ sidecar escapes the device");
                continue;
            }
        }
        const Result current = readReadOnly(candidate);
        if (!current.ok) {
            combined.error = current.error;
            continue;
        }
        combined.ok = true;
        mergeAnalysis(&combined.analysis, current.analysis);
    }
    if (!found) {
        combined.error = QStringLiteral("No related ANLZ file exists");
        return combined;
    }
    return combined;
}

AnalysisReader::Result AnalysisCache::readReadOnly(
    std::shared_ptr<const DeviceIndex> index, const QString& trackId)
{
    for (auto source = m_sources.begin(); source != m_sources.end();) {
        if (source->expired() || !m_entries.contains(source.key())) {
            m_entries.remove(source.key());
            source = m_sources.erase(source);
        } else {
            ++source;
        }
    }
    if (!index)
        return {ReadResult{false, QStringLiteral("Device index unavailable")}, {}};
    const auto found = index->trackBySourceAwareId.constFind(trackId);
    if (found == index->trackBySourceAwareId.cend())
        return {ReadResult{false, QStringLiteral("Track unavailable")}, {}};
    const QString path = index->tracks.at(*found).analysisPath;
    const QString key = index->deviceId + QLatin1Char('\n') + trackId;
    const QByteArray before = sourceFingerprint(path);
    if (auto* cached = m_entries.object(key)) {
        if (cached->index.lock() == index && cached->fingerprint == before)
            return cached->result;
        m_entries.remove(key);
    }
    auto result = DeviceSource{}.readAnalysisReadOnly(*index, trackId);
    if (result.ok && before == sourceFingerprint(path)) {
        const auto& a = result.analysis;
        qint64 cost = sizeof(Entry) + 128 + before.size() + key.size() * 2
            + a.beats.size() * sizeof(Beat) + a.waveformPreview.size();
        for (const auto* cues : {&a.hotCues, &a.memoryCues, &a.loops}) {
            cost += cues->size() * sizeof(Cue);
            for (const auto& cue : *cues)
                cost += (cue.label.size() + cue.color.size()) * 2;
        }
        if (cost <= m_entries.maxCost()) {
            m_entries.insert(key, new Entry{index, before, result}, int(cost));
            m_sources.insert(key, index);
        }
    }
    return result;
}

} // namespace rekordbox
