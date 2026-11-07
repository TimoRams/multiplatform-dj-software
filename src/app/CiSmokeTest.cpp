#include "deck/DjEngine.h"
#include "deck/sync/DeckSync.h"
#include "audio/AudioEngine.h"
#include "audio/SamplerController.h"
#include "audio/RenderModeRouter.h"
#include "audio/device/AudioDeviceService.h"
#include "audio/cache/AudioPageCache.h"
#include "ApplicationBootstrap.h"
#include "controllers/flx10/Flx10ControllerIdentity.h"
#include "fx/FxManager.h"
#include "app/ControlClock.h"
#include "app/SettingsManager.h"
#include "library/LibraryDatabase.h"
#include <QThread>
#include <QElapsedTimer>
#include "controllers/midi/MidiControllerManager.h"
#include "controllers/midi/ParameterStore.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QtGlobal>
#include <cmath>

namespace {
int fail(const QString& message)
{
    qCritical().noquote() << "[ci-smoke]" << message;
    return 1;
}

bool testLiveDeckFeatures(QString& error)
{
    QTemporaryDir directory;
    auto& settings = SettingsManager::getInstance();
    juce::PropertiesFile::Options options;
    options.applicationName = "live-deck-test"; options.filenameSuffix = ".xml";
    options.folderName = directory.path().toStdString(); options.osxLibrarySubFolder = "Application Support";
    settings.getAppProperties().setStorageParameters(options);
    settings.setLoadLock(true); settings.setAutoCueEnabled(false);
    SetRecorder fixture;
    fixture.configure(48000);
    if (!fixture.startRecording(directory.path())) { error = fixture.error(); return false; }
    juce::AudioBuffer<float> tone(2, 512);
    for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(tone.getWritePointer(ch), 0.1f, 512);
    for (int i = 0; i < 192; ++i) fixture.capture(tone, 512);
    fixture.stopRecording();
    if (!fixture.error().isEmpty()) { error = fixture.error(); return false; }
    LibraryDatabase database;
    if (!database.open()) { error = "live feature database failed to open"; return false; }
    AudioDeviceService devices;
    AudioPageCache cache(32 * 1024 * 1024);
    AudioEngine bus(cache);
    ControlClock clock;
    engine::sync::SyncCoordinator sync;
    bus.prepareToPlay(512, 48000);
    bool ok = true;
    const auto require = [&](bool condition, const char* message) {
        if (!condition) { error = QString::fromLatin1(message); ok = false; }
        return condition;
    };
    const auto wait = [&](auto predicate) {
        QElapsedTimer timer; timer.start();
        while (!predicate() && timer.elapsed() < 8000) { QCoreApplication::processEvents(); QThread::msleep(1); }
        return predicate();
    };
    {
        DjEngine deck(devices, cache, bus.deck(0), clock, sync, 0);
        deck.setLibraryDatabase(&database);
        deck.loadTrack(fixture.filePath());
        if (!require(wait([&] { return deck.hasTrack(); }), "local deck fixture did not load")) return false;
        const QString samplerDirectory = directory.filePath("Sampler");
        const QString sourceSample = directory.filePath("sample-source.wav");
        require(QFile::copy(fixture.filePath(), sourceSample), "sampler source fixture copy failed");
        {
            SamplerController sampler(bus.sampler(), clock, samplerDirectory);
            sampler.attachDecks({&deck, nullptr, nullptr, nullptr}, &sync, nullptr);
            const int oldMaster = AudioEngine::masterFirstChannel(), oldBooth = AudioEngine::boothFirstChannel(), oldHeadphones = AudioEngine::headphonesFirstChannel();
            AudioEngine::setOutputRouting(1,3,2); sampler.setCue(true);
            require(!sampler.cue() && sampler.error().contains("headphone"), "sampler cue accepts an overlapping output assignment");
            AudioEngine::setOutputRouting(1,3,5); sampler.setCue(true);
            require(sampler.cue(), "sampler cue rejects a valid separate headphone pair");
            sampler.setCue(false); AudioEngine::setOutputRouting(oldMaster,oldBooth,oldHeadphones);
            sampler.probeFile(sourceSample);
            require(wait([&] { return !sampler.fileInfo().value("loading").toBool(); })
                && sampler.fileInfo().value("waveform").toList().size() == 192,
                "asynchronous source overview is unavailable before sampler import");
            sampler.loadFile(0, sourceSample, 0, 1, 120, true, true, "Loop fixture", "#40a0e0", 0.5);
            require(wait([&] { return sampler.slotList().first().toMap().value("loaded").toBool(); }), "sampler import failed");
            auto entry = sampler.slotList().first().toMap();
            require(entry.value("name") == "Loop fixture" && entry.value("loop").toBool()
                && entry.value("sync").toBool(), "sampler import lost mode/name/sync metadata");
            sampler.loadFile(0, directory.filePath("invalid.wav"));
            require(wait([&] { return !sampler.slotList().first().toMap().value("state").toString().contains("LOADING"); }), "failed sampler import did not finish");
            require(sampler.slotList().first().toMap().value("loaded").toBool(), "failed import destroyed prior playable sample");
            sampler.edit(0, "Edited loop", "#80b040", 0.7, true, true, 120, 0.1, 0.9);
            require(wait([&] { return sampler.slotList().first().toMap().value("loaded").toBool(); }), "sampler trim edit failed");
            {
                const auto initial = sampler.beginEditor(3);
                const auto session = initial.value("session").toULongLong();
                require(session != 0, "editor session did not open");
                sampler.setEditorFile(session, sourceSample);
                require(wait([&] { return !sampler.editorBusy(); }) && sampler.editorError().isEmpty(), "editor source probe failed");
                QVariantMap draft {{"name", "Editor loop"}, {"color", "#40a0e0"}, {"gain", 1.0},
                    {"loop", true}, {"sync", true}, {"bpm", 120.0}, {"start", 0.1}, {"end", 0.8}};
                AudioEngine::setOutputRouting(1,3,5);
                sampler.previewEditor(session, draft);
                require(wait([&] { return !sampler.previewLoading(); }) && sampler.previewPlaying(), "editor headphone preview did not prepare");
                juce::AudioBuffer<float> previewOutput(6,512);
                for (int i = 0; i < 4; ++i) bus.getNextAudioBlock({&previewOutput,0,512});
                require(previewOutput.getMagnitude(4,0,512) > 0.001f
                    && previewOutput.getMagnitude(0,0,512) == 0 && previewOutput.getMagnitude(2,0,512) == 0,
                    "editor preview leaks into master/booth or fails to reach headphones");
                sampler.stopEditorPreview();
                for (int i = 0; i < 4; ++i) bus.getNextAudioBlock({&previewOutput,0,512});
                require(!sampler.previewPlaying(), "editor preview did not stop");
                AudioEngine::setOutputRouting(oldMaster,oldBooth,oldHeadphones);
                bool saved = false;
                const auto completion = QObject::connect(&sampler, &SamplerController::editorFinished, &sampler,
                    [&](qulonglong token, bool success, const QString&) { if (token == session) saved = success; });
                sampler.applyEditor(session,draft); sampler.setBank(2);
                require(wait([&] { return !sampler.editorBusy(); }) && saved, "editor save did not complete");
                QObject::disconnect(completion);
                require(!sampler.slotList()[3].toMap().value("loaded").toBool(), "bank change redirected editor save");
                sampler.setBank(0);
                require(sampler.slotList()[3].toMap().value("name") == "Editor loop", "editor did not save its original absolute slot");
                const auto cancelled = sampler.beginEditor(3).value("session").toULongLong();
                draft["name"] = "Discarded edit"; draft["start"] = 0.01; draft["end"] = 0.5;
                sampler.applyEditor(cancelled,draft); sampler.cancelEditor(cancelled);
                require(sampler.slotList()[3].toMap().value("name") == "Editor loop", "cancelling a save replaced the old sample");
                const auto capture = sampler.beginEditor(4).value("session").toULongLong();
                deck.setManualBpm(120); sampler.captureEditor(capture,0,1);
                require(wait([&] { return !sampler.editorBusy(); }) && sampler.editorError().isEmpty(), "editor capture preparation failed");
                sampler.cancelEditor(capture);
                require(!sampler.slotList()[4].toMap().value("loaded").toBool(), "cancelled capture modified a sampler slot");
            }
            {
                SamplerAudioEngine failedAudio;
                const auto failedDirectory = directory.filePath("failed-editor");
                SamplerController failed(failedAudio,clock,failedDirectory);
                failed.loadFile(0,sourceSample,0,0.5,120,false,false,"Keep this sample");
                require(wait([&] { return failed.slotList()[0].toMap().value("loaded").toBool(); }), "failed-write fixture did not load");
                require(QFile::remove(failedDirectory + "/banks.json") && QDir().mkpath(failedDirectory + "/banks.json"), "cannot create failed-write fixture");
                const auto session = failed.beginEditor(0).value("session").toULongLong();
                failed.applyEditor(session, {{"name","Do not commit"},{"color","#40a0e0"},{"gain",1.0},
                    {"loop",false},{"sync",false},{"bpm",120.0},{"start",0.0},{"end",0.5}});
                require(wait([&] { return !failed.editorBusy(); }) && !failed.editorError().isEmpty()
                    && failed.slotList()[0].toMap().value("name") == "Keep this sample"
                    && failed.slotList()[0].toMap().value("loaded").toBool(), "failed editor save destroyed previous sample/draft");
                failed.clearEditor(session);
                require(failed.slotList()[0].toMap().value("loaded").toBool(), "failed clear removed a playable sample");
                failed.cancelEditor(session);
            }
            sampler.setBank(1); sampler.loadFile(2, sourceSample, 0, 0.5);
            require(wait([&] { return sampler.slotList()[2].toMap().value("loaded").toBool(); }), "second sampler bank did not load");
            sampler.setBank(0); sampler.setGain(0.63); sampler.setQuantize(4);
            sampler.setSequencerStep(0,0,true);
            sampler.setSequencerStep(0,4,true);
            sampler.setSequencerSwing(0.23);
            require(QFile::remove(sourceSample), "cannot remove original sample fixture");
        }
        {
            SamplerController restored(bus.sampler(), clock, samplerDirectory);
            require(wait([&] { return restored.slotList().first().toMap().value("loaded").toBool(); }), "sampler restart failed without original file");
            const auto entry = restored.slotList().first().toMap();
            require(entry.value("name") == "Edited loop" && std::abs(entry.value("start").toDouble() - 0.1) < 0.001
                && std::abs(entry.value("end").toDouble() - 0.9) < 0.001 && restored.quantize() == 4
                && std::abs(restored.gain() - 0.63) < 0.001
                && restored.sequencerPattern().first().toInt() == 17
                && std::abs(restored.sequencerSwing() - 0.23) < 0.001,
                "sampler metadata/range/settings/sequencer pattern did not persist");
            require(wait([&] { return restored.slotList()[3].toMap().value("loaded").toBool(); })
                && restored.slotList()[3].toMap().value("name") == "Editor loop", "late cancelled editor result overwrote the persisted slot");
            restored.setBank(1);
            require(wait([&] { return restored.slotList()[2].toMap().value("loaded").toBool(); }), "sampler bank identity did not persist");
            restored.clear(2);
            require(!restored.slotList()[2].toMap().value("loaded").toBool(), "cleared sampler slot remains playable");
            restored.attachDecks({&deck, nullptr, nullptr, nullptr}, &sync, nullptr);
            deck.setManualBpm(120);
            restored.captureDeck(1, 0, 1);
            require(wait([&] { return restored.slotList()[1].toMap().value("loaded").toBool(); }), "capture from deck beat range failed");
            require(restored.slotList()[1].toMap().value("loop").toBool(), "captured beats did not become a loop");
            ParameterStore parameters;
            MidiControllerManager midi(&parameters, clock, nullptr, MidiControllerManager::HardwareAccess::Disabled);
            midi.connectSampler(&restored);
            midi.selectMapping("Built-in: DDJ-FLX10");
            QObject::connect(&parameters, &ParameterStore::parameterChanged, &restored, [&](const QString& id, float value) {
                if (id == "sampler_gain") restored.setMidiGainNormalized(value);
                if (id == "sampler_cue" && value >= 0.5f) restored.setCue(!restored.cue());
            });
            auto sendMidi = [&](const juce::MidiMessage& message) {
                static_cast<juce::MidiInputCallback&>(midi).handleIncomingMidiMessage(nullptr,message);
                QCoreApplication::processEvents();
            };
            const int midiMaster = AudioEngine::masterFirstChannel(), midiBooth = AudioEngine::boothFirstChannel(), midiHeadphones = AudioEngine::headphonesFirstChannel();
            AudioEngine::setOutputRouting(1,3,5); restored.setCue(false);
            sendMidi(juce::MidiMessage::noteOn(7,0x69,static_cast<juce::uint8>(127)));
            require(restored.cue(), "FLX10 sampler CUE press is unmapped");
            sendMidi(juce::MidiMessage::noteOn(7,0x69,static_cast<juce::uint8>(127)));
            require(restored.cue(), "repeated sampler CUE press toggled twice");
            sendMidi(juce::MidiMessage::noteOff(7,0x69)); require(restored.cue(), "sampler CUE release toggled state");
            restored.setCue(false); // Touch and MIDI share the actual state.
            sendMidi(juce::MidiMessage::noteOn(7,0x69,static_cast<juce::uint8>(127)));
            require(restored.cue(), "sampler CUE did not follow a touch change");
            sendMidi(juce::MidiMessage::noteOff(7,0x69));
            restored.setGain(0.75); restored.resetMidiPickup();
            sendMidi(juce::MidiMessage::controllerEvent(7,0x23,127));
            sendMidi(juce::MidiMessage::controllerEvent(7,0x03,127));
            require(std::abs(restored.gain()-0.75) < 0.001 && restored.midiPickupPending(), "FLX10 startup volume jumped before pickup");
            sendMidi(juce::MidiMessage::controllerEvent(7,0x03,64));
            sendMidi(juce::MidiMessage::controllerEvent(7,0x23,0));
            require(std::abs(restored.gain()-1.5*8192/16383.0) < 0.001 && !restored.midiPickupPending(), "FLX10 14-bit sampler volume/pickup failed");
            AudioEngine::setOutputRouting(1,3,2); restored.setCue(false);
            sendMidi(juce::MidiMessage::noteOn(7,0x69,static_cast<juce::uint8>(127)));
            require(!restored.cue(), "hardware sampler CUE bypasses headphone routing validation");
            sendMidi(juce::MidiMessage::noteOff(7,0x69));
            AudioEngine::setOutputRouting(midiMaster,midiBooth,midiHeadphones);
            restored.setQuantize(0);
            midi.selectPerformancePadMode("deckA", 3);
            midi.setPerformancePadPressed("deckA", 1, true);
            juce::AudioBuffer<float> renderedSample(2,512);
            for (int i = 0; i < 4; ++i) bus.getNextAudioBlock({&renderedSample,0,512});
            require(bus.sampler().state(9) == SamplerAudioEngine::Playing,
                "MIDI sampler pad depends on a deck connection or track instead of global sampler");
        }
        deck.play();
        require(deck.isPlaying(), "live fixture must have play intent");
        deck.loadTrack(directory.filePath("missing.wav"));
        require(deck.trackLoadError().contains("Load lock") && deck.hasTrack(), "load lock did not protect a playing deck");
        deck.ejectTrack();
        require(deck.hasTrack(), "eject bypassed load lock");
        deck.pause();
        deck.storeMemoryCue("Intro");
        require(deck.memoryCues().size() == 1, "memory cue storage failed");
        deck.renameMemoryCue(0, "Renamed");
        require(deck.memoryCues().first().toMap().value("label") == "Renamed", "memory cue rename failed");
        deck.storeSavedLoop(0);
        deck.setSavedLoopArmed(0, true);
        require(deck.savedLoops().first().toMap().value("armed").toBool(), "saved loop did not arm");
        deck.play();
        juce::AudioBuffer<float> rendered(2, 512);
        // The real callback completes the initial normal-playback handoff.
        // Advancing only the control clock leaves that handoff pending forever.
        for (int block = 0; block < 32 && bus.deck(0).renderModeRouter().normalPlaybackHandoffPending(); ++block)
            bus.getNextAudioBlock({&rendered, 0, 512});
        clock.advanceForTesting(0.02);
        require(deck.loopActive(), "entering an armed region did not activate its loop");
        deck.clearLoop(); clock.advanceForTesting(0.02);
        require(!deck.loopActive(), "leaving an active loop immediately retriggered it");
        deck.pause();
        auto request = QVariantMap{{"readOnlyExternal", true}, {"sourceType", "rekordboxDevice"},
            {"sourceId", "fixture-usb"}, {"trackId", "rekordbox:fixture-usb:1"},
            {"filePath", fixture.filePath()}, {"title", "Imported One"}};
        deck.loadExternalTrack(request);
        if (!require(wait([&] { return deck.readOnlyExternalTrack(); }), "external fixture did not load")) return false;
        deck.storeMemoryCue("External memory");
        deck.storeSavedLoop(0); deck.setSavedLoopArmed(0, true);
        request["trackId"] = "rekordbox:fixture-usb:2"; request["title"] = "Imported Two";
        deck.loadExternalTrack(request);
        if (!require(wait([&] { return deck.trackTitle() == "Imported Two"; }), "second external fixture did not load")) return false;
        require(deck.memoryCues().isEmpty(), "external supplemental data leaked into another track");
        request["trackId"] = "rekordbox:fixture-usb:1"; request["title"] = "Imported One";
        deck.loadExternalTrack(request);
        if (!require(wait([&] { return deck.trackTitle() == "Imported One"; }), "external fixture did not reload")) return false;
        require(deck.memoryCues().size() == 1 && deck.memoryCues().first().toMap().value("label") == "External memory",
                "external memory cue did not restore from local supplemental data");
        require(deck.savedLoops().first().toMap().value("armed").toBool(), "external active-loop flag did not persist");
        deck.deleteMemoryCue(0);
        require(deck.memoryCues().isEmpty(), "memory cue deletion failed");
        deck.prepareForShutdown();
        bus.releaseResources();
        deck.releaseTransportReaders();
    }
    database.shutdown(false);
    settings.getAppProperties().closeFiles();
    return ok;
}

bool testSoundColorAdjust(QString& error)
{
    FxManager manager;
    manager.setSoundColorParam(0.83f);
    manager.setSoundColorAdjustEnabled(false);
    for (const auto* mode : {"Space", "D.Echo", "Crush", "Pitch", "Noise", "Sweep", "Filter"}) {
        manager.setSoundColorMode(QString::fromLatin1(mode));
        const float automatic = manager.effectiveSoundColorParam();
        manager.setSoundColorParam(0.0f);
        if (!(automatic > 0.0f && automatic < 1.0f)
            || manager.effectiveSoundColorParam() != automatic) {
            error = QStringLiteral("disabled Sound Color Adjust depends on its hidden value");
            return false;
        }
        manager.setSoundColorParam(1.0f);
        if (manager.effectiveSoundColorParam() != automatic) {
            error = QStringLiteral("hidden Sound Color Adjust still changes the effect");
            return false;
        }
    }
    manager.setSoundColorMode(QStringLiteral("Pitch"));
    if (std::abs(1.0f + 35.0f * manager.effectiveSoundColorParam() - 12.0f) > 0.001f) {
        error = QStringLiteral("single-knob Sound Color Pitch does not span one octave");
        return false;
    }
    manager.setSoundColorParam(0.83f);
    manager.setSoundColorAdjustEnabled(true);
    if (manager.effectiveSoundColorParam() != 0.83f) {
        error = QStringLiteral("re-enabling Sound Color Adjust lost its manual value");
        return false;
    }

    QTemporaryDir directory;
    if (!directory.isValid()) {
        error = QStringLiteral("cannot create Sound Color settings fixture");
        return false;
    }
    auto& settings = SettingsManager::getInstance();
    auto& properties = settings.getAppProperties();
    juce::PropertiesFile::Options options;
    options.applicationName = "sound-color-adjust-test";
    options.filenameSuffix = ".xml";
    options.folderName = directory.path().toStdString();
    options.osxLibrarySubFolder = "Application Support";
    if (!options.getDefaultFile().isAChildOf(juce::File(directory.path().toStdString()))) {
        error = QStringLiteral("Sound Color settings fixture escaped its temporary directory");
        return false;
    }
    properties.setStorageParameters(options);
    if (!settings.soundColorAdjustEnabled()) {
        error = QStringLiteral("Sound Color Adjust compatibility default is disabled");
        return false;
    }
    for (const bool enabled : {false, true}) {
        settings.setSoundColorAdjustEnabled(enabled);
        properties.closeFiles();
        if (settings.soundColorAdjustEnabled() != enabled) {
            error = QStringLiteral("Sound Color Adjust setting did not survive a settings reload");
            properties.closeFiles();
            return false;
        }
    }
    properties.closeFiles();
    return true;
}

bool testAnalysisSettingsAndPersistence(QString& error)
{
    const auto require = [&error](bool value, const char* message) {
        if (!value && error.isEmpty()) error = QString::fromLatin1(message);
        return value;
    };
    QTemporaryDir directory;
    if (!require(directory.isValid(), "analysis fixture directory unavailable")) return false;
    auto& settings = SettingsManager::getInstance();
    auto& properties = settings.getAppProperties();
    properties.closeFiles();
    juce::PropertiesFile::Options storage;
    storage.applicationName = "analysis-test"; storage.filenameSuffix = ".xml";
    storage.folderName = directory.path().toStdString(); storage.osxLibrarySubFolder = "Application Support";
    if (!require(storage.getDefaultFile().isAChildOf(juce::File(directory.path().toStdString())), "analysis fixture escaped temporary directory")) return false;
    properties.setStorageParameters(storage);
    if (!require(settings.analysisProfile() == QLatin1String("electronic") && settings.analysisMinBpm() == 48
        && settings.analysisMaxBpm() == 255, "electronic analysis is not the broad-range default")) return false;
    settings.setAnalysisProfile(QStringLiteral("legacy"));
    settings.setAnalysisMinBpm(90); settings.setAnalysisMaxBpm(230);
    const auto queued = settings.analysisOptions();
    properties.closeFiles();
    if (!require(settings.analysisProfile() == QLatin1String("legacy") && settings.analysisMinBpm() == 90
        && settings.analysisMaxBpm() == 230, "analysis preferences did not survive reload")) return false;
    settings.setAnalysisProfile(QStringLiteral("electronic"));
    if (!require(settings.analysisMinBpm() == 48 && settings.analysisMaxBpm() == 255
        && queued.profile == analysis::AnalysisProfile::Legacy, "profiles or queued options contaminated one another")) return false;
    {
        LibraryDatabase database;
        if (!require(database.open(), "analysis database migration failed")) return false;
        if (!require(database.addTrack("analysis-track", "Analysis", "Fixture", 10, directory.filePath("track.wav")), "analysis fixture track insertion failed")) return false;
        int writes = 0;
        QObject::connect(&database, &LibraryDatabase::analysisUpdated, &database, [&](const QString&) { ++writes; });
        const auto wait = [&](int count) {
            QElapsedTimer timer; timer.start();
            while (writes < count && timer.elapsed() < 5000) {
                QCoreApplication::processEvents(); QThread::msleep(1);
            }
            return writes >= count;
        };
        analysis::AnalysisResult manual;
        manual.complete = manual.validated = true;
        manual.bpm = 128; manual.sampleRate = 44100; manual.firstBeatSample = 8820;
        manual.detectedKey = "8A"; manual.beatFingerprint = "manual";
        manual.beatGrid.lockedByUser = manual.beatGrid.userModified = true;
        manual.beatGrid.type = TrackData::BeatGridType::ConstantTempo;
        for (int i = 0; i < 8; ++i) {
            TrackData::BeatMarker marker; marker.positionSec = .2 + i * 60.0 / 128;
            marker.beatInBar = i % 4 + 1; manual.beats.push_back(marker);
        }
        if (!require(database.requestAnalysisPersistence("analysis-track", manual) && wait(1), "manual grid fixture did not persist")) return false;
        auto automatic = manual;
        automatic.bpm = 90; automatic.firstBeatSample = 100; automatic.sampleRate = 48000;
        automatic.beatGrid.lockedByUser = automatic.beatGrid.userModified = false;
        automatic.identity.optionsFingerprint = settings.analysisOptions().fingerprint();
        automatic.beatFingerprint = automatic.keyFingerprint = automatic.identity.optionsFingerprint;
        automatic.sections.beatGrid = 4; automatic.sections.key = 2;
        automatic.detectedKey = "9A";
        if (!require(database.requestAnalysisPersistence("analysis-track", automatic) && wait(2), "automatic analysis did not persist")) return false;
        LibraryDatabase::AnalysisSnapshot snapshot;
        if (!require(database.tryGetAnalysisData("analysis-track", &snapshot) && snapshot.bpm == 128
            && snapshot.firstBeatSample == 8820 && snapshot.sampleRate == 44100 && snapshot.beatGrid.size() == 8
            && snapshot.beatGrid.front().positionSec == .2 && snapshot.beatFingerprint == "manual"
            && snapshot.key == "9A" && snapshot.keyFingerprint == automatic.keyFingerprint,
            "automatic analysis overwrote a protected grid or lost the new key provenance")) return false;
    }
    properties.closeFiles();
    return true;
}

bool testFxManagerControls(QString& error)
{
    const auto require = [&error](bool condition, const QString& message) {
        if (!condition && error.isEmpty())
            error = message;
        return condition;
    };
    FxManager manager;
    for (const int unit : {1, 2}) {
        manager.setUnitEnabled(unit, true);
        if (!require(manager.unitEnabled(unit)
                         && (unit == 1 ? manager.wetDry1() : manager.wetDry2()) == 0.0f,
                     QStringLiteral("FX cold-start enable changed zero mix")))
            return false;
        manager.setUnitEnabled(unit, false);
    }
    int deck1BChangedCount = 0;
    int deck2BChangedCount = 0;
    int deck1AChangedCount = 0;
    int unit1EnabledChangedCount = 0;
    int unit2EnabledChangedCount = 0;
    int unit1WetDryChangedCount = 0;
    int unit2WetDryChangedCount = 0;
    QObject::connect(&manager, &FxManager::deck1BChanged,
                     [&] { ++deck1BChangedCount; });
    QObject::connect(&manager, &FxManager::deck2BChanged,
                     [&] { ++deck2BChangedCount; });
    QObject::connect(&manager, &FxManager::deck1AChanged,
                     [&] { ++deck1AChangedCount; });
    QObject::connect(&manager, &FxManager::enabled1Changed,
                     [&] { ++unit1EnabledChangedCount; });
    QObject::connect(&manager, &FxManager::enabled2Changed,
                     [&] { ++unit2EnabledChangedCount; });
    QObject::connect(&manager, &FxManager::wetDry1Changed,
                     [&] { ++unit1WetDryChangedCount; });
    QObject::connect(&manager, &FxManager::wetDry2Changed,
                     [&] { ++unit2WetDryChangedCount; });

    const QMetaObject* const metaObject = manager.metaObject();
    const int deck1BPropertyIndex = metaObject->indexOfProperty("deck1B");
    const int deck2BPropertyIndex = metaObject->indexOfProperty("deck2B");
    const int enabledPropertyIndex = metaObject->indexOfProperty("enabled1");
    if (!require(deck1BPropertyIndex >= 0 && deck2BPropertyIndex >= 0 &&
                     enabledPropertyIndex >= 0,
                 QStringLiteral("FxManager QObject properties are missing")))
        return false;
    const QMetaProperty deck1BProperty =
        metaObject->property(deck1BPropertyIndex);
    const QMetaProperty deck2BProperty =
        metaObject->property(deck2BPropertyIndex);
    if (!require(deck1BProperty.isValid() && deck1BProperty.isWritable() &&
                     deck2BProperty.isValid() && deck2BProperty.isWritable(),
                 QStringLiteral("FxManager deck assignment property is not writable")) ||
        !require(enabledPropertyIndex >= 0 &&
                     !metaObject->property(enabledPropertyIndex).isWritable(),
                 QStringLiteral("FxManager enabled property is unexpectedly writable")))
        return false;

    deck1BProperty.write(&manager, true);
    deck1BProperty.write(&manager, true);
    deck2BProperty.write(&manager, true);
    deck2BProperty.write(&manager, true);
    manager.setDeckAssignment(1, 1, true);
    manager.setDeckAssignment(1, 1, true);
    if (!require(deck1BProperty.read(&manager).toBool() &&
                     deck2BProperty.read(&manager).toBool() &&
                     deck1BChangedCount == 1 && deck2BChangedCount == 1,
                 QStringLiteral("FX assignment properties/signals did not de-duplicate per unit")) ||
        !require(manager.deck1A() && deck1AChangedCount == 1,
                 QStringLiteral("setDeckAssignment did not de-duplicate assignment signals")))
        return false;

    manager.setEffectType(1, QStringLiteral("Echo"));
    manager.setEffectType(2, QStringLiteral("Reverb"));
    manager.setWetDry(1, 0.7f);
    manager.setWetDry(2, 0.4f);
    if (!require(manager.effectType1() == QStringLiteral("Echo") &&
                     manager.effectType2() == QStringLiteral("Reverb") &&
                     qFuzzyCompare(manager.wetDry1(), 0.7f) &&
                     qFuzzyCompare(manager.wetDry2(), 0.4f),
                 QStringLiteral("FX units did not retain independent type and mix state")) ||
        !require(!manager.enabled1() && !manager.enabled2() &&
                     unit1EnabledChangedCount == 0 && unit2EnabledChangedCount == 0,
                 QStringLiteral("setting FX type or mix implicitly enabled a unit")))
        return false;

    manager.setUnitEnabled(1, true);
    if (!require(manager.enabled1() && !manager.enabled2() &&
                     unit1EnabledChangedCount == 1 && unit2EnabledChangedCount == 0 &&
                     unit1WetDryChangedCount == 1 && unit2WetDryChangedCount == 1,
                 QStringLiteral("enabling unit 1 changed unit 2 or emitted unexpected signals")))
        return false;

    manager.setUnitEnabled(2, true);
    manager.setUnitEnabled(1, false);
    manager.setWetDry(1, 0.2f);
    if (!require(!manager.enabled1() && manager.enabled2() &&
                     unit1EnabledChangedCount == 2 && unit2EnabledChangedCount == 1 &&
                     qFuzzyCompare(manager.wetDry1(), 0.2f) &&
                     qFuzzyCompare(manager.wetDry2(), 0.4f) &&
                     unit1WetDryChangedCount == 2 && unit2WetDryChangedCount == 1,
                 QStringLiteral("FX unit enable and mix updates were not isolated")))
        return false;

    return true;
}

bool testBeatFxFirstHardwareEngage(QString& error)
{
    ControlClock clock;
    ParameterStore store;
    FxManager fx;
    MidiControllerManager midi(&store, clock, nullptr,
                               MidiControllerManager::HardwareAccess::Disabled);
    midi.connectDecks(nullptr, nullptr, nullptr, nullptr);
    midi.connectFxManager(&fx);
    const auto send = [&store](const char* id, float value) {
        store.setMidiParameter(QString::fromLatin1(id), value);
    };
    const auto require = [&error](bool condition, const QString& message) {
        if (!condition && error.isEmpty())
            error = message;
        return condition;
    };
    if (!require(fx.soundColorMode() == QStringLiteral("None"),
                 QStringLiteral("Sound Color was selected at startup")))
        return false;
    midi.selectMapping(flx10::kMappingLabel);
    const auto sendColorWire = [&midi](int note, int velocity) {
        const juce::uint8 bytes[] {0x96, static_cast<juce::uint8>(note),
                                  static_cast<juce::uint8>(velocity)};
        static_cast<juce::MidiInputCallback&>(midi).handleIncomingMidiMessage(
            nullptr, juce::MidiMessage(bytes, 3));
        QCoreApplication::sendPostedEvents(&midi, QEvent::MetaCall);
    };
    const QStringList colorModes {
        QStringLiteral("Space"), QStringLiteral("D.Echo"), QStringLiteral("Crush"),
        QStringLiteral("Pitch"), QStringLiteral("Noise"), QStringLiteral("Filter")
    };
    for (int note = 0; note < colorModes.size(); ++note) {
        sendColorWire(note, 127);
        if (!require(fx.soundColorMode() == colorModes[note],
                     QStringLiteral("Sound Color wire ON failed for %1").arg(colorModes[note])))
            return false;
        sendColorWire(note, 127);
        if (!require(fx.soundColorMode() == colorModes[note],
                     QStringLiteral("repeated Sound Color wire ON toggled off")))
            return false;
        sendColorWire(note, 0);
        if (!require(fx.soundColorMode() == QStringLiteral("None"),
                     QStringLiteral("Sound Color zero-velocity wire OFF was dropped")))
            return false;
    }
    sendColorWire(5, 127);
    sendColorWire(4, 127);
    sendColorWire(5, 0);
    if (!require(fx.soundColorMode() == QStringLiteral("Noise"),
                 QStringLiteral("late wire OFF for Filter cleared Noise")))
        return false;
    sendColorWire(4, 0);
    sendColorWire(3, 127);
    sendColorWire(3, 0);
    sendColorWire(3, 127);
    if (!require(fx.soundColorMode() == QStringLiteral("Pitch"),
                 QStringLiteral("Sound Color wire re-selection required two presses")))
        return false;
    sendColorWire(3, 0);
    if (!require(fx.soundColorMode() == QStringLiteral("None"),
                 QStringLiteral("Sound Color wire OFF left a mode selected")))
        return false;
    send("deckA_sound_color", 1.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("None"),
                 QStringLiteral("turning an unassigned Color knob selected an effect")))
        return false;
    send("sound_color_fx_filter", 1.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Filter"),
                 QStringLiteral("Sound Color button failed to select Filter")))
        return false;
    send("sound_color_fx_filter", 1.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Filter"),
                 QStringLiteral("repeated Sound Color ON toggled off the selection")))
        return false;
    send("sound_color_fx_filter", 0.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("None"),
                 QStringLiteral("Sound Color OFF failed to deselect")))
        return false;
    send("sound_color_fx_filter", 1.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Filter"),
                 QStringLiteral("Sound Color ON after OFF failed to select")))
        return false;
    send("sound_color_fx_noise", 1.0f);
    send("sound_color_fx_filter", 0.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Noise"),
                 QStringLiteral("previous Sound Color OFF cleared the new selection")))
        return false;
    send("sound_color_fx_crush", 1.0f);
    send("sound_color_fx_noise", 0.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Crush"),
                 QStringLiteral("Sound Color mode replacement failed")))
        return false;
    send("sound_color_fx_crush", 0.0f);
    send("beat_fx_select_2", 1.0f);
    send("beat_fx_channel_deck_a", 1.0f);
    send("beat_fx_level_depth", 0.7f);
    if (!require(!fx.enabled1(), QStringLiteral("hardware FX selectors/knob engaged the unit")))
        return false;

    // This is the cold-start path: wetDry1 is still zero, while the physical
    // knob has already reported 0.7. Its synchronous notification used to
    // replace the pending ON with the unit's old disabled state.
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && midi.beatFxActive() && fx.deck1A()
                     && fx.effectType1() == QStringLiteral("Echo")
                     && qFuzzyCompare(fx.wetDry1(), 0.7f),
                 QStringLiteral("first hardware FX ON was cancelled before UI activation")))
        return false;
    send("beat_fx_on", 0.0f);
    send("beat_fx_level_depth", 0.3f);
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && midi.beatFxActive()
                     && qFuzzyCompare(fx.wetDry1(), 0.3f),
                 QStringLiteral("hardware FX re-engage lost the new knob depth")))
        return false;
    fx.setUnitEnabled(1, false);
    fx.setWetDry1(0.0f);
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && midi.beatFxActive() && fx.wetDry1() == 0.0f,
                 QStringLiteral("hardware FX ON raised zero UI depth")))
        return false;
    send("beat_fx_on", 0.0f);
    send("beat_fx_level_depth", 0.0f);
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && midi.beatFxActive() && fx.wetDry1() == 0.0f,
                 QStringLiteral("hardware FX ON raised the knob's zero depth")))
        return false;
    send("beat_fx_level_depth", 0.0005f);
    if (!require(fx.enabled1() && qFuzzyCompare(fx.wetDry1(), 0.0005f),
                 QStringLiteral("fine hardware FX depth was replaced by a default")))
        return false;
    send("beat_fx_on", 0.0f);
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && qFuzzyCompare(fx.wetDry1(), 0.0005f),
                 QStringLiteral("hardware FX re-engage raised fine depth")))
        return false;
    send("beat_fx_channel_master", 1.0f);
    send("beat_fx_level_depth", 0.4f);
    if (!require(fx.enabled1() && !fx.deck1A() && !fx.deck1B()
                     && !fx.deck1C() && !fx.deck1D(),
                 QStringLiteral("Master FX also left per-deck instances assigned")))
        return false;
    send("beat_fx_select_5", 1.0f);
    const float previousDivision = fx.beatDiv1();
    fx.setPrimaryParam(1, 0.01f);
    const QList<float> reverbSteps {0.01f, 0.10f, 0.25f, 0.50f, 0.75f, 0.90f, 1.0f};
    for (const float expected : reverbSteps) {
        if (expected != reverbSteps.front())
            send("beat_fx_beat_plus", 1.0f);
        if (!require(qFuzzyCompare(fx.primaryParam1(), expected)
                         && fx.beatDiv1() == previousDivision,
                     QStringLiteral("Reverb BEAT plus changed timing instead of percentage")))
            return false;
    }
    send("beat_fx_beat_plus", 1.0f);
    if (!require(fx.primaryParam1() == 1.0f,
                 QStringLiteral("Reverb percentage exceeded 100 percent")))
        return false;
    for (auto it = reverbSteps.crbegin() + 1; it != reverbSteps.crend(); ++it) {
        send("beat_fx_beat_minus", 1.0f);
        if (!require(qFuzzyCompare(fx.primaryParam1(), *it),
                     QStringLiteral("Reverb BEAT minus skipped a percentage step")))
            return false;
    }
    send("beat_fx_beat_minus", 1.0f);
    send("beat_fx_level_depth", 0.8f);
    send("beat_fx_on", 0.0f);
    send("beat_fx_on", 1.0f);
    if (!require(qFuzzyCompare(fx.primaryParam1(), 0.01f)
                     && qFuzzyCompare(fx.wetDry1(), 0.8f),
                 QStringLiteral("Reverb mix or ON overwrote its percentage")))
        return false;
    send("beat_fx_select_2", 1.0f);
    send("beat_fx_beat_plus", 1.0f);
    if (!require(fx.beatDiv1() > previousDivision,
                 QStringLiteral("Echo BEAT plus no longer controls timing")))
        return false;
    fx.setUnitEnabled(1, false);
    return require(!midi.beatFxActive(),
                   QStringLiteral("UI FX OFF did not update hardware-side state"));
}
}

int runCiSmokeTest(int argc, char* argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    QGuiApplication app(argc, argv);
    juce::ScopedJuceInitialiser_GUI juceRuntime;
    QCoreApplication::setApplicationName(QStringLiteral("BrockDJ CI smoke test"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BROCKDJ_VERSION));

    QString fxManagerError;
    if (!testLiveDeckFeatures(fxManagerError))
        return fail(fxManagerError);
    if (!testSoundColorAdjust(fxManagerError))
        return fail(fxManagerError);
    if (!testAnalysisSettingsAndPersistence(fxManagerError))
        return fail(fxManagerError);
    if (!testFxManagerControls(fxManagerError))
        return fail(fxManagerError);
    if (!testBeatFxFirstHardwareEngage(fxManagerError))
        return fail(fxManagerError);

    const QString qmlResource = QStringLiteral(":/DJSoftware/src/qml/main.qml");
    if (!QFile::exists(qmlResource))
        return fail(QStringLiteral("embedded main QML resource is missing"));
    if (!QFile::exists(flx10::kMappingResource))
        return fail(QStringLiteral("embedded controller mapping resource is missing"));

    QQmlEngine qmlEngine;
    configureEmbeddedQmlEngine(qmlEngine);
    QQmlComponent component(&qmlEngine, QUrl(QStringLiteral("qrc%1").arg(qmlResource)));
    while (component.isLoading())
        QCoreApplication::processEvents();
    if (component.isError())
        return fail(QStringLiteral("main QML failed to load (Qt %1; import paths: %2):\n%3")
                        .arg(QString::fromLatin1(qVersion()),
                             qmlEngine.importPathList().join(QStringLiteral(", ")),
                             component.errorString()));

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
        return fail(QStringLiteral("temporary directory creation failed"));

    const QString connectionName = QStringLiteral("brockdj_ci_smoke");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(temporaryDirectory.filePath(QStringLiteral("library.sqlite")));
        if (!database.open())
            return fail(QStringLiteral("temporary SQLite database failed to open: %1")
                            .arg(database.lastError().text()));

        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "CREATE TABLE smoke_check (id INTEGER PRIMARY KEY, value TEXT NOT NULL)")))
            return fail(QStringLiteral("temporary SQLite initialization failed: %1")
                            .arg(query.lastError().text()));
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    const auto runStartupSubprocess = [](const QString& flag, int timeoutMs,
                                         QString& error) {
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(), {flag});
        if (!process.waitForStarted(5000)) {
            error = QStringLiteral("isolated startup smoke could not start: %1")
                        .arg(process.errorString());
            return false;
        }
        if (!process.waitForFinished(timeoutMs)) {
            process.terminate();
            if (!process.waitForFinished(2000)) {
                process.kill();
                process.waitForFinished(2000);
            }
            const QString output = QString::fromLocal8Bit(process.readAllStandardOutput())
                + QString::fromLocal8Bit(process.readAllStandardError());
            error = QStringLiteral("isolated startup smoke exceeded its deadline (%1):\n%2")
                        .arg(flag, output);
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            error = QStringLiteral("isolated startup smoke failed (%1):\n%2")
                        .arg(flag,
                             QString::fromLocal8Bit(process.readAllStandardOutput())
                                 + QString::fromLocal8Bit(process.readAllStandardError()));
            return false;
        }
        return true;
    };

    QString startupSmokeError;
    if (!runStartupSubprocess(QStringLiteral("--ci-startup-smoke-test"), 35000,
                              startupSmokeError))
        return fail(startupSmokeError);
    if (!runStartupSubprocess(QStringLiteral("--ci-startup-early-close-test"), 15000,
                              startupSmokeError))
        return fail(startupSmokeError);

    qInfo().noquote() << "BrockDJ" << BROCKDJ_VERSION << BROCKDJ_BUILD_ARCH
                      << "package, startup-close, and early-close smoke tests passed";
    return 0;
}
