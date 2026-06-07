#include "DjEngine.h"
#include "DjMasterBus.h"
#include "audio/ReverseStreamAudioSource.h"
#include "audio/AudioDeviceUtils.h"
#include "audio/MetadataUtils.h"
#include "library/CoverArtExtractor.h"
#include "library/CoverArtProvider.h"
#include "library/LibraryCoverService.h"
#include "fx/FxProcessor.h"
#include "library/LibraryDatabase.h"
#include "library/TrackIdGenerator.h"
#include "WaveformCache.h"
#include "WaveformAnalyzer.h"
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
#include <QVariantMap>
#include <QImage>
#include <QBuffer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <expected>
#include <ranges>
#include <vector>
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>
#endif

namespace {

constexpr double kVolumeMin = 0.0;
constexpr double kVolumeMax = 1.0;
constexpr double kTrimMin = 0.0;
constexpr double kTrimMax = 2.0;
constexpr double kEqMin = -1.0;
constexpr double kEqMax = 1.0;
constexpr double kFilterMin = -1.0;
constexpr double kFilterMax = 1.0;

double playHistoryThresholdSeconds(double durationSec)
{
    if (durationSec <= 0.0)
        return 12.0;
    if (durationSec <= 45.0)
        return std::clamp(durationSec * 0.35, 5.0, 12.0);
    return std::clamp(durationSec * 0.12, 10.0, 20.0);
}

QString defaultHotCueColor(int index)
{
    static const char* kColors[] = {
        "#e04040", "#e08030", "#e0c030", "#40c040",
        "#3080e0", "#8040e0", "#e040a0", "#40c0c0",
    };
    return QString::fromUtf8(kColors[static_cast<size_t>(index) % 8]);
}

QString defaultSavedLoopColor(int index)
{
    static const char* kColors[] = {
        "#30b050", "#3080e0", "#e08030", "#8040e0",
        "#e04040", "#40c0c0", "#e0c030", "#e040a0",
    };
    return QString::fromUtf8(kColors[static_cast<size_t>(index) % 8]);
}

} // namespace

QStringList DjEngine::getAvailableAudioDeviceTypes() const
{
    QStringList types;

    auto& manager = const_cast<juce::AudioDeviceManager&>(deviceManager);
    const QString currentType = getCurrentAudioDeviceType();
    QString jackType;
    QString pipewireType;
    QString pulseType;

    for (auto* type : manager.getAvailableDeviceTypes()) {
        if (type == nullptr || type->getTypeName().isEmpty())
            continue;
        const QString name = QString::fromUtf8(type->getTypeName().toRawUTF8());
        types.push_back(name);
#if JUCE_LINUX || JUCE_BSD
        const QString lower = name.toLower();
        if (jackType.isEmpty() && lower == QStringLiteral("jack"))
            jackType = name;
        if (pipewireType.isEmpty() && lower.contains(QStringLiteral("pipewire")))
            pipewireType = name;
        if (pulseType.isEmpty() && (lower.contains(QStringLiteral("pulse")) || lower.contains(QStringLiteral("pulseaudio"))))
            pulseType = name;
#endif
    }

    const QString preferredType = !jackType.isEmpty() ? jackType
                                : !pipewireType.isEmpty() ? pipewireType
                                : !pulseType.isEmpty() ? pulseType
                                : currentType;

    const int preferredIndex = types.indexOf(preferredType);
    if (preferredIndex > 0)
        types.move(preferredIndex, 0);
    else {
        const int currentIndex = types.indexOf(currentType);
        if (currentIndex > 0)
            types.move(currentIndex, 0);
    }

    return types;
}


QStringList DjEngine::getAvailableAudioOutputDevices(const QString& deviceType) const
{
    QStringList devices;
    QStringList allDevices;

    auto& manager = const_cast<juce::AudioDeviceManager&>(deviceManager);
    auto* type = findDeviceType(manager, deviceType);
    if (type == nullptr)
        return devices;

    type->scanForDevices();
    const QString selectedType = !deviceType.isEmpty()
        ? deviceType
        : QString::fromUtf8(type->getTypeName().toRawUTF8());
    const QString currentOutput = getCurrentAudioOutputDevice();
    const QString selectedTypeLower = selectedType.toLower();
    QSet<QString> seen;

    const auto names = type->getDeviceNames(false);
    for (const auto& name : names) {
        const QString qName = QString::fromUtf8(name.toRawUTF8()).trimmed();
        if (qName.isEmpty() || seen.contains(qName))
            continue;

        seen.insert(qName);
        allDevices.push_back(qName);

        bool keep = true;
#if JUCE_LINUX || JUCE_BSD
        const QString lowerName = qName.toLower();
        
        // Exclude virtual/internal/problematic devices on Linux
        keep = !(lowerName.startsWith(QStringLiteral("hw:"))
                 || lowerName.startsWith(QStringLiteral("plughw:"))
                 || lowerName.startsWith(QStringLiteral("sysdefault:"))
                 || lowerName.startsWith(QStringLiteral("front:"))
                 || lowerName.startsWith(QStringLiteral("surround"))
                 || lowerName.startsWith(QStringLiteral("iec958:"))
                 || lowerName.startsWith(QStringLiteral("dmix:"))
                 || lowerName.startsWith(QStringLiteral("dsnoop:"))
                 || lowerName.startsWith(QStringLiteral("usbstream:"))
                 || lowerName.startsWith(QStringLiteral("jackinput"))
                 || lowerName.contains(QStringLiteral("internal"))
                 || lowerName.contains(QStringLiteral("loopback"))
                 || lowerName.startsWith(QStringLiteral("lavaplayer"))
                 || lowerName.startsWith(QStringLiteral("Combined"))
                 || lowerName == QStringLiteral("null"));

        if (!keep) {
            if (lowerName == QStringLiteral("default")
                || lowerName.contains(QStringLiteral("pipewire"))
                || lowerName.contains(QStringLiteral("pulse"))) {
                keep = true;
            }
        }
        
        // On PipeWire/ALSA, prefer human-readable names and exclude raw configs
        if (keep && (selectedTypeLower.contains(QStringLiteral("alsa")) || selectedTypeLower.contains(QStringLiteral("pipewire")) || selectedTypeLower.contains(QStringLiteral("pulse")))) {
            // Look for actual physical devices or named profiles
            keep = !lowerName.contains(QStringLiteral("@"))
                && !lowerName.startsWith(QStringLiteral("builtin_"))
                && !lowerName.contains(QStringLiteral(":CARD="))
                && !lowerName.contains(QStringLiteral(":DEV="));
        }
#endif

        if (keep)
            devices.push_back(qName);
    }

    if (devices.isEmpty())
        devices = allDevices;

    if (!currentOutput.isEmpty() && !devices.contains(currentOutput) && allDevices.contains(currentOutput))
        devices.push_front(currentOutput);

    const int currentOutputIndex = devices.indexOf(currentOutput);
    if (currentOutputIndex > 0)
        devices.move(currentOutputIndex, 0);

    return devices;
}


QStringList DjEngine::getAvailableOutputChannelPairs(const QString& deviceType,
                                                     const QString& outputDevice) const
{
    QString selectedType = deviceType;
    QString selectedOutput = outputDevice;

    if (selectedType.isEmpty())
        selectedType = getCurrentAudioDeviceType();
    if (selectedOutput.isEmpty())
        selectedOutput = getCurrentAudioOutputDevice();

    const QString loweredType = selectedType.trimmed().toLower();
    if (loweredType == QStringLiteral("jack") || loweredType.contains(QStringLiteral("jack")))
        return buildChannelPairList(kMaxSupportedOutputChannel);

    int channelCount = readCurrentDeviceOutputChannelCount(deviceManager, selectedType, selectedOutput);
    if (channelCount < 2)
        channelCount = readDeviceOutputChannelCount(selectedType, selectedOutput);

    return buildChannelPairList(channelCount);
}


QString DjEngine::getCurrentAudioDeviceType() const
{
    return QString::fromUtf8(deviceManager.getCurrentAudioDeviceType().toRawUTF8());
}


QString DjEngine::getCurrentAudioOutputDevice() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return QString::fromUtf8(device->getName().toRawUTF8());
    return QString();
}


int DjEngine::getCurrentAudioSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return static_cast<int>(std::lround(device->getCurrentSampleRate()));
    return 0;
}


int DjEngine::getCurrentAudioBufferSize() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();
    return 0;
}


bool DjEngine::isJackServerRunning() const
{
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
    QString msg;
    return probeJackServer(msg);
#else
    return false;
#endif
}


QString DjEngine::jackServerStatus() const
{
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
    QString msg;
    probeJackServer(msg);
    return msg;
#else
    return QStringLiteral("JACK backend not built in this binary.");
#endif
}


