#pragma once

#include "app/ControlClock.h"
#include "DjMasterBus.h"

#include <QPointer>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <array>
#include <memory>
#include <mutex>

class DjEngine;
class ParameterStore;
class MidiControllerManager;
class ControllerIntegrationManager;
class LibraryManager;
class LibraryDatabase;
class LibraryTableModel;
class LibraryAnalysisManager;
class FxManager;
class LinkManager;
class SystemMonitor;
class CursorControl;
class CoverArtProvider;
class LibraryCoverService;
class LibraryPreviewPlayer;
class SettingsManager;
class AppConfig;
class AppExitGate;
class MixerParameterBridge;
class MixerControl;
class AudioDeviceService;
class AudioPageCache;
class MediaIoScheduler;
class UiScaleController;
class WaveformZoomController;
namespace engine::sync { class SyncCoordinator; }

struct ApplicationRuntime {
    QQmlApplicationEngine* engine = nullptr;
    SettingsManager* settingsManager = nullptr;
    AppConfig* appConfig = nullptr;
    AppExitGate* exitGate = nullptr;

    std::unique_ptr<AudioDeviceService> audioDeviceService;
    std::unique_ptr<AudioPageCache> audioPageCache;
    std::unique_ptr<DjMasterBus> masterBus;
    std::unique_ptr<ControlClock> controlClock;
    ControlClock::Registration syncClockRegistration;
    std::unique_ptr<engine::sync::SyncCoordinator> syncCoordinator;
    std::unique_ptr<DjEngine> deckA;
    std::unique_ptr<DjEngine> deckB;
    std::unique_ptr<DjEngine> deckC;
    std::unique_ptr<DjEngine> deckD;
    std::array<DjMasterBus::DeckRegistration, DjMasterBus::kMaximumDecks> deckRegistrations;

    std::unique_ptr<ParameterStore> parameterStore;
    std::unique_ptr<MediaIoScheduler> mediaIoScheduler;
    std::unique_ptr<MixerParameterBridge> mixerParameterBridge;
    std::unique_ptr<MixerControl> mixerControl;
    QPointer<MidiControllerManager> midiManager;
    std::unique_ptr<ControllerIntegrationManager> controllerManager;

    std::unique_ptr<LibraryManager> libraryManager;
    std::unique_ptr<LibraryDatabase> libraryDb;
    std::unique_ptr<LibraryTableModel> libraryTableModel;
    std::unique_ptr<LibraryAnalysisManager> libraryAnalysisManager;
    std::unique_ptr<LibraryPreviewPlayer> libraryPreviewPlayer;
    DjMasterBus::AuxRegistration previewRegistration;

    std::unique_ptr<FxManager> fxManager;
    std::unique_ptr<LinkManager> linkManager;
    std::unique_ptr<SystemMonitor> sysMonitor;
    std::unique_ptr<CursorControl> cursorControl;
    std::unique_ptr<UiScaleController> uiScaleController;
    std::unique_ptr<WaveformZoomController> waveformZoomController;
    std::unique_ptr<CoverArtProvider> coverProvider;
    std::unique_ptr<LibraryCoverService> libraryCoverService;

    CoverArtProvider* coverProviderPtr = nullptr;
    QPointer<QObject> rootObjectForStartup;
    bool runtimeInitStarted = false;
};

namespace ApplicationLifecycle {

void stopQuickWindowRendering(QQuickWindow* window);

void clearQmlContextProperties(QQmlApplicationEngine& engine);

// Phase 1: user-initiated exit (AppExitGate) — decks, GPU, library, controllers, MIDI.
void performExitTeardown(ApplicationRuntime& runtime, bool manualBackup);

// Phase 2: post event-loop — audio device, transport readers, QML teardown, settings.
void shutdownApplication(ApplicationRuntime& runtime);

} // namespace ApplicationLifecycle
