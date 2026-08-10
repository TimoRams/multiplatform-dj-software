#include "audio/device/AudioDeviceService.h"

#include <QCoreApplication>
#include <iostream>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

struct DeckDeviceView {
    AudioDeviceService& service;
    AudioDeviceService::ConfigurationSnapshot configuration() const {
        return service.configurationSnapshot();
    }
};
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    AudioDeviceService service;
    DeckDeviceView deckA{service};
    DeckDeviceView deckB{service};

    ok &= require(service.manager().getCurrentAudioDevice() == nullptr,
                  "constructing the service must not select a default device");
    ok &= require(mergePreferredOutputDevices(
                      { QStringLiteral("None"), QStringLiteral("default"), QStringLiteral("USB") },
                      { QStringLiteral("Default"), QStringLiteral("Remembered device") })
                      == QStringList { QStringLiteral("None"), QStringLiteral("Remembered device"),
                                       QStringLiteral("default"), QStringLiteral("USB") },
                  "saved devices remain selectable without duplicating canonical names");
    ok &= require(mergePreferredOutputDevices({}, { QStringLiteral("None"), QString() })
                      == QStringList { QStringLiteral("None") },
                  "empty and silent preferences never create duplicate choices");
    ok &= require(normalizeMasterFirstChannelForOutput(QStringLiteral("default"), -1) == 1,
                  "a selected Master device always defaults to stereo channels 1-2");
    ok &= require(normalizeMasterFirstChannelForOutput(QStringLiteral("None"), 1) == -1,
                  "an unassigned Master device remains intentionally silent");
    const auto noDevicePairs = service.availableOutputChannelPairs(
        QStringLiteral("Missing backend"), QStringLiteral("None"));
    ok &= require(noDevicePairs == QStringList { QStringLiteral("None") },
                  "an unassigned output must expose only the silent None route");
    const auto missingDevicePairs = service.availableOutputChannelPairs(
        QStringLiteral("Missing backend"), QStringLiteral("Missing device"));
    ok &= require(missingDevicePairs == QStringList { QStringLiteral("None") },
                  "an unavailable device must not expose fallback channel pairs");
    ok &= require(service.manager().getCurrentAudioDevice() == nullptr,
                  "querying device choices must not open a default device");

    ok &= require(&deckA.service.manager() == &deckB.service.manager(),
                  "all decks must observe one AudioDeviceManager");

    int changes = 0;
    QObject::connect(&service, &AudioDeviceService::configurationChanged,
                     [&changes]() { ++changes; });
    service.publishDeviceConfigurationSnapshot(48000, 256);
    ok &= require(deckA.configuration() == deckB.configuration(),
                  "all deck views must share one configuration snapshot");
    ok &= require(deckA.configuration().sampleRate == 48000
                      && deckA.configuration().bufferSize == 256,
                  "sample rate and buffer updates must be globally visible");
    service.publishDeviceConfigurationSnapshot(48000, 256);
    ok &= require(changes == 1, "unchanged configuration must not emit a signal");

    service.publishDeviceConfigurationSnapshot(1, 99999);
    ok &= require(service.configurationSnapshot().sampleRate == 44100
                      && service.configurationSnapshot().bufferSize == 4096,
                  "invalid configuration must be normalized safely");

    int routingChanges = 0;
    QObject::connect(&service, &AudioDeviceService::routingChanged,
                     [&routingChanges](int, int, int) { ++routingChanges; });
    service.setOutputFirstChannel(1);
    ok &= require(routingChanges == 0, "unchanged routing must not emit a signal");
    service.setOutputFirstChannel(3);
    ok &= require(routingChanges == 1 && service.outputRouting().masterFirstChannel == 3,
                  "routing must have one global source of truth");

    {
        DeckDeviceView temporary{service};
        ok &= require(&temporary.service == &service, "deck view must borrow the service");
    }
    service.publishDeviceConfigurationSnapshot(44100, 512);
    ok &= require(service.configurationSnapshot().sampleRate == 44100,
                  "destroying a deck view must not destroy the service");
    return ok ? 0 : 1;
}
