#pragma once

#include <QString>
#include <QStringList>

enum class ControllerTransport
{
    Midi,
    Hid,
    VendorUsb,
};

struct ControllerProfile
{
    QString id;
    QString displayName;
    QString vendorName;
    QStringList nativeMappingFiles;
    bool supportsMidiMapping = false;
    bool supportsHidDisplay = false;
    bool requiresLinuxUnlock = false;
};

