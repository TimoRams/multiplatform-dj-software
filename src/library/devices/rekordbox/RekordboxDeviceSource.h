#pragma once

#include "library/devices/rekordbox/RekordboxTypes.h"

#include <QString>
#include <QCache>
#include <memory>

namespace rekordbox {

class PdbReader final
{
public:
    struct Result : ReadResult {
        QVector<Track> tracks;
        QVector<Playlist> playlists;
    };

    [[nodiscard]] Result readReadOnly(const QString& path) const;
};

class AnalysisReader final
{
public:
    struct Result : ReadResult {
        Analysis analysis;
    };

    [[nodiscard]] Result readReadOnly(const QString& path) const;
    [[nodiscard]] Result readRelatedReadOnly(const QString& datPath,
                                            const QString& mountPath = {}) const;
};

// Worker-owned, bounded RAM cache. Index lifetime scopes entries to one mounted
// source; file identities include all three sidecars, including absent files.
class AnalysisCache final
{
public:
    explicit AnalysisCache(int maxBytes = 16 * 1024 * 1024) : m_entries(maxBytes) {}
    [[nodiscard]] AnalysisReader::Result readReadOnly(
        std::shared_ptr<const DeviceIndex> index, const QString& trackId);
    void clear() { m_entries.clear(); m_sources.clear(); }
    [[nodiscard]] int entryCount() const { return m_entries.size(); }

private:
    struct Entry {
        std::weak_ptr<const DeviceIndex> index;
        QByteArray fingerprint;
        AnalysisReader::Result result;
    };
    QCache<QString, Entry> m_entries;
    QHash<QString, std::weak_ptr<const DeviceIndex>> m_sources;
};

class DeviceSource final
{
public:
    struct Result : ReadResult {
        DeviceIndex index;
    };

    // Prefer the documented encrypted OneLibrary schema. Legacy export.pdb
    // remains a fallback; a modern failure is retained in index.warning.
    [[nodiscard]] Result readIndexReadOnly(const QString& mountPath,
                                           const QString& deviceId) const;
    [[nodiscard]] AnalysisReader::Result readAnalysisReadOnly(
        const DeviceIndex& index, const QString& sourceAwareTrackId) const;

    [[nodiscard]] static QString resolveContainedPath(const QString& mountPath,
                                                      const QString& devicePath,
                                                      bool requireFile = true);
};

// Reads the presentation identity of a connected device — the name and
// background colour it was given in the exporting library software.
//
// This is deliberately separate from the track-library readers. A device can
// carry several library formats at once, and the identity lives in only one of
// them, so which parser BrockDJ uses to list tracks must not decide whether the
// device shows up with its proper name.
//
// Every access is read-only: the database is opened through a `mode=ro`
// immutable URI, so SQLite never creates a journal, a WAL file or a temporary
// file next to it. Nothing is ever written to the connected medium.
class DeviceIdentityReader final
{
public:
    // Returns an empty identity when the device carries no readable profile —
    // a missing database, a format this build cannot open and a device that was
    // simply never named are all indistinguishable to the caller on purpose.
    [[nodiscard]] DeviceIdentity readReadOnly(const QString& mountPath) const;

    // True when this build was compiled with the support needed to open the
    // encrypted device database. Exposed so tests can skip rather than fail on
    // a machine without the dependency.
    [[nodiscard]] static bool isSupported();

    [[nodiscard]] static DeviceColor colorFromType(int rawColorType);

    // Display colour for the palette entry, or an empty string for None and
    // Unknown so the UI keeps its standard accent.
    [[nodiscard]] static QString colorHex(DeviceColor color);
};

} // namespace rekordbox
