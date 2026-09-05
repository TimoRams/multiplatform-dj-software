#include "MetadataUtils.h"

#include <QFile>
#include <QRegularExpression>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

#include <cmath>
#include <cstring>

namespace metadata {

bool nearlyEqual(double a, double b)
{
    return std::abs(a - b) <= kParamEpsilon;
}

QString fromJuce(const juce::String& s)
{
    return QString::fromUtf8(s.toRawUTF8());
}

QString cleanup(QString text)
{
    if (text.isEmpty())
        return text;
    text.replace(QRegularExpression(QStringLiteral("[\\x00\\r\\n\\t]+")), QStringLiteral(" "));
    return text.simplified().trimmed();
}

QString normaliseKey(const QString& key)
{
    QString result;
    result.reserve(key.size());
    for (const QChar ch : key.trimmed().toLower()) {
        if (ch.isLetterOrNumber())
            result.append(ch);
    }
    return result;
}

QHash<QString, QString> buildMetadataLookup(const juce::StringPairArray& metadata)
{
    QHash<QString, QString> map;
    auto keys   = metadata.getAllKeys();
    auto values = metadata.getAllValues();
    for (int i = 0; i < metadata.size(); ++i) {
        QString val = cleanup(fromJuce(values[i]));
        if (val.isEmpty())
            continue;
        QString nk = normaliseKey(fromJuce(keys[i]));
        if (!nk.isEmpty() && !map.contains(nk))
            map.insert(nk, val);
        QString raw = cleanup(fromJuce(keys[i]));
        if (raw.contains(QLatin1Char(':'))) {
            for (const auto& part : raw.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
                QString alt = normaliseKey(part);
                if (!alt.isEmpty() && !map.contains(alt))
                    map.insert(alt, val);
            }
        }
    }
    return map;
}

QString metaValue(const QHash<QString, QString>& map, std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) {
        auto it = map.constFind(normaliseKey(QString::fromUtf8(c)));
        if (it != map.cend())
            return it.value();
    }
    return {};
}

std::optional<Id3v1Tag> readId3v1(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 128)
        return std::nullopt;
    f.seek(f.size() - 128);
    QByteArray data = f.read(128);
    if (data.size() != 128 || std::memcmp(data.constData(), "TAG", 3) != 0)
        return std::nullopt;
    Id3v1Tag t;
    t.title  = cleanup(QString::fromLatin1(data.mid(3, 30)));
    t.artist = cleanup(QString::fromLatin1(data.mid(33, 30)));
    t.album  = cleanup(QString::fromLatin1(data.mid(63, 30)));
    t.year   = cleanup(QString::fromLatin1(data.mid(93, 4)));
    return t;
}

void filenameHeuristic(const QString& baseName, QString& title, QString& artist)
{
    if (title.isEmpty())
        title = baseName;
    if (artist.isEmpty()) {
        static const QRegularExpression pat(QStringLiteral("^\\s*(.+?)\\s*[-–]\\s*(.+)\\s*$"));
        auto m = pat.match(baseName);
        if (m.hasMatch()) {
            QString a = cleanup(m.captured(1));
            QString t = cleanup(m.captured(2));
            if (!a.isEmpty())
                artist = a;
            if (!t.isEmpty())
                title = t;
        }
    }
}

double parseBpmString(const QString& raw)
{
    if (raw.isEmpty())
        return 0.0;
    QString c = raw.trimmed().replace(QLatin1Char(','), QLatin1Char('.'));
    static const QRegularExpression numPat(QStringLiteral("([0-9]+(?:\\.[0-9]+)?)"));
    auto m = numPat.match(c);
    if (m.hasMatch()) {
        bool ok = false;
        double v = m.captured(1).toDouble(&ok);
        if (ok)
            return v;
    }
    return 0.0;
}

std::optional<TagLibTags> readTagLibTags(const QString& path)
{
    TagLib::FileRef file(path.toUtf8().constData());
    if (file.isNull() || file.tag() == nullptr)
        return std::nullopt;

    const TagLib::Tag* tag = file.tag();
    TagLibTags result;
    result.title = cleanup(QString::fromStdWString(tag->title().toWString()));
    result.artist = cleanup(QString::fromStdWString(tag->artist().toWString()));
    result.album = cleanup(QString::fromStdWString(tag->album().toWString()));
    result.genre = cleanup(QString::fromStdWString(tag->genre().toWString()));
    result.comment = cleanup(QString::fromStdWString(tag->comment().toWString()));
    if (tag->year() > 0)
        result.year = QString::number(tag->year());
    if (tag->track() > 0)
        result.trackNumber = QString::number(tag->track());

    if (file.file() != nullptr) {
        const TagLib::PropertyMap properties = file.file()->properties();
        for (const char* key : {"BPM", "TBPM"}) {
            const auto it = properties.find(TagLib::String(key));
            if (it != properties.end() && !it->second.isEmpty()) {
                result.bpm = parseBpmString(
                    QString::fromStdWString(it->second.front().toWString()));
                if (result.bpm > 0.0)
                    break;
            }
        }
    }
    return result;
}

} // namespace metadata
