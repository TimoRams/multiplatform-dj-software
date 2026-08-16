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
    const QString fxManagerHeader = readFile(
        QStringLiteral(SOURCE_DIR "/src/fx/FxManager.h"));
    const QString fxTypesHeader = readFile(
        QStringLiteral(SOURCE_DIR "/src/fx/FxTypes.h"));
    bool ok = require(!header.isEmpty(), "DjEngine public contract header is readable");
    ok &= require(!fxManagerHeader.isEmpty(), "FxManager public header is readable");
    ok &= require(!fxTypesHeader.isEmpty(), "FX type contract header is readable");

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
             "WaveformAnalyzer.h", "juce_audio_devices/juce_audio_devices.h",
             "fx/FxProcessor.h" })
        ok &= require(!header.contains(QString::fromUtf8(forbidden)), forbidden);

    ok &= require(header.contains(QStringLiteral("#include \"fx/FxTypes.h\"")),
                  "DjEngine exposes the lightweight FX command types");
    ok &= require(fxManagerHeader.contains(QStringLiteral("#include \"fx/FxTypes.h\""))
                      && fxManagerHeader.contains(QStringLiteral("class DjEngine;")),
                  "FxManager uses the lightweight FX contract and a deck forward declaration");
    ok &= require(!fxManagerHeader.contains(QStringLiteral("FxProcessor.h"))
                      && !fxManagerHeader.contains(QStringLiteral("deck/DjEngine.h")),
                  "FxManager does not pull DSP or the complete deck facade into consumers");
    ok &= require(!fxTypesHeader.contains(QStringLiteral("juce_"))
                      && !fxTypesHeader.contains(QStringLiteral("FxProcessor.h"))
                      && !fxTypesHeader.contains(QStringLiteral("audio/")),
                  "FX command types remain processor- and audio-independent");

    return ok ? 0 : 1;
}
