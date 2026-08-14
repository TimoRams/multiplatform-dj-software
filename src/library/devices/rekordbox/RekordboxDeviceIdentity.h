#pragma once

#include "library/devices/rekordbox/RekordboxTypes.h"

#include <QString>

namespace rekordbox {

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
