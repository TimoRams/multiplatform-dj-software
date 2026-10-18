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
    const auto mixerSource = readFile(QStringLiteral(SOURCE_DIR "/src/app/MixerControl.cpp"));
    const auto lifecycleSource = readFile(QStringLiteral(SOURCE_DIR "/src/app/ApplicationLifecycle.cpp"));
    const auto displaySource = readFile(QStringLiteral(SOURCE_DIR "/src/controllers/flx10/Flx10Display.cpp"));
    const auto controllerSource = readFile(QStringLiteral(SOURCE_DIR "/src/controllers/flx10/DDJFLX10Controller.cpp"));
    const auto bridgeSource = readFile(QStringLiteral(SOURCE_DIR "/src/controllers/flx10/Flx10MidiBridge.cpp"));
    const auto fxSource = readFile(QStringLiteral(SOURCE_DIR "/src/fx/FxManager.cpp"));
    const auto mappingSource = readFile(
        QStringLiteral(SOURCE_DIR "/src/controllers/mappings/DDJ-FLX10.brockdj.xml"));
    for (const QChar deck : {QLatin1Char('A'), QLatin1Char('B'), QLatin1Char('C'), QLatin1Char('D')}) {
        const auto entry = QStringLiteral("paramId=\"deck%1_reverse\" status=\"0x9%2\" control=\"0x38\" type=\"toggle\"")
            .arg(deck).arg(deck.toLatin1() - 'A');
        ok &= require(mappingSource.contains(entry),
                      "SHIFT reverse maps the dedicated toggle note on all four decks");
        for (const int base : {0x17, 0x37}) {
            const auto colorEntry = QStringLiteral("paramId=\"deck%1_sound_color\" status=\"0xB6\" control=\"0x%2\" type=\"encoder-absolute\"")
                .arg(deck).arg(base + deck.toLatin1() - 'A', 2, 16, QLatin1Char('0'));
            ok &= require(mappingSource.contains(colorEntry, Qt::CaseInsensitive),
                          "all four Color knobs map both halves of their channel-seven CC pair");
        }
    }
    const auto reverseBegin = bridgeSource.indexOf(
        QStringLiteral("if (parseDeckButtonParam(id, QStringLiteral(\"reverse\"), directDeck))"));
    const auto reverseEnd = bridgeSource.indexOf(
        QStringLiteral("if (parseDeckButtonParam(id, QStringLiteral(\"slip_reverse\"), directDeck))"));
    const auto reverseAction = bridgeSource.mid(reverseBegin, reverseEnd - reverseBegin);
    ok &= require(reverseBegin >= 0 && reverseEnd > reverseBegin
                      && reverseAction.contains(QStringLiteral("value >= 0.5f"))
                      && reverseAction.contains(QStringLiteral("engine->setSlip(false)"))
                      && reverseAction.contains(QStringLiteral("engine->setReverse(!engine->isReverse())")),
                  "dedicated reverse toggles persistent reverse without slip on press only");
    const auto applyFxBegin = bridgeSource.indexOf(QStringLiteral("void MidiControllerManager::applyBeatFxState"));
    const auto applyFxEnd = bridgeSource.indexOf(QStringLiteral("void MidiControllerManager::pushBeatFxTiming"));
    const auto applyFx = bridgeSource.mid(applyFxBegin, applyFxEnd - applyFxBegin);
    ok &= require(applyFxBegin >= 0 && applyFxEnd > applyFxBegin
                      && applyFx.contains(QStringLiteral("QScopedValueRollback<bool> applyingState(m_applyingBeatFxState, true)"))
                      && applyFx.contains(QStringLiteral("const bool requestedActive = m_beatFxActive;"))
                      && applyFx.contains(QStringLiteral("setUnitEnabled(1, requestedActive)"))
                      && !applyFx.contains(QStringLiteral("setUnitEnabled(1, m_beatFxActive)"))
                      && bridgeSource.contains(QStringLiteral("if (!m_fxManager || m_applyingBeatFxState)"))
                      && bridgeSource.contains(QStringLiteral("const bool appliedActive = m_fxManager->enabled1();")),
                  "first Beat FX ON survives synchronous wet/routing notifications before enabled is written");
    ok &= require(fxSource.contains(QStringLiteral("cachedBpm = engine->getCurrentBpm();"))
                      && fxSource.contains(QStringLiteral("&DjEngine::currentBpmChanged"))
                      && !fxSource.contains(QStringLiteral("&DjEngine::tempoChanged"))
                      && bridgeSource.contains(QStringLiteral("m_beatFxLevelDepth = currentWet;"))
                      && bridgeSource.contains(QStringLiteral("for (const bool playingOnly : {true, false})")),
                  "FX timing follows loaded/dynamic BPM and zero depth feedback instead of stale fader-only state");
    const auto uploadBegin = displaySource.indexOf(QStringLiteral("bool DDJFLX10Controller::uploadDeck"));
    const auto uploadEnd = displaySource.indexOf(QStringLiteral("void DDJFLX10Controller::beginWaveformSweep"));
    const auto upload = displaySource.mid(uploadBegin, uploadEnd - uploadBegin);
    const auto ejectBegin = controllerSource.indexOf(QStringLiteral("m_trackEjectedConnections[deck] = connect"));
    const auto ejectEnd = controllerSource.indexOf(QStringLiteral("m_metadataConnections[deck] = connect"));
    const auto clearBegin = controllerSource.indexOf(QStringLiteral("m_dataClearedConnections[deck] = connect"));
    const auto clearEnd = controllerSource.indexOf(QStringLiteral("m_keyAnalyzedConnections[deck] = connect"));
    ok &= require(upload.contains(QStringLiteral("engine->hasTrack()"))
                      && upload.indexOf(QStringLiteral("sendXx30(deck)"))
                          < upload.indexOf(QStringLiteral("m_waveforms[deck].isEmpty()"))
                      && controllerSource.contains(QStringLiteral("if (!m_deckInitialised[deck])"))
                      && controllerSource.contains(QStringLiteral("epoch != m_waveformRequestEpoch[deck]"))
                      && controllerSource.contains(QStringLiteral("current->trackGeneration != trackGeneration")),
                  "FLX10 track initialization precedes waveform availability, retries and rejects stale worker output");
    ok &= require(ejectBegin >= 0 && ejectEnd > ejectBegin && clearBegin >= 0 && clearEnd > clearBegin
                      && controllerSource.mid(ejectBegin, ejectEnd - ejectBegin)
                             .contains(QStringLiteral("resetDeckWaveformOutput(deck)"))
                      && !controllerSource.mid(clearBegin, clearEnd - clearBegin)
                             .contains(QStringLiteral("resetDeckWaveformOutput(deck)"))
                      && controllerSource.contains(QStringLiteral("uploadDeck(deck, m_waveformUploadQualityPermille[deck] > 0)"))
                      && controllerSource.contains(QStringLiteral("!m_deckInitialised[deck] || !m_uploadActive[deck]")),
                  "eject clears the display, analysis does not, and backpressure retries initialization before waveform streaming");
    ok &= require(bootstrapSource.contains(QStringLiteral("attachSettingsManager(&settingsManager)"))
                      && bootstrapSource.indexOf(QStringLiteral("attachSettingsManager(&settingsManager)"))
                          < bootstrapSource.indexOf(QStringLiteral("new MidiControllerManager"))
                      && bootstrapSource.contains(QStringLiteral("attachParameterStore(runtime.parameterStore.get())"))
                      && mixerSource.contains(QStringLiteral("settings->getCrossfaderPosition()"))
                      && mixerSource.contains(QStringLiteral("m_settings->setCrossfaderPosition(m_cfPos)"))
                      && mixerSource.contains(QStringLiteral("m_settings->setCrossfaderCurveMode(m_cfCurveMode)"))
                      && mixerSource.contains(QStringLiteral("m_settings->setCrossfaderAssignD(m_assignD)"))
                      && lifecycleSource.contains(QStringLiteral("runtime.mixerControl->persistCrossfaderSettings()")),
                  "crossfader restore precedes MIDI startup and persistence is owned by C++, including exit");
    ok &= require(mixerSource.contains(QStringLiteral("suffix == QLatin1String(\"crossfader_assign_a\")"))
                      && mixerSource.contains(QStringLiteral("suffix == QLatin1String(\"crossfader_assign_thru\")"))
                      && mixerSource.contains(QStringLiteral("suffix == QLatin1String(\"crossfader_assign_b\")"))
                      && mixerSource.contains(QStringLiteral("if (value < 0.5f)"))
                      && mixerSource.contains(QStringLiteral("AudioEngine::setCrossfaderAssignment(static_cast<int>(domain::toIndex(*id))")),
                  "hardware assignments route each deck independently and ignore inactive switch notes");
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
    const auto inputBridgeSource = readFile(
        QStringLiteral(SOURCE_DIR "/src/controllers/flx10/Flx10InputOwnershipBridge.cpp"));
    ok &= require(header.contains(QStringLiteral("inputExternal"))
                      && transportSource.contains(QStringLiteral("if (inputExternal())\n        return;"))
                      && transportSource.contains(QStringLiteral("AudioEngine::setDeckInputSuppressed"))
                      && transportSource.contains(QStringLiteral("if (ownership == 2)\n        pause();")),
                  "external FLX10 input pauses, blocks play and silences the deck");
    const auto cuesSource = readFile(QStringLiteral(SOURCE_DIR "/src/deck/DjEngineCues.cpp"));
    ok &= require(cuesSource.contains(QStringLiteral("|| inputExternal()\n        || !m_hasTrack")),
                  "CUE hold preview cannot start audio on an externally owned deck");
    ok &= require(inputBridgeSource.contains(QStringLiteral("flx10::decodeInputSelect"))
                      && bootstrapSource.contains(QStringLiteral("setFlx10HostUsbPort")),
                  "FLX10 INPUT SELECT reports and the configured USB port reach the decks");
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
