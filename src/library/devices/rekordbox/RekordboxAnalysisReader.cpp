#include "library/devices/rekordbox/RekordboxAnalysisReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

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
    const QByteArray data = file.readAll();
    if (data.size() != file.size()
        || QByteArrayView(data.constData(), 4) != QByteArrayView("PMAI", 4)) {
        result.error = QStringLiteral("Invalid ANLZ file header");
        return result;
    }

    quint32 headerLength = 0;
    quint32 declaredLength = 0;
    if (!readBe32(data, 4, &headerLength) || !readBe32(data, 8, &declaredLength)
        || headerLength < 12 || headerLength > static_cast<quint32>(data.size())
        || declaredLength < headerLength
        || declaredLength != static_cast<quint32>(data.size())) {
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
        if (!readBe32(data, section + 4, &sectionHeader)
            || !readBe32(data, section + 8, &sectionLength)
            || sectionHeader < 12 || sectionLength < sectionHeader
            || !rangeFits(section, sectionLength, declaredLength)) {
            result.error = QStringLiteral("Invalid ANLZ section length");
            return result;
        }
        const QByteArrayView tag(data.constData() + section, 4);
        const qsizetype body = section + 12;
        const qsizetype sectionEnd = section + sectionLength;
        QString parseError;
        bool parsed = true;
        if (tag == QByteArrayView("PQTZ", 4))
            parsed = parseBeatGrid(data, body, sectionEnd, &result.analysis, &parseError);
        else if (tag == QByteArrayView("PCOB", 4))
            parsed = parseLegacyCueList(data, body, sectionEnd, &result.analysis, &parseError);
        else if (tag == QByteArrayView("PCO2", 4))
            parsed = parseExtendedCueList(data, body, sectionEnd, &result.analysis, &parseError);
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

AnalysisReader::Result AnalysisReader::readRelatedReadOnly(const QString& datPath) const
{
    Result combined;
    const QFileInfo datInfo(datPath);
    const QString base = datInfo.dir().filePath(datInfo.completeBaseName());
    const QStringList candidates{datPath, base + QStringLiteral(".EXT"),
                                 base + QStringLiteral(".2EX")};
    bool found = false;
    for (const QString& candidate : candidates) {
        if (!QFileInfo::exists(candidate))
            continue;
        found = true;
        const Result current = readReadOnly(candidate);
        if (!current.ok) {
            combined.error = current.error;
            return combined;
        }
        mergeAnalysis(&combined.analysis, current.analysis);
    }
    if (!found) {
        combined.error = QStringLiteral("No related ANLZ file exists");
        return combined;
    }
    combined.ok = true;
    return combined;
}

} // namespace rekordbox
