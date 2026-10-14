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
    const auto deckSource = readFile(QStringLiteral(SOURCE_DIR "/src/deck/DjEngine.cpp"));
    const auto transportSource = readFile(QStringLiteral(SOURCE_DIR "/src/deck/DjEngineTransport.cpp"));
    const auto librarySource = readFile(QStringLiteral(SOURCE_DIR "/src/library/LibraryAnalysisManager.cpp"));
    const auto bootstrapSource = readFile(QStringLiteral(SOURCE_DIR "/src/app/ApplicationBootstrap.cpp"));
    const auto beginReload = deckSource.indexOf(QStringLiteral("void DjEngine::beginWaveformReanalysis"));
    const auto finishReload = deckSource.indexOf(QStringLiteral("void DjEngine::finishWaveformReanalysis"));
    const auto reload = deckSource.mid(beginReload, finishReload - beginReload);
    ok &= require(beginReload >= 0 && finishReload > beginReload
                      && reload.indexOf(QStringLiteral("shutdownAndJoin()"))
                          < reload.indexOf(QStringLiteral("beginVisualTrackLoad"))
                      && reload.contains(QStringLiteral("m_trackLoader.currentGeneration()"))
                      && !reload.contains(QStringLiteral("loadTrack("))
                      && !reload.contains(QStringLiteral("m_transport->")),
                  "reanalysis stops old waveform writers and resets visuals without reloading audio");
    const auto invalidation = librarySource.indexOf(QStringLiteral("emit waveformInvalidationStarted"));
    const auto erase = librarySource.indexOf(QStringLiteral("WaveformCache::invalidateForFile"));
    const auto resume = librarySource.indexOf(QStringLiteral("emit waveformInvalidationFinished"));
    ok &= require(invalidation >= 0 && erase > invalidation && resume > erase
                      && librarySource.contains(QStringLiteral("{600, 1200}"))
                      && bootstrapSource.contains(QStringLiteral("&DjEngine::beginWaveformReanalysis"))
                      && bootstrapSource.contains(QStringLiteral("&DjEngine::finishWaveformReanalysis"))
                      && transportSource.contains(QStringLiteral("generation == safeThis->m_reanalyzedVisualGeneration"))
                      && transportSource.contains(QStringLiteral("acceptLoadedWaveform")),
                  "all decks quiesce before both cache resolutions are erased and reject stale loader publications");
    ok &= require(!fxManagerHeader.isEmpty(), "FxManager public header is readable");
    ok &= require(!fxTypesHeader.isEmpty(), "FX type contract header is readable");

    // Public QML/controller contract: keep these stable unless all consumers are
    // intentionally migrated in the same change.
    for (const char* property : std::array {
             "progress", "isPlaying", "tempoPercent", "trackData", "syncEnabled",
             "loopActive", "volume", "hotCues", "savedLoops", "beatgridLocked",
             "readOnlyExternalTrack", "externalSourceId" })
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
