#include "MetadataUtils.h"

#include <QFile>
#include <QRegularExpression>

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

} // namespace metadata
