#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// Pure helpers for the collection-health view: check names, filename parsing
// and relocation matching. No database or filesystem access, so they are unit-testable.
namespace brockdj::library {

// Canonical check names in display order.
const QStringList& collectionHealthChecks();

// Maps aliases ("dupe", "unanalysed") to a canonical check name; empty if unknown.
QString normalizeHealthCheck(const QString& name);

struct ArtistTitle {
    QString artist;
    QString title;
};

// "01 - Artist - Title (Extended Mix).mp3" -> {"Artist", "Title (Extended Mix)"}.
// Leading track numbers and underscores are cleaned up; without a " - " separator
// only the title is filled.
ArtistTitle artistTitleFromFileName(const QString& path);

// Pairs missing paths with candidate files found during a folder scan. A match
// needs the same file name (case-insensitive); when several candidates share it,
// the one in a folder with the same name as the old parent folder wins, otherwise
// the file is skipped as ambiguous. Each candidate is used at most once.
struct RelocationResult {
    QHash<QString, QString> moves;   // old path -> new path
    QStringList ambiguous;           // missing paths with several equally good candidates
    QStringList notFound;            // missing paths without any candidate
};
RelocationResult matchRelocations(const QStringList& missingPaths, const QStringList& candidatePaths);

} // namespace brockdj::library
