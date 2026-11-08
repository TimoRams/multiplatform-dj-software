#include "CollectionHealth.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace brockdj::library {

const QStringList& collectionHealthChecks()
{
    static const QStringList checks{
        QStringLiteral("missing"),
        QStringLiteral("duplicate"),
        QStringLiteral("noinfo"),
        QStringLiteral("unanalyzed"),
        QStringLiteral("lowbitrate"),
    };
    return checks;
}

QString normalizeHealthCheck(const QString& name)
{
    const QString n = name.trimmed().toLower();
    if (n == QStringLiteral("dupe") || n == QStringLiteral("duplicates"))
        return QStringLiteral("duplicate");
    if (n == QStringLiteral("unanalysed"))
        return QStringLiteral("unanalyzed");
    return collectionHealthChecks().contains(n) ? n : QString();
}

ArtistTitle artistTitleFromFileName(const QString& path)
{
    QString base = QFileInfo(path).completeBaseName();
    base.replace(u'_', u' ');
    base = base.simplified();
    // "01. ", "1 - ", "A1 - " and two-digit "01 " prefixes from rips and vinyl dumps;
    // other bare numbers stay ("808 State").
    static const QRegularExpression trackNo(QStringLiteral(
        "^(?:[A-Da-d]?\\d{1,3}\\s*[.)-]\\s*|[A-Da-d]\\d{1,2}\\s+|\\d{2}\\s+)(?=\\S)"));
    base.remove(trackNo);

    ArtistTitle result;
    const qsizetype sep = base.indexOf(QStringLiteral(" - "));
    if (sep > 0) {
        result.artist = base.left(sep).trimmed();
        result.title = base.mid(sep + 3).trimmed();
    } else {
        result.title = base.trimmed();
    }
    return result;
}

namespace {

QString parentName(const QString& path)
{
    return QFileInfo(path).absoluteDir().dirName().toLower();
}

} // namespace

RelocationResult matchRelocations(const QStringList& missingPaths, const QStringList& candidatePaths)
{
    QHash<QString, QStringList> byName;
    for (const QString& candidate : candidatePaths)
        byName[QFileInfo(candidate).fileName().toLower()].append(candidate);

    RelocationResult result;
    QSet<QString> used;
    for (const QString& missing : missingPaths) {
        QStringList options;
        for (const QString& candidate : byName.value(QFileInfo(missing).fileName().toLower())) {
            if (!used.contains(candidate))
                options.append(candidate);
        }
        if (options.isEmpty()) {
            result.notFound.append(missing);
            continue;
        }
        if (options.size() > 1) {
            const QString wantedParent = parentName(missing);
            QStringList sameParent;
            for (const QString& option : std::as_const(options)) {
                if (parentName(option) == wantedParent)
                    sameParent.append(option);
            }
            if (sameParent.size() != 1) {
                result.ambiguous.append(missing);
                continue;
            }
            options = sameParent;
        }
        used.insert(options.first());
        result.moves.insert(missing, options.first());
    }
    return result;
}

} // namespace brockdj::library
