#include "ApplicationLifecycle.h"

#include "DjEngine.h"
#include "audio/AudioEngine.h"
#include "SettingsManager.h"
#include "app/AppConfig.h"
#include "app/AppExitGate.h"
#include "controllers/ControllerIntegrationManager.h"
#include "fx/FxManager.h"
#include "library/LibraryAnalysisManager.h"
#include "library/LibraryCoverService.h"
#include "library/LibraryDatabase.h"
#include "library/LibraryManager.h"
#include "library/LibraryPreviewPlayer.h"
#include "library/LibraryTableModel.h"
#include "link/LinkManager.h"
#include "midi/MidiControllerManager.h"
#include "midi/ParameterStore.h"
#include "audio/device/AudioDeviceService.h"
#include "audio/cache/AudioPageCache.h"
#include "engine/sync/SyncCoordinator.h"
#include "io/MediaIoScheduler.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QVariant>

#include <mutex>

namespace {

std::once_flag g_exitTeardownOnce;
std::once_flag g_shutdownOnce;

} // namespace

namespace ApplicationLifecycle {

void stopQuickWindowRendering(QQuickWindow* window)
{
    if (!window)
        return;

    window->hide();
    if (window->isSceneGraphInitialized())
        window->releaseResources();
}

void clearQmlContextProperties(QQmlApplicationEngine& engine)
{
    engine.rootContext()->setContextProperty("settingsManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("appConfig", QVariant());
    engine.rootContext()->setContextProperty("appExit", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckA", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckB", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckC", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckD", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("libraryManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("libraryDb", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("libraryModel", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("libraryAnalyzer", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("fxManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("linkManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("sysMonitor", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("parameterStore", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("midiManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("controllerManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("cursorControl", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("uiScaleController", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("waveformZoomController", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("mixerControl", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("controlClock", static_cast<QObject*>(nullptr));
}

void performExitTeardown(ApplicationRuntime& runtime, bool manualBackup)
{
    std::call_once(g_exitTeardownOnce, [&]() {
        if (!runtime.settingsManager)
            return;

        if (runtime.controlClock)
            runtime.controlClock->stop();

        runtime.settingsManager->setRequestManualBackupOnExit(manualBackup);
        runtime.settingsManager->flushToDisk();

        if (runtime.libraryAnalysisManager) {
            runtime.libraryAnalysisManager->cancel();
            if (runtime.libraryDb) {
                QObject::disconnect(runtime.libraryAnalysisManager.get(), nullptr,
                                    runtime.libraryDb.get(), nullptr);
                QObject::disconnect(runtime.libraryDb.get(), nullptr,
                                    runtime.libraryAnalysisManager.get(), nullptr);
            }
        }

        for (DjEngine* deck : {runtime.deckA.get(), runtime.deckB.get(),
                               runtime.deckC.get(), runtime.deckD.get()}) {
            if (deck)
                deck->prepareForShutdown();
        }

        QQuickWindow* quickWindow = nullptr;
        if (runtime.engine) {
            for (QObject* root : runtime.engine->rootObjects()) {
                if (auto* window = qobject_cast<QQuickWindow*>(root))
                    quickWindow = window;
            }
        }
        stopQuickWindowRendering(quickWindow);

        if (runtime.libraryDb)
            runtime.libraryDb->shutdown(manualBackup);

        if (runtime.engine) {
            runtime.engine->rootContext()->setContextProperty("midiManager",
                                                               static_cast<QObject*>(nullptr));
            runtime.engine->rootContext()->setContextProperty("controllerManager",
                                                               static_cast<QObject*>(nullptr));
        }

        if (runtime.controllerManager && runtime.settingsManager) {
            QObject::disconnect(runtime.settingsManager, nullptr,
                                runtime.controllerManager.get(), nullptr);
            runtime.controllerManager->setFlx10Enabled(false);
            runtime.controllerManager->prepareForShutdown();
            runtime.controllerManager.reset();
        }

        if (runtime.midiManager) {
            QCoreApplication::removePostedEvents(runtime.midiManager);
            runtime.midiManager->shutdown();
        }

        runtime.settingsManager->markCleanShutdown();
    });
}

void shutdownApplication(ApplicationRuntime& runtime)
{
    std::call_once(g_shutdownOnce, [&]() {
        if (!runtime.settingsManager)
            return;

        performExitTeardown(runtime, runtime.settingsManager->requestManualBackupOnExit());

        if (runtime.linkManager)
            runtime.linkManager->shutdown();

        if (runtime.audioEngine) {
            runtime.audioEngine->unregisterCallback(runtime.audioDeviceService->manager());
            runtime.previewRegistration.reset();
            runtime.audioEngine->beginShutdown();
        }

        if (runtime.libraryPreviewPlayer)
            runtime.libraryPreviewPlayer->stop();

        if (runtime.audioDeviceService)
            runtime.audioDeviceService->closeAudioDevice();

        for (DjEngine* deck : {runtime.deckA.get(), runtime.deckB.get(),
                               runtime.deckC.get(), runtime.deckD.get()}) {
            if (deck)
                deck->releaseTransportReaders();
        }

        if (runtime.engine) {
            clearQmlContextProperties(*runtime.engine);
            runtime.engine->clearComponentCache();
            const auto qmlRoots = runtime.engine->rootObjects();
            for (QObject* root : qmlRoots)
                root->deleteLater();

            for (int i = 0; i < 100 && !runtime.engine->rootObjects().isEmpty(); ++i)
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        }

        runtime.deckD.reset();
        runtime.deckC.reset();
        runtime.deckB.reset();
        runtime.deckA.reset();
        runtime.audioEngine.reset();
        runtime.libraryPreviewPlayer.reset();
        runtime.syncClockRegistration.reset();
        if (runtime.syncCoordinator)
            runtime.syncCoordinator->shutdown();
        runtime.audioPageCache.reset();

        // Consumers go away before the scheduler rejects new work and joins its
        // single general-purpose I/O thread.
        runtime.libraryCoverService.reset();
        runtime.libraryManager.reset();
        if (runtime.mediaIoScheduler) {
            runtime.mediaIoScheduler->requestStop();
            runtime.mediaIoScheduler->stopAndJoin();
            runtime.mediaIoScheduler.reset();
        }

        runtime.linkManager.reset();
        runtime.audioDeviceService.reset();
        runtime.libraryDb.reset();

        runtime.settingsManager->shutdown();
    });
}

} // namespace ApplicationLifecycle
