#include <QCoreApplication>
#include <QFile>
#include <QString>

#include <array>
#include <iostream>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

QString readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString header = readFile(QStringLiteral(SOURCE_DIR "/src/deck/DjEngine.h"));
    bool ok = require(!header.isEmpty(), "DjEngine public contract header is readable");

    // Public QML/controller contract: keep these stable unless all consumers are
    // intentionally migrated in the same change.
    for (const char* property : std::array {
             "progress", "isPlaying", "tempoPercent", "trackData", "syncEnabled",
             "loopActive", "volume", "hotCues", "savedLoops", "beatgridLocked",
             "readOnlyExternalTrack" })
        ok &= require(header.contains(QString::fromUtf8(property)), property);
    for (const char* method : std::array {
             "loadTrack", "play", "pause", "setPosition", "setTempoPercent",
             "triggerHotCue", "setSyncEnabled", "setReverse", "setSlip",
             "loadExternalTrack", "externalSourceUnavailable", "ejectExternalSource" })
        ok &= require(header.contains(QString::fromUtf8(method)), method);

    // Header-level architecture boundary: implementation dependencies belong in
    // the facade .cpp files or the owning component, not in the public QML type.
    for (const char* forbidden : std::array {
             "audio/cache/AudioPageCache.h", "MasterBusAudioEndpoint.h", "TrackData.h",
             "WaveformAnalyzer.h", "juce_audio_devices/juce_audio_devices.h" })
        ok &= require(!header.contains(QString::fromUtf8(forbidden)), forbidden);

    return ok ? 0 : 1;
}
