#include "library/LibrarySearchQuery.h"
#include "library/CollectionHealth.h"

#include <QDate>
#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace brockdj::library {
namespace {

constexpr double kBpmTolerance = 0.06;
constexpr int kLowBitrateKbps = 192;
constexpr int kLongTrackSecs = 20 * 60;
constexpr int kLoosenMinLength = 4;

// Characters ignored on both sides when matching text, so "acdc" finds
// "AC/DC" and "jay z" finds "Jay-Z".
const QString& punctuation()
{
    static const QString chars = QStringLiteral("'\u2019.,/-_!?():");
    return chars;
}

QString accentVariants(QChar c)
{
    switch (c.unicode()) {
    case 'a': return QStringLiteral("a\u00e0\u00e1\u00e2\u00e3\u00e4\u00e5\u0101");
    case 'c': return QStringLiteral("c\u00e7\u0107\u010d");
    case 'd': return QStringLiteral("d\u0111");
    case 'e': return QStringLiteral("e\u00e8\u00e9\u00ea\u00eb\u0113\u0117");
    case 'g': return QStringLiteral("g\u011f");
    case 'i': return QStringLiteral("i\u00ec\u00ed\u00ee\u00ef\u012b\u0131");
    case 'l': return QStringLiteral("l\u0142");
    case 'n': return QStringLiteral("n\u00f1\u0144");
    case 'o': return QStringLiteral("o\u00f2\u00f3\u00f4\u00f5\u00f6\u00f8\u014d");
    case 's': return QStringLiteral("s\u015b\u0161");
    case 'u': return QStringLiteral("u\u00f9\u00fa\u00fb\u00fc\u016b");
    case 'y': return QStringLiteral("y\u00fd\u00ff");
    case 'z': return QStringLiteral("z\u017a\u017c\u017e");
    default: return QString(c);
    }
}

// A GLOB pattern unit: one character class, or the single-char wildcard.
QString globUnit(QChar c)
{
    if (c == QChar(u'\0'))
        return QStringLiteral("?");
    const QString lower = accentVariants(c);
    const QString both = lower + lower.toUpper();
    QString unique;
    for (const QChar ch : both) {
        if (!unique.contains(ch))
            unique.append(ch);
    }
    if (unique.size() == 1) {
        if (c == u'*' || c == u'?' || c == u'[')
            return QStringLiteral("[") + c + QStringLiteral("]");
        return unique;
    }
    return QStringLiteral("[") + unique + QStringLiteral("]");
}

// `folded` may contain '\0' as a one-character wildcard.
QString containsGlob(const QString& folded)
{
    QString pattern = QStringLiteral("*");
    for (const QChar c : folded)
        pattern += globUnit(c);
    pattern += u'*';
    return pattern;
}

QString exactGlob(const QString& folded)
{
    QString pattern;
    for (const QChar c : folded)
        pattern += globUnit(c);
    return pattern;
}

QStringList wholeWordGlobs(const QString& folded)
{
    const QString word = exactGlob(folded);
    return {word, word + QStringLiteral(" *"), QStringLiteral("* ") + word,
            QStringLiteral("* ") + word + QStringLiteral(" *")};
}

QString strippedExpr(const QString& column)
{
    QString expr = QStringLiteral("COALESCE(%1,'')").arg(column);
    for (const QChar c : punctuation()) {
        const QString literal = c == u'\'' ? QStringLiteral("''") : QString(c);
        expr = QStringLiteral("REPLACE(%1,'%2','')").arg(expr, literal);
    }
    return expr;
}

QString unquote(QString value)
{
    if (value.size() >= 2 && value.startsWith(u'"') && value.endsWith(u'"'))
        return value.mid(1, value.size() - 2);
    if (value.startsWith(u'"'))
        return value.mid(1);
    return value;
}

QStringList tokenize(const QString& text)
{
    QStringList tokens;
    QString current;
    bool inQuotes = false;
    for (const QChar c : text) {
        if (c == u'"') {
            inQuotes = !inQuotes;
            current.append(c);
        } else if (!inQuotes && c.isSpace()) {
            if (!current.isEmpty())
                tokens.append(std::exchange(current, {}));
        } else {
            current.append(c);
        }
    }
    if (!current.isEmpty())
        tokens.append(current);
    return tokens;
}

struct NumericFilter {
    QString op; // "=", ">", ">=", "<", "<=", "range"
    double a = 0.0;
    double b = 0.0;
};

std::optional<double> parseDuration(const QString& raw)
{
    const QString v = raw.trimmed().toLower();
    bool ok = false;
    if (v.contains(u':')) {
        const QStringList parts = v.split(u':');
        double total = 0.0;
        for (const QString& part : parts) {
            const double n = part.toDouble(&ok);
            if (!ok)
                return std::nullopt;
            total = total * 60.0 + n;
        }
        return total;
    }
    if (v.endsWith(u'm')) {
        const double n = v.chopped(1).toDouble(&ok);
        return ok ? std::optional<double>(n * 60.0) : std::nullopt;
    }
    if (v.endsWith(u's')) {
        const double n = v.chopped(1).toDouble(&ok);
        return ok ? std::optional<double>(n) : std::nullopt;
    }
    const double n = v.toDouble(&ok);
    return ok ? std::optional<double>(n) : std::nullopt;
}

std::optional<double> parsePlainNumber(const QString& raw)
{
    bool ok = false;
    const double n = raw.trimmed().toDouble(&ok);
    return ok ? std::optional<double>(n) : std::nullopt;
}

template <typename Parser>
std::optional<NumericFilter> parseNumeric(const QString& value, Parser parse)
{
    for (const char* op : {">=", "<=", ">", "<", "="}) {
        const QString prefix = QString::fromLatin1(op);
        if (value.startsWith(prefix)) {
            const auto n = parse(value.mid(prefix.size()));
            if (!n)
                return std::nullopt;
            return NumericFilter{prefix, *n, 0.0};
        }
    }
    const qsizetype dash = value.indexOf(u'-', 1);
    if (dash > 0) {
        const auto a = parse(value.left(dash));
        const auto b = parse(value.mid(dash + 1));
        if (!a || !b)
            return std::nullopt;
        return NumericFilter{QStringLiteral("range"), std::min(*a, *b), std::max(*a, *b)};
    }
    const auto n = parse(value);
    if (!n)
        return std::nullopt;
    return NumericFilter{QStringLiteral("="), *n, 0.0};
}

std::optional<QString> normalizeCamelot(const QString& key)
{
    static const QRegularExpression re(QStringLiteral("^0?(1[0-2]|[1-9])\\s*([ABab])$"));
    const auto match = re.match(key.trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    return match.captured(1) + match.captured(2).toUpper();
}

QStringList withLeadingZeroForms(const QStringList& keys)
{
    QStringList out;
    for (const QString& k : keys) {
        out.append(k);
        if (k.size() == 2)
            out.append(QStringLiteral("0") + k);
    }
    return out;
}

class Compiler {
public:
    Compiler(const LibrarySearchContext& context, bool loosen)
        : m_context(context), m_loosen(loosen)
    {
        m_now = context.nowSecs > 0 ? context.nowSecs : QDateTime::currentSecsSinceEpoch();
    }

    LibrarySearchSql compile(const QString& text)
    {
        QList<QStringList> groups{{}};
        for (const QString& token : tokenize(text)) {
            if (token == QStringLiteral("|") || token == QStringLiteral("OR"))
                groups.append(QStringList{});
            else
                groups.last().append(token);
        }

        QStringList groupSql;
        for (const QStringList& group : groups) {
            QStringList terms;
            for (const QString& token : group) {
                const QString sql = term(token);
                if (!sql.isEmpty())
                    terms.append(sql);
            }
            if (!terms.isEmpty())
                groupSql.append(QStringLiteral("(%1)").arg(terms.join(QStringLiteral(" AND "))));
        }

        LibrarySearchSql result;
        if (!groupSql.isEmpty())
            result.condition = groupSql.size() == 1
                ? groupSql.first()
                : QStringLiteral("(%1)").arg(groupSql.join(QStringLiteral(" OR ")));
        result.bindings = std::move(m_bindings);
        result.canLoosen = m_canLoosen && !m_loosen;
        return result;
    }

private:
    QString bind(const QVariant& value)
    {
        const QString name = QStringLiteral(":sq%1").arg(m_next++);
        m_bindings.insert(name, value);
        return name;
    }

    QString term(QString token)
    {
        bool negate = false;
        if (token.size() > 1 && token.startsWith(u'-')) {
            negate = true;
            token = token.mid(1);
        }
        const QString sql = positiveTerm(token);
        if (sql.isEmpty() || !negate)
            return sql;
        return QStringLiteral("NOT COALESCE(%1,0)").arg(sql);
    }

    QString positiveTerm(const QString& token)
    {
        if (token.startsWith(u'"'))
            return plainWords(unquote(token));

        bool fuzzy = false;
        QString body = token;
        if (body.startsWith(u'~')) {
            fuzzy = true;
            body = body.mid(1);
        }
        const qsizetype colon = body.indexOf(u':');
        const qsizetype quote = body.indexOf(u'"');
        const QString field = (colon > 0 && (quote < 0 || colon < quote))
            ? body.left(colon).toLower()
            : (fuzzy ? body.toLower() : QString());
        const QString rawValue = colon > 0 ? body.mid(colon + 1) : QString();

        if (fuzzy) {
            if (field == QStringLiteral("deck"))
                return deckMatch();
            if (field == QStringLiteral("key") || field == QStringLiteral("harmonic"))
                return harmonicKeys(unquote(rawValue));
            if (field == QStringLiteral("bpm"))
                return closeBpm(unquote(rawValue));
            return plainWords(token);
        }
        if (field.isEmpty())
            return plainWords(token);

        bool exact = false;
        QString value = rawValue;
        if (value.startsWith(u'=')) {
            exact = true;
            value = value.mid(1);
        }
        const bool empty = value == QStringLiteral("\"\"");
        value = unquote(value);

        if (const QString column = textColumn(field); !column.isEmpty())
            return textField(column, value, exact, empty);
        if (field == QStringLiteral("tag") || field == QStringLiteral("tags"))
            return tagMatch(value, exact, empty);
        if (field == QStringLiteral("key"))
            return keyField(value, exact, empty);
        if (field == QStringLiteral("color") || field == QStringLiteral("colour")
            || field == QStringLiteral("farbe"))
            return colorField(value, empty);
        if (field == QStringLiteral("is"))
            return isCheck(value.toLower());
        if (field == QStringLiteral("health"))
            return healthCheck(value);
        if (field == QStringLiteral("added"))
            return dateField(QStringLiteral("COALESCE(t.date_added,0)"), value, false);
        if (field == QStringLiteral("lastplayed"))
            return dateField(QStringLiteral("COALESCE(t.last_played,0)"), value, true);
        if (const QString column = numericColumn(field); !column.isEmpty())
            return numericField(field, column, value);
        return plainWords(token);
    }

    static QString textColumn(const QString& field)
    {
        if (field == QStringLiteral("artist")) return QStringLiteral("t.artist");
        if (field == QStringLiteral("title")) return QStringLiteral("t.title");
        if (field == QStringLiteral("album")) return QStringLiteral("t.album");
        if (field == QStringLiteral("genre")) return QStringLiteral("t.genre");
        if (field == QStringLiteral("comment")) return QStringLiteral("t.comment");
        if (field == QStringLiteral("notes")) return QStringLiteral("t.notes");
        if (field == QStringLiteral("location") || field == QStringLiteral("path"))
            return QStringLiteral("l.file_path");
        return {};
    }

    static QString numericColumn(const QString& field)
    {
        if (field == QStringLiteral("bpm")) return QStringLiteral("COALESCE(t.bpm,0)");
        if (field == QStringLiteral("rating")) return QStringLiteral("COALESCE(t.rating,0)");
        if (field == QStringLiteral("energy")) return QStringLiteral("COALESCE(t.energy,0)");
        if (field == QStringLiteral("played") || field == QStringLiteral("plays"))
            return QStringLiteral("COALESCE(t.play_count,0)");
        if (field == QStringLiteral("bitrate")) return QStringLiteral("COALESCE(t.bitrate_kbps,0)");
        if (field == QStringLiteral("duration") || field == QStringLiteral("length"))
            return QStringLiteral("COALESCE(t.duration_sec,0)");
        return {};
    }

    // Patterns for one folded word: the word itself, plus one-letter-off
    // variants when loosening.
    QStringList wordPatterns(const QString& folded)
    {
        QStringList patterns{containsGlob(folded)};
        if (folded.size() >= kLoosenMinLength) {
            m_canLoosen = true;
            if (m_loosen) {
                for (qsizetype i = 0; i < folded.size(); ++i) {
                    QString substituted = folded;
                    substituted[i] = QChar(u'\0');
                    patterns.append(containsGlob(substituted));
                    QString deleted = folded;
                    deleted.remove(i, 1);
                    patterns.append(containsGlob(deleted));
                    if (i > 0) {
                        QString inserted = folded;
                        inserted.insert(i, QChar(u'\0'));
                        patterns.append(containsGlob(inserted));
                    }
                }
            }
        }
        patterns.removeDuplicates();
        return patterns;
    }

    QString globAny(const QString& expr, const QStringList& patterns)
    {
        QStringList parts;
        for (const QString& p : patterns)
            parts.append(QStringLiteral("%1 GLOB %2").arg(expr, bind(p)));
        return parts.size() == 1 ? parts.first()
                                 : QStringLiteral("(%1)").arg(parts.join(QStringLiteral(" OR ")));
    }

    QString plainWord(const QString& folded)
    {
        QStringList synonyms;
        if (folded == QStringLiteral("ft") || folded == QStringLiteral("feat")
            || folded == QStringLiteral("featuring"))
            synonyms = {QStringLiteral("feat"), QStringLiteral("ft"), QStringLiteral("featuring")};
        else if (folded == QStringLiteral("&") || folded == QStringLiteral("and")
                 || folded == QStringLiteral("und"))
            synonyms = {QStringLiteral("&"), QStringLiteral("and"), QStringLiteral("und")};
        const QStringList alternatives = synonyms.isEmpty() ? QStringList{folded} : synonyms;

        QStringList parts;
        for (const QString& alt : alternatives) {
            // Synonyms are short and common inside other words ("Daft"),
            // so they only match as whole words.
            const QStringList strict = synonyms.isEmpty() ? QStringList{containsGlob(alt)}
                                                          : wholeWordGlobs(alt);
            const QStringList loose = synonyms.isEmpty() ? wordPatterns(alt) : strict;
            for (const char* column : {"t.title", "t.artist", "t.album"})
                parts.append(globAny(strippedExpr(QString::fromLatin1(column)), loose));
            for (const char* column : {"t.genre", "t.comment", "t.notes"})
                parts.append(globAny(strippedExpr(QString::fromLatin1(column)), strict));
            parts.append(globAny(QStringLiteral("COALESCE(l.file_path,'')"), strict));
            parts.append(QStringLiteral(
                "EXISTS (SELECT 1 FROM TrackTags tt JOIN Tags tg ON tt.tag_id = tg.id"
                " WHERE tt.track_id = t.id AND %1)")
                .arg(globAny(strippedExpr(QStringLiteral("tg.name")), strict)));
        }
        return QStringLiteral("(%1)").arg(parts.join(QStringLiteral(" OR ")));
    }

    QString plainWords(const QString& text)
    {
        // A quoted phrase stays one unit; bare tokens are single words.
        const QString folded = foldSearchText(text).simplified();
        if (folded.isEmpty())
            return {};
        return plainWord(folded);
    }

    QString textField(const QString& column, const QString& value, bool exact, bool empty)
    {
        if (empty || (exact && value.isEmpty()))
            return QStringLiteral("TRIM(COALESCE(%1,'')) = ''").arg(column);
        if (exact)
            return QStringLiteral("LOWER(TRIM(COALESCE(%1,''))) = LOWER(%2)")
                .arg(column, bind(value.trimmed()));
        const QString folded = foldSearchText(value).simplified();
        if (folded.isEmpty())
            return {};
        const bool loosenable = column == QStringLiteral("t.title")
            || column == QStringLiteral("t.artist") || column == QStringLiteral("t.album");
        return globAny(strippedExpr(column),
                       loosenable ? wordPatterns(folded) : QStringList{containsGlob(folded)});
    }

    QString tagMatch(const QString& value, bool exact, bool empty)
    {
        const QString base = QStringLiteral(
            "EXISTS (SELECT 1 FROM TrackTags tt JOIN Tags tg ON tt.tag_id = tg.id"
            " WHERE tt.track_id = t.id%1)");
        if (empty || (exact && value.isEmpty()))
            return QStringLiteral("NOT ") + base.arg(QString());
        if (exact)
            return base.arg(QStringLiteral(" AND LOWER(TRIM(tg.name)) = LOWER(%1)")
                                .arg(bind(value.trimmed())));
        const QString folded = foldSearchText(value).simplified();
        if (folded.isEmpty())
            return {};
        return base.arg(QStringLiteral(" AND ")
                        + globAny(strippedExpr(QStringLiteral("tg.name")), {containsGlob(folded)}));
    }

    static QString keyExpr()
    {
        return QStringLiteral("UPPER(REPLACE(TRIM(COALESCE(t.key,'')),' ',''))");
    }

    QString keyIn(const QStringList& keys)
    {
        QStringList placeholders;
        for (const QString& k : withLeadingZeroForms(keys))
            placeholders.append(bind(k));
        return QStringLiteral("%1 IN (%2)").arg(keyExpr(), placeholders.join(QStringLiteral(", ")));
    }

    QString keyField(const QString& value, bool exact, bool empty)
    {
        if (empty || value.isEmpty() || value == QStringLiteral("?"))
            return QStringLiteral("TRIM(COALESCE(t.key,'')) = ''");
        if (const auto camelot = normalizeCamelot(value))
            return keyIn({*camelot});
        if (exact)
            return QStringLiteral("LOWER(TRIM(COALESCE(t.key,''))) = LOWER(%1)").arg(bind(value.trimmed()));
        return globAny(QStringLiteral("COALESCE(t.key,'')"), {containsGlob(foldSearchText(value))});
    }

    QString harmonicKeys(const QString& value)
    {
        const QStringList keys = harmonicCamelotKeys(value);
        if (keys.isEmpty())
            return QStringLiteral("0");
        return keyIn(keys);
    }

    QString bpmNear(double bpm)
    {
        QStringList ranges;
        for (const double factor : {1.0, 0.5, 2.0}) {
            const double centre = bpm * factor;
            ranges.append(QStringLiteral("(COALESCE(t.bpm,0) BETWEEN %1 AND %2)")
                              .arg(bind(centre * (1.0 - kBpmTolerance)),
                                   bind(centre * (1.0 + kBpmTolerance))));
        }
        return QStringLiteral("(%1)").arg(ranges.join(QStringLiteral(" OR ")));
    }

    QString closeBpm(const QString& value)
    {
        const auto bpm = parsePlainNumber(value);
        if (!bpm || *bpm <= 0.0)
            return QStringLiteral("0");
        return bpmNear(*bpm);
    }

    QString deckMatch()
    {
        QStringList keys;
        for (const QString& key : m_context.deckKeys)
            keys.append(harmonicCamelotKeys(key));
        keys.removeDuplicates();
        QStringList bpms;
        for (const double bpm : m_context.deckBpms) {
            if (bpm > 0.0)
                bpms.append(bpmNear(bpm));
        }
        QStringList parts;
        if (!keys.isEmpty())
            parts.append(keyIn(keys));
        if (!bpms.isEmpty())
            parts.append(QStringLiteral("(%1)").arg(bpms.join(QStringLiteral(" OR "))));
        if (parts.isEmpty())
            return {};
        return QStringLiteral("(%1)").arg(parts.join(QStringLiteral(" AND ")));
    }

    QString colorField(const QString& value, bool empty)
    {
        const QString v = value.trimmed().toLower();
        if (empty || v.isEmpty() || v == QStringLiteral("none") || v == QStringLiteral("keine"))
            return QStringLiteral("TRIM(COALESCE(t.color,'')) = ''");
        if (v == QStringLiteral("any"))
            return QStringLiteral("TRIM(COALESCE(t.color,'')) <> ''");
        static const QList<std::pair<QStringList, QString>> names{
            {{QStringLiteral("red"), QStringLiteral("rot")}, QStringLiteral("#e84040")},
            {{QStringLiteral("orange")}, QStringLiteral("#e87830")},
            {{QStringLiteral("yellow"), QStringLiteral("gelb")}, QStringLiteral("#e8c030")},
            {{QStringLiteral("green"), QStringLiteral("grün"), QStringLiteral("gruen")},
             QStringLiteral("#40c040")},
            {{QStringLiteral("blue"), QStringLiteral("blau")}, QStringLiteral("#4090e8")},
            {{QStringLiteral("purple"), QStringLiteral("lila"), QStringLiteral("violet")},
             QStringLiteral("#a040e8")},
            {{QStringLiteral("pink")}, QStringLiteral("#e040a0")},
        };
        QString hex = v;
        for (const auto& [aliases, colorHex] : names) {
            if (aliases.contains(v))
                hex = colorHex;
        }
        if (!hex.startsWith(u'#'))
            hex.prepend(u'#');
        return QStringLiteral("LOWER(TRIM(COALESCE(t.color,''))) = %1").arg(bind(hex));
    }

    QString isCheck(const QString& what)
    {
        if (what == QStringLiteral("analyzed") || what == QStringLiteral("analysed"))
            return QStringLiteral("COALESCE(t.is_analyzed,0) = 1");
        if (what == QStringLiteral("unanalyzed") || what == QStringLiteral("unanalysed"))
            return QStringLiteral("COALESCE(t.is_analyzed,0) = 0");
        if (what == QStringLiteral("played"))
            return QStringLiteral("COALESCE(t.play_count,0) > 0");
        if (what == QStringLiteral("unplayed"))
            return QStringLiteral("COALESCE(t.play_count,0) = 0");
        if (what == QStringLiteral("rated"))
            return QStringLiteral("COALESCE(t.rating,0) > 0");
        if (what == QStringLiteral("unrated"))
            return QStringLiteral("COALESCE(t.rating,0) = 0");
        if (what == QStringLiteral("cued"))
            return QStringLiteral("EXISTS (SELECT 1 FROM CuePoints cp WHERE cp.track_id = t.id)");
        if (what == QStringLiteral("long"))
            return QStringLiteral("COALESCE(t.duration_sec,0) > %1").arg(kLongTrackSecs);
        if (what == QStringLiteral("lowbitrate"))
            return QStringLiteral("(COALESCE(t.bitrate_kbps,0) > 0 AND t.bitrate_kbps < %1)")
                .arg(kLowBitrateKbps);
        if (what == QStringLiteral("noinfo"))
            return QStringLiteral(
                "(TRIM(COALESCE(t.artist,'')) = '' OR TRIM(COALESCE(t.title,'')) = ''"
                " OR LOWER(TRIM(t.artist)) IN ('unknown','unknown artist','artist','<unknown>')"
                " OR LOWER(TRIM(t.title)) IN ('untitled','unknown','<unknown>')"
                " OR LOWER(TRIM(t.title)) GLOB 'track*[0-9]')");
        if (what == QStringLiteral("duplicate") || what == QStringLiteral("dupe"))
            return QStringLiteral(
                "(TRIM(COALESCE(t.title,'')) <> '' AND"
                " (LOWER(TRIM(COALESCE(t.artist,''))), LOWER(TRIM(t.title))) IN"
                " (SELECT LOWER(TRIM(COALESCE(d.artist,''))), LOWER(TRIM(d.title)) FROM Tracks d"
                "  WHERE TRIM(COALESCE(d.title,'')) <> '' GROUP BY 1, 2 HAVING COUNT(*) > 1))");
        if (what == QStringLiteral("missing"))
            return QStringLiteral(
                "EXISTS (SELECT 1 FROM MissingFiles mf WHERE mf.file_path = l.file_path)");
        return QStringLiteral("0");
    }

    // health:<check> is is:<check> minus tracks the user marked as fine for that check.
    QString healthCheck(const QString& value)
    {
        const QString check = normalizeHealthCheck(value);
        if (check.isEmpty())
            return QStringLiteral("0");
        return QStringLiteral(
                   "(%1 AND NOT EXISTS (SELECT 1 FROM HealthIgnores hi"
                   " WHERE hi.track_id = t.id AND hi.check_name = %2))")
            .arg(isCheck(check), bind(check));
    }

    QString numericField(const QString& field, const QString& column, const QString& value)
    {
        const bool duration = field == QStringLiteral("duration") || field == QStringLiteral("length");
        const auto filter = duration ? parseNumeric(value, parseDuration)
                                     : parseNumeric(value, parsePlainNumber);
        if (!filter)
            return QStringLiteral("0");
        if (filter->op == QStringLiteral("range"))
            return QStringLiteral("(%1 BETWEEN %2 AND %3)").arg(column, bind(filter->a), bind(filter->b));
        if (filter->op != QStringLiteral("="))
            return QStringLiteral("%1 %2 %3").arg(column, filter->op, bind(filter->a));
        if (field == QStringLiteral("bpm")) {
            // bpm:128 means the 128 family (128.00 … 128.99), as DJs read tempos.
            const double base = std::floor(filter->a);
            if (base == filter->a)
                return QStringLiteral("(%1 >= %2 AND %1 < %3)").arg(column, bind(base), bind(base + 1.0));
            return QStringLiteral("ABS(%1 - %2) < 0.05").arg(column, bind(filter->a));
        }
        return QStringLiteral("%1 = %2").arg(column, bind(filter->a));
    }

    // Relative ages ("<7d" = less than 7 days ago) or calendar dates.
    QString dateField(const QString& column, const QString& value, bool requireSet)
    {
        static const QRegularExpression ageRe(QStringLiteral("^(\\d+(?:\\.\\d+)?)\\s*([dwmy])$"));
        QString op;
        QString rest = value.trimmed().toLower();
        for (const char* candidate : {">=", "<=", ">", "<", "="}) {
            if (rest.startsWith(QLatin1String(candidate))) {
                op = QString::fromLatin1(candidate);
                rest = rest.mid(op.size()).trimmed();
                break;
            }
        }
        const QString guard = requireSet ? QStringLiteral("%1 > 0 AND ").arg(column) : QString();

        const auto cutoffFor = [this](const QRegularExpressionMatch& age) {
            const double n = age.captured(1).toDouble();
            const QChar unit = age.captured(2).at(0);
            const double days = unit == u'd' ? n : unit == u'w' ? n * 7.0 : unit == u'm' ? n * 30.0 : n * 365.0;
            return m_now - static_cast<qint64>(days * 86400.0);
        };
        if (op.isEmpty()) {
            const QStringList range = rest.split(u'-');
            if (range.size() == 2) {
                const auto from = ageRe.match(range[0].trimmed());
                const auto to = ageRe.match(range[1].trimmed());
                if (from.hasMatch() && to.hasMatch()) {
                    const qint64 a = cutoffFor(from);
                    const qint64 b = cutoffFor(to);
                    return QStringLiteral("(%1%2 BETWEEN %3 AND %4)")
                        .arg(guard, column, bind(std::min(a, b)), bind(std::max(a, b)));
                }
            }
        }
        if (const auto age = ageRe.match(rest); age.hasMatch()) {
            const qint64 cutoff = cutoffFor(age);
            // A younger age is a later timestamp, so the comparison flips.
            QString sqlOp = QStringLiteral(">");
            if (op == QStringLiteral(">")) sqlOp = QStringLiteral("<");
            else if (op == QStringLiteral(">=")) sqlOp = QStringLiteral("<=");
            else if (op == QStringLiteral("<=")) sqlOp = QStringLiteral(">=");
            return QStringLiteral("(%1%2 %3 %4)").arg(guard, column, sqlOp, bind(cutoff));
        }

        const QDate date = QDate::fromString(rest, Qt::ISODate);
        if (!date.isValid())
            return QStringLiteral("0");
        const auto timeZone = QTimeZone::systemTimeZone();
        const qint64 dayStart = QDateTime(date, QTime(0, 0), timeZone).toSecsSinceEpoch();
        const qint64 dayEnd = QDateTime(date.addDays(1), QTime(0, 0), timeZone).toSecsSinceEpoch();
        if (op.isEmpty() || op == QStringLiteral("="))
            return QStringLiteral("(%1%2 >= %3 AND %2 < %4)").arg(guard, column, bind(dayStart), bind(dayEnd));
        if (op == QStringLiteral(">"))
            return QStringLiteral("(%1%2 >= %3)").arg(guard, column, bind(dayEnd));
        if (op == QStringLiteral(">="))
            return QStringLiteral("(%1%2 >= %3)").arg(guard, column, bind(dayStart));
        if (op == QStringLiteral("<"))
            return QStringLiteral("(%1%2 < %3)").arg(guard, column, bind(dayStart));
        return QStringLiteral("(%1%2 < %3)").arg(guard, column, bind(dayEnd));
    }

    const LibrarySearchContext& m_context;
    bool m_loosen = false;
    bool m_canLoosen = false;
    qint64 m_now = 0;
    int m_next = 0;
    QVariantMap m_bindings;
};

} // namespace

QString foldSearchText(const QString& text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing || punctuation().contains(c))
            continue;
        out.append(c.toLower());
    }
    return out;
}

QStringList harmonicCamelotKeys(const QString& key)
{
    const auto camelot = normalizeCamelot(key);
    if (!camelot)
        return {};
    const int n = camelot->chopped(1).toInt();
    const QChar letter = camelot->back();
    const auto wrap = [](int v) { return ((v - 1 + 12) % 12) + 1; };
    return {*camelot,
            QString::number(wrap(n - 1)) + letter,
            QString::number(wrap(n + 1)) + letter,
            QString::number(n) + QChar(letter == u'A' ? u'B' : u'A')};
}

LibrarySearchSql compileLibrarySearch(const QString& text,
                                      const LibrarySearchContext& context,
                                      bool loosen)
{
    return Compiler(context, loosen).compile(text);
}

} // namespace brockdj::library
