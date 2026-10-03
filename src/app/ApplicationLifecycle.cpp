#include "ApplicationLifecycle.h"

#include "deck/DjEngine.h"
#include "audio/AudioEngine.h"
#include "SettingsManager.h"
#include "controllers/ControllerIntegrationManager.h"
#include "fx/FxManager.h"
#include "library/LibraryAnalysisManager.h"
#include "library/LibraryCoverService.h"
#include "library/LibraryDatabase.h"
#include "library/LibraryManager.h"
#include "library/LibraryPreviewPlayer.h"
#include "library/LibraryTableModel.h"
#include "library/devices/DeviceLibraryManager.h"
#include "link/LinkManager.h"
#include "controllers/midi/MidiControllerManager.h"
#include "controllers/midi/ParameterStore.h"
#include "audio/device/AudioDeviceService.h"
#include "audio/cache/AudioPageCache.h"
#include "deck/sync/DeckSync.h"
#include "library/MediaIoScheduler.h"

#include <QCoreApplication>
#include <QEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QThread>
#include <QVariant>

namespace ApplicationLifecycle {

void stopQuickWindowRendering(QQuickWindow* window)
{
    if (!window)
        return;

    window->hide();
    if (window->isSceneGraphInitialized())
        window->releaseResources();
}

void setQmlContextProperty(QQmlApplicationEngine& engine,
                           QmlContextProperty property,
                           QObject* object)
{
    const auto index = static_cast<std::size_t>(property);
    engine.rootContext()->setContextProperty(kQmlContextPropertyContract[index].name, object);
}

void clearQmlContextProperties(QQmlApplicationEngine& engine)
{
    for (std::size_t index = 0; index < kQmlContextPropertyContract.size(); ++index) {
        const auto& property = kQmlContextPropertyContract[index];
        if (property.clearAsInvalidVariant)
            engine.rootContext()->setContextProperty(property.name, QVariant());
        else
            engine.rootContext()->setContextProperty(property.name, static_cast<QObject*>(nullptr));
    }
}

void performExitTeardown(ApplicationRuntime& runtime, bool manualBackup)
{
    if (runtime.exitTeardownStarted)
        return;
    runtime.exitTeardownStarted = true;
    runtime.stopping = true;

    if (runtime.controlClock)
        runtime.controlClock->stop();

    if (runtime.settingsManager) {
        runtime.settingsManager->setRequestManualBackupOnExit(manualBackup);
        runtime.settingsManager->flushToDisk();
    }

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
        setQmlContextProperty(*runtime.engine, QmlContextProperty::MidiManager, nullptr);
        setQmlContextProperty(*runtime.engine, QmlContextProperty::ControllerManager, nullptr);
    }

    if (runtime.controllerManager) {
        if (runtime.settingsManager)
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

    if (runtime.settingsManager)
        runtime.settingsManager->markCleanShutdown();
    runtime.exitTeardownComplete = true;
}

void shutdownApplication(ApplicationRuntime& runtime)
{
    if (runtime.shutdownStarted
        || (runtime.exitTeardownStarted && !runtime.exitTeardownComplete)) {
        return;
    }
    runtime.shutdownStarted = true;
    runtime.stopping = true;

    performExitTeardown(runtime, runtime.settingsManager
                                     ? runtime.settingsManager->requestManualBackupOnExit()
                                     : false);

    if (runtime.linkManager)
        runtime.linkManager->shutdown();

    if (runtime.audioEngine) {
        if (runtime.audioDeviceService)
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
        Q_ASSERT(QThread::currentThread() == runtime.engine->thread());
        clearQmlContextProperties(*runtime.engine);
        runtime.engine->clearComponentCache();
        const auto qmlRoots = runtime.engine->rootObjects();
        for (QObject* root : qmlRoots) {
            root->deleteLater();
            QCoreApplication::sendPostedEvents(root, QEvent::DeferredDelete);
        }
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
    runtime.syncCoordinator.reset();
    runtime.audioPageCache.reset();

    // Consumers go away before the scheduler rejects new work and joins its
    // single general-purpose I/O thread.
    runtime.libraryCoverService.reset();
    runtime.deviceLibraryManager.reset();
    runtime.libraryManager.reset();
    if (runtime.mediaIoScheduler) {
        runtime.mediaIoScheduler->requestStop();
        runtime.mediaIoScheduler->stopAndJoin();
        runtime.mediaIoScheduler.reset();
    }

    runtime.linkManager.reset();
    runtime.audioDeviceService.reset();
    runtime.libraryDb.reset();

    if (runtime.settingsManager)
        runtime.settingsManager->shutdown();
}

} // namespace ApplicationLifecycle
