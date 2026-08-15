#include "ApplicationBootstrap.h"
#include "ApplicationLifecycle.h"
#include "platform/PosixSignalHandler.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <array>
#include <memory>

// Qt Includes
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQml/qqml.h>
#include <QFont>
#include <QIcon>
#include <QSize>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QTimer>
#include <QStandardPaths>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QQuickGraphicsConfiguration>
#include <QScreen>
#include <QSGRendererInterface>
#include <QtGlobal>

#include "deck/DjEngine.h"
#include "audio/AudioEngine.h"
#include "audio/TimeStretchProcessor.h"
#include "library/LibraryManager.h"
#include "library/devices/DeviceLibraryManager.h"
#include "library/MediaIoScheduler.h"
#include "library/CoverArtProvider.h"
#include "library/LibraryCoverService.h"
#include "library/LibraryPreviewPlayer.h"
#include "fx/FxManager.h"
#include "link/LinkManager.h"
#include "SystemMonitor.h"
#include "controllers/midi/ParameterStore.h"
#include "MixerControl.h"
#include "controllers/midi/MidiControllerManager.h"
#include "controllers/ControllerIntegrationManager.h"
#include "SettingsManager.h"
#include "app/AppConfig.h"
#include "library/LibraryDatabase.h"
#include "library/LibraryTableModel.h"
#include "library/LibraryAnalysisManager.h"
#include "app/CursorControl.h"
#include "audio/device/AudioDeviceService.h"
#include "audio/cache/AudioPageCache.h"
#include "deck/sync/SyncCoordinator.h"
#include "app/ControlClock.h"
#include "app/UiScaleController.h"
#include "app/WaveformZoomController.h"

using namespace Qt::StringLiterals;

namespace {
TimeStretchBackend timeStretchBackendForSetting(const QString& backend)
{
    return backend.compare(QLatin1String("rubberband"), Qt::CaseInsensitive) == 0
        ? TimeStretchBackend::RubberBand : TimeStretchBackend::Signalsmith;
}

QtMessageHandler g_previousMessageHandler = nullptr;
const QString kBreezeDialOverrideWarning = QStringLiteral("Member fillColor of the object BreezeDial overrides a member of the base object");

void setEnvDefault(const char* name, const char* value)
{
    if (qEnvironmentVariableIsEmpty(name))
        qputenv(name, value);
}

bool renderDiagnosticsEnabled()
{
    const auto value = qEnvironmentVariable("BROCKDJ_RENDER_DIAGNOSTICS")
                           .trimmed().toLower();
    return value == "1" || value == "true" || value == "on";
}

const char* graphicsApiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Software: return "software";
    case QSGRendererInterface::OpenVG: return "openvg";
    case QSGRendererInterface::OpenGL: return "opengl";
    case QSGRendererInterface::Direct3D11: return "d3d11";
    case QSGRendererInterface::Vulkan: return "vulkan";
    case QSGRendererInterface::Metal: return "metal";
    case QSGRendererInterface::Null: return "null";
    case QSGRendererInterface::Direct3D12: return "d3d12";
    case QSGRendererInterface::Unknown: break;
    }
    return "unknown";
}

class RenderDiagnosticsEventFilter final : public QObject
{
public:
    explicit RenderDiagnosticsEventFilter(QObject* parent)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* window = qobject_cast<QWindow*>(watched);
        if (!window)
            return QObject::eventFilter(watched, event);
        switch (event->type()) {
        case QEvent::Expose:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::WindowStateChange:
        case QEvent::ScreenChangeInternal:
        case QEvent::DevicePixelRatioChange:
            qInfo() << "[render-diagnostics] window event=" << event->type()
                    << "visible=" << window->isVisible()
                    << "exposed=" << window->isExposed()
                    << "state=" << window->windowState()
                    << "size=" << window->size()
                    << "dpr=" << window->devicePixelRatio()
                    << "screen="
                    << (window->screen() ? window->screen()->name() : QString());
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }
};

void configureQtRuntimeDefaults()
{
    // Cross-platform UI: never inherit the host OS Quick Controls theme (e.g. KDE Breeze).
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    setEnvDefault("QT_SCALE_FACTOR_ROUNDING_POLICY", "RoundPreferFloor");

    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES")) {
        qputenv("QT_LOGGING_RULES", renderDiagnosticsEnabled()
                ? "qt.scenegraph.general=true;qt.rhi.general=true"
                : "qt.scenegraph.general=false;qt.rhi.general=false");
    }
}

void filteredMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message.contains(kBreezeDialOverrideWarning))
        return;

    if (g_previousMessageHandler)
        g_previousMessageHandler(type, context, message);
}

#if defined(Q_OS_LINUX)
void configureLinuxVulkanBackend(bool& useVulkan,
                                 QString& requestedVkIcd)
{
    QString rhiBackend = qEnvironmentVariable("BROCKDJ_RHI_BACKEND").trimmed().toLower();
    if (rhiBackend.isEmpty())
        rhiBackend = QStringLiteral("vulkan");

    if (rhiBackend == "vulkan") {
        qputenv("QSG_RHI_BACKEND", "vulkan");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        useVulkan = true;
        qDebug() << "[startup] RHI backend forced to vulkan";
    } else if (rhiBackend == "opengl") {
        qputenv("QSG_RHI_BACKEND", "opengl");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        qWarning() << "[startup] RHI backend forced to opengl (diagnostics only)";
    } else if (rhiBackend == "auto") {
        qDebug() << "[startup] RHI backend auto (BROCKDJ_RHI_BACKEND=auto)";
    } else {
        qWarning() << "[startup] Unknown BROCKDJ_RHI_BACKEND value; using Qt auto selection:"
                   << rhiBackend;
    }

    if (!useVulkan)
        return;

    requestedVkIcd = qEnvironmentVariable("BROCKDJ_VK_ICD").trimmed();
    if (!requestedVkIcd.isEmpty())
        qputenv("VK_ICD_FILENAMES", requestedVkIcd.toUtf8());
}
#endif
}

int runApplication(int argc, char *argv[])
{
    bool useVulkan = false;
    QString requestedVkIcd;
    QElapsedTimer startupTimer;
    startupTimer.start();

    auto logStartupStep = [&startupTimer](const char* step) {
        qDebug() << "[startup]" << step << startupTimer.elapsed() << "ms";
    };

    qInfo("========================================");
    qInfo("BROCK DJ ENGINE - INITIAL BUILD TEST");
    qInfo("JUCE Version:   %s", juce::SystemStats::getJUCEVersion().toRawUTF8());
    qInfo("C++ Standard:   %lld", static_cast<long long>(__cplusplus));
    qInfo("========================================");

    qDebug() << "Essentia disabled by project policy; using internal analysis pipeline.";

    g_previousMessageHandler = qInstallMessageHandler(filteredMessageHandler);
    QQuickWindow::setTextRenderType(QQuickWindow::CurveTextRendering);

    configureQtRuntimeDefaults();

#if defined(Q_OS_LINUX)
    // Preserve the current Vulkan production default while keeping auto and
    // OpenGL available for the Wayland comparison matrix. Qt owns whichever
    // backend is selected; do not manufacture a second Vulkan lifecycle here.
    configureLinuxVulkanBackend(useVulkan, requestedVkIcd);
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);

    // Must run before QGuiApplication; pairs with QT_QUICK_CONTROLS_STYLE above.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QGuiApplication app(argc, argv);
    const bool renderDiagnostics = renderDiagnosticsEnabled();
    if (renderDiagnostics) {
        qInfo() << "[render-diagnostics] Qt=" << qVersion()
                << "platform=" << QGuiApplication::platformName()
                << "session=" << qEnvironmentVariable("XDG_SESSION_TYPE")
                << "requestedRhi=" << qEnvironmentVariable("BROCKDJ_RHI_BACKEND", "vulkan")
                << "renderLoop=" << qEnvironmentVariable("QSG_RENDER_LOOP", "default")
                << "pipelineCache=" << qEnvironmentVariable("BROCKDJ_VK_CACHE", "on")
                << "icdOverride=" << qEnvironmentVariable("VK_ICD_FILENAMES");
    }
#if defined(Q_OS_UNIX)
    PosixSignalHandler posixSignals;
    if (posixSignals.initialize()) {
        QObject::connect(&posixSignals, &PosixSignalHandler::shutdownRequested,
                         &app, &QCoreApplication::quit, Qt::QueuedConnection);
    } else {
        qWarning() << "[startup] POSIX signal integration unavailable:"
                   << posixSignals.errorString();
    }
#endif
    logStartupStep("QGuiApplication created");

#if defined(Q_OS_LINUX)
    if (useVulkan) {
        if (!requestedVkIcd.isEmpty())
            qDebug() << "[startup] Vulkan ICD override:" << requestedVkIcd;
        const QString requestedVkApi = qEnvironmentVariable("BROCKDJ_VK_API").trimmed();
        if (!requestedVkApi.isEmpty()) {
            qWarning() << "[startup] BROCKDJ_VK_API is ignored: Qt owns the Vulkan instance"
                       << "and negotiates the API required by Qt Quick:" << requestedVkApi;
        }
        if (!qEnvironmentVariableIsEmpty("BROCKDJ_VK_ICD_AUTO")) {
            qWarning() << "[startup] BROCKDJ_VK_ICD_AUTO is obsolete;"
                       << "the Vulkan loader now selects the device unless BROCKDJ_VK_ICD is explicit";
        }
        qDebug() << "[startup] Vulkan instance ownership delegated to Qt Quick";
    }
#endif // Q_OS_LINUX

    // Load app icon from embedded QRC (generated by scripts/generate_icons.sh).
    // Multiple sizes let Qt pick the sharpest one for each use (taskbar, title bar, dock).
    {
        static constexpr std::array<int, 7> kIconSizes {16, 32, 48, 64, 128, 256, 512};
        QIcon appIcon;
        for (const int sz : kIconSizes) {
            const QString path = QStringLiteral(":/icons/%1.png").arg(sz);
            appIcon.addFile(path, QSize(sz, sz));
        }
        if (!appIcon.isNull())
            app.setWindowIcon(appIcon);
    }

    // Set a global default font with proper hinting strategy
    QFont defaultFont = app.font();
    defaultFont.setHintingPreference(QFont::PreferFullHinting);
    defaultFont.setStyleStrategy(QFont::PreferAntialias);
    app.setFont(defaultFont);

    juce::ScopedJuceInitialiser_GUI juceInit;
    logStartupStep("JUCE GUI initialised");

    SettingsManager::getInstance().init();
    logStartupStep("SettingsManager init done");

    auto& settingsManager = SettingsManager::getInstance();

    AppConfig appConfig;
    appConfig.init(settingsManager.getConfigDirectoryPath());
    logStartupStep("AppConfig init done");
    QQmlApplicationEngine engine;

    ApplicationRuntime runtime;
    runtime.engine = &engine;
    runtime.settingsManager = &settingsManager;
    runtime.appConfig = &appConfig;
    runtime.parameterStore = std::make_unique<ParameterStore>();
    runtime.mediaIoScheduler = std::make_unique<MediaIoScheduler>();
    runtime.mediaIoScheduler->start();
    runtime.libraryManager = std::make_unique<LibraryManager>(*runtime.mediaIoScheduler);
    runtime.libraryDb = std::make_unique<LibraryDatabase>();
    runtime.libraryTableModel = std::make_unique<LibraryTableModel>("library_conn");
    runtime.libraryAnalysisManager = std::make_unique<LibraryAnalysisManager>();
    runtime.deviceLibraryManager = std::make_unique<DeviceLibraryManager>();
    runtime.fxManager = std::make_unique<FxManager>();
    runtime.controlClock = std::make_unique<ControlClock>();
    runtime.linkManager = std::make_unique<LinkManager>(*runtime.controlClock);
    runtime.sysMonitor = std::make_unique<SystemMonitor>(*runtime.controlClock);
    runtime.cursorControl = std::make_unique<CursorControl>();
    runtime.uiScaleController = std::make_unique<UiScaleController>(&settingsManager);
    runtime.waveformZoomController = std::make_unique<WaveformZoomController>(&settingsManager);
    runtime.coverProvider = std::make_unique<CoverArtProvider>();
    runtime.coverProviderPtr = runtime.coverProvider.get();
    runtime.libraryCoverService = std::make_unique<LibraryCoverService>(
        runtime.coverProviderPtr, *runtime.mediaIoScheduler);
    runtime.mixerControl = std::make_unique<MixerControl>();
    runtime.audioDeviceService = std::make_unique<AudioDeviceService>();
    runtime.audioPageCache = std::make_unique<AudioPageCache>();
    settingsManager.setAudioDeviceService(runtime.audioDeviceService.get());
    QObject::connect(runtime.audioDeviceService.get(), &AudioDeviceService::configurationChanged,
                     &settingsManager, [&settingsManager, &runtime]() {
                         if (!runtime.audioDeviceService)
                             return;
                         settingsManager.persistActiveAudioConfiguration(
                             runtime.audioDeviceService->currentDeviceType(),
                             runtime.audioDeviceService->currentOutputDevice(),
                             runtime.audioDeviceService->currentSampleRate(),
                             runtime.audioDeviceService->currentBufferSize());
                     });
    runtime.audioEngine = std::make_unique<AudioEngine>(*runtime.audioPageCache);
    runtime.syncCoordinator = std::make_unique<engine::sync::SyncCoordinator>();
    ControlClock::Callbacks syncClockCallbacks;
    syncClockCallbacks.syncCoordinate = [&runtime](const ControlTickContext&) {
        if (runtime.syncCoordinator)
            runtime.syncCoordinator->update();
    };
    runtime.syncClockRegistration = runtime.controlClock->registerCallbacks(
        std::move(syncClockCallbacks));
    runtime.syncCoordinator->setTightDoubleSyncEnabled(settingsManager.tightDoubleSync());
    QObject::connect(&settingsManager, &SettingsManager::tightDoubleSyncChanged,
                     runtime.linkManager.get(),
                     [&runtime, &settingsManager]() {
                         if (runtime.syncCoordinator)
                             runtime.syncCoordinator->setTightDoubleSyncEnabled(
                                 settingsManager.tightDoubleSync());
                     });
    const auto publishLinkSnapshot = [&runtime]() {
        if (!runtime.syncCoordinator || !runtime.linkManager)
            return;
        const auto previous = runtime.syncCoordinator->snapshot();
        engine::sync::LinkSyncSnapshot link;
        link.enabled = runtime.linkManager->enabled();
        link.numPeers = runtime.linkManager->numPeers();
        link.bpm = runtime.linkManager->bpm();
        link.beat = runtime.linkManager->beat();
        link.phase = runtime.linkManager->phase();
        link.generation = previous.stateGeneration + 1;
        runtime.syncCoordinator->setLinkSnapshot(link);
    };
    QObject::connect(runtime.linkManager.get(), &LinkManager::enabledChanged, publishLinkSnapshot);
    QObject::connect(runtime.linkManager.get(), &LinkManager::bpmChanged, publishLinkSnapshot);
    QObject::connect(runtime.linkManager.get(), &LinkManager::beatChanged, publishLinkSnapshot);
    QObject::connect(runtime.linkManager.get(), &LinkManager::phaseChanged, publishLinkSnapshot);
    QObject::connect(runtime.linkManager.get(), &LinkManager::numPeersChanged, publishLinkSnapshot);
    publishLinkSnapshot();
    QObject::connect(runtime.audioDeviceService.get(), &AudioDeviceService::routingChanged,
                     [](int master, int booth, int headphones) {
                         AudioEngine::setOutputRouting(master, booth, headphones);
                     });

    engine.addImageProvider("coverart", runtime.coverProvider.release());
    logStartupStep("Cover art provider installed");

    engine.rootContext()->setContextProperty("settingsManager", &settingsManager);
    engine.rootContext()->setContextProperty("appConfig", &appConfig);
    engine.rootContext()->setContextProperty("deckA", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckB", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckC", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckD", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("libraryManager", runtime.libraryManager.get());
    engine.rootContext()->setContextProperty("libraryDb",    static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("libraryModel", runtime.libraryTableModel.get());
    engine.rootContext()->setContextProperty("libraryAnalyzer", runtime.libraryAnalysisManager.get());
    engine.rootContext()->setContextProperty("deviceLibraryManager", runtime.deviceLibraryManager.get());
    engine.rootContext()->setContextProperty("fxManager", runtime.fxManager.get());
    engine.rootContext()->setContextProperty("linkManager", runtime.linkManager.get());
    engine.rootContext()->setContextProperty("sysMonitor", runtime.sysMonitor.get());
    engine.rootContext()->setContextProperty("parameterStore", runtime.parameterStore.get());
    engine.rootContext()->setContextProperty("midiManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("controllerManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("cursorControl", runtime.cursorControl.get());
    engine.rootContext()->setContextProperty("uiScaleController", runtime.uiScaleController.get());
    engine.rootContext()->setContextProperty("waveformZoomController", runtime.waveformZoomController.get());
    qmlRegisterSingletonInstance("BrockDJ.Mixer", 1, 0, "Control", runtime.mixerControl.get());
    engine.rootContext()->setContextProperty("mixerControl", runtime.mixerControl.get());
    engine.rootContext()->setContextProperty("controlClock", runtime.controlClock.get());
    engine.rootContext()->setContextProperty("libraryCover", runtime.libraryCoverService.get());

    const auto url = QUrl(u"qrc:/DJSoftware/src/qml/main.qml"_s);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    auto initialiseRuntime = [&]() {
        if (runtime.runtimeInitStarted)
            return;
        runtime.runtimeInitStarted = true;

        if (!runtime.libraryDb->open())
            qWarning() << "[main] LibraryDatabase failed to open – library features disabled.";
        logStartupStep("LibraryDatabase open attempted");

        runtime.libraryDb->setTableModel(runtime.libraryTableModel.get());

        QObject::connect(runtime.libraryDb.get(), &LibraryDatabase::trackMetaChanged,
                         runtime.libraryTableModel.get(), &LibraryTableModel::refreshMetaForTrack);

        runtime.libraryAnalysisManager->setLibraryDatabase(runtime.libraryDb.get());
        engine.rootContext()->setContextProperty("libraryDb", runtime.libraryDb.get());

        runtime.libraryTableModel->refresh();
        logStartupStep("LibraryTableModel refreshed");

        if (runtime.rootObjectForStartup)
            runtime.rootObjectForStartup->setProperty("startupLibraryReady", true);

        QTimer::singleShot(0, &app, [&]() {
            runtime.deckA = std::make_unique<DjEngine>(*runtime.audioDeviceService, *runtime.audioPageCache,
                                                       runtime.audioEngine->deck(0),
                                                       *runtime.controlClock,
                                                       *runtime.syncCoordinator, 0);
            runtime.deckB = std::make_unique<DjEngine>(*runtime.audioDeviceService, *runtime.audioPageCache,
                                                       runtime.audioEngine->deck(1),
                                                       *runtime.controlClock,
                                                       *runtime.syncCoordinator, 1);
            runtime.deckC = std::make_unique<DjEngine>(*runtime.audioDeviceService, *runtime.audioPageCache,
                                                       runtime.audioEngine->deck(2),
                                                       *runtime.controlClock,
                                                       *runtime.syncCoordinator, 2);
            runtime.deckD = std::make_unique<DjEngine>(*runtime.audioDeviceService, *runtime.audioPageCache,
                                                       runtime.audioEngine->deck(3),
                                                       *runtime.controlClock,
                                                       *runtime.syncCoordinator, 3);
            logStartupStep("DjEngines constructed");

            for (const auto [deck, name] : std::array<std::pair<DjEngine*, const char*>, 4>{{
                    {runtime.deckA.get(), "deckA"}, {runtime.deckB.get(), "deckB"},
                    {runtime.deckC.get(), "deckC"}, {runtime.deckD.get(), "deckD"}}})
                deck->setCoverArtProvider(runtime.coverProviderPtr, name);

            engine.rootContext()->setContextProperty("deckA", runtime.deckA.get());
            engine.rootContext()->setContextProperty("deckB", runtime.deckB.get());
            engine.rootContext()->setContextProperty("deckC", runtime.deckC.get());
            engine.rootContext()->setContextProperty("deckD", runtime.deckD.get());

            auto* midi = new MidiControllerManager(runtime.parameterStore.get(),
                                                   *runtime.controlClock, &app);
            QQmlEngine::setObjectOwnership(midi, QQmlEngine::CppOwnership);
            runtime.midiManager = midi;
            runtime.midiManager->connectDecks(
                runtime.deckA.get(), runtime.deckB.get(),
                runtime.deckC.get(), runtime.deckD.get());
            engine.rootContext()->setContextProperty("midiManager", runtime.midiManager.data());

            runtime.controllerManager = std::make_unique<ControllerIntegrationManager>(
                *runtime.controlClock);
            runtime.controllerManager->setDecks(runtime.deckA.get(), runtime.deckB.get());
            engine.rootContext()->setContextProperty("controllerManager", runtime.controllerManager.get());
            QObject::connect(&settingsManager,
                             &SettingsManager::controllerSettingsChanged,
                             runtime.controllerManager.get(),
                             [&settingsManager, controller = runtime.controllerManager.get()] {
                                 controller->setFlx10Enabled(settingsManager.flx10ControllerSupportEnabled());
                             });
            runtime.controllerManager->setFlx10Enabled(settingsManager.flx10ControllerSupportEnabled());

            const auto applyTimeStretchBackend = [&runtime, &settingsManager] {
                const auto backend = timeStretchBackendForSetting(settingsManager.timeStretchBackend());
                for (DjEngine* deck : {runtime.deckA.get(), runtime.deckB.get(),
                                       runtime.deckC.get(), runtime.deckD.get()})
                    deck->audioEndpoint().setTimeStretchBackend(backend);
            };
            QObject::connect(&settingsManager, &SettingsManager::timeStretchBackendChanged,
                             &app, applyTimeStretchBackend);
            applyTimeStretchBackend();

            for (DjEngine* deck : {runtime.deckA.get(), runtime.deckB.get(),
                                   runtime.deckC.get(), runtime.deckD.get()}) {
                deck->setLibraryDatabase(runtime.libraryDb.get());
                deck->setLibraryCoverService(runtime.libraryCoverService.get());
                QObject::connect(runtime.deviceLibraryManager.get(),
                                 &DeviceLibraryManager::deviceRemoved,
                                 deck, &DjEngine::externalSourceUnavailable);
                QObject::connect(runtime.deviceLibraryManager.get(),
                                 &DeviceLibraryManager::deviceEjectRequested,
                                 deck, &DjEngine::ejectExternalSource);
            }

            runtime.fxManager->registerEngines(runtime.deckA.get(), runtime.deckB.get(),
                                               runtime.deckC.get(), runtime.deckD.get());
            runtime.midiManager->connectFxManager(runtime.fxManager.get());

            runtime.libraryPreviewPlayer = std::make_unique<LibraryPreviewPlayer>(
                *runtime.controlClock, *runtime.audioPageCache, &app);
            QObject::connect(runtime.deviceLibraryManager.get(),
                             &DeviceLibraryManager::deviceEjectRequested,
                             runtime.libraryPreviewPlayer.get(),
                             [preview = runtime.libraryPreviewPlayer.get()](const QString&) {
                                 preview->stop();
                             });
            runtime.previewRegistration = runtime.audioEngine->registerAuxEndpoint(
                *runtime.libraryPreviewPlayer);
            engine.rootContext()->setContextProperty("libraryPreview",
                                                     runtime.libraryPreviewPlayer.get());
            runtime.mixerControl->attachParameterStore(runtime.parameterStore.get());
            runtime.mixerControl->setDecks(runtime.deckA.get(), runtime.deckB.get(),
                                           runtime.deckC.get(), runtime.deckD.get());

            // Register the source player before opening the hardware. In
            // particular, JACK may not fully activate a callback that is added
            // only after the client/device has already been opened. A manual
            // Apply appeared to fix startup because it reopened the device with
            // this callback already registered.
            runtime.audioEngine->registerCallback(runtime.audioDeviceService->manager());

            // SettingsManager owns the preferred configuration.  Do not replace
            // it with a backend fallback (or an unavailable-device default) at
            // startup: AudioDeviceService publishes the active configuration.
            const QString preferredAudioType = settingsManager.getAudioMasterDeviceType();
            const QString preferredAudioOutput = settingsManager.getAudioMasterOutputDevice();
            const bool audioSettingsApplied = runtime.deckA->applyAudioDeviceSettings(
                preferredAudioType,
                preferredAudioOutput,
                settingsManager.getAudioSampleRate(),
                settingsManager.getAudioBufferSize(),
                settingsManager.getAudioMasterFirstChannel(),
                settingsManager.getAudioHeadphonesFirstChannel(),
                settingsManager.getAudioBoothFirstChannel());

            if (audioSettingsApplied) {
                qDebug() << "[startup] Audio preference restored:"
                         << "preferred=" << preferredAudioType << "/" << preferredAudioOutput
                         << "active=" << runtime.audioDeviceService->currentDeviceType()
                         << "/" << runtime.audioDeviceService->currentOutputDevice();
            } else {
                qWarning() << "[startup] Audio preference could not be restored:"
                           << preferredAudioType << "/" << preferredAudioOutput
                           << runtime.audioDeviceService->lastError();

                // Some Linux audio backends become enumerable shortly after the
                // GUI is ready. Retry a bounded number of times; never poll or
                // replace the user's preferred device with a fallback.
                for (const int delayMs : {750, 2500}) {
                    QTimer::singleShot(delayMs, &app, [&runtime, &settingsManager, delayMs]() {
                        if (!runtime.audioDeviceService || !runtime.deckA
                            || !runtime.audioDeviceService->currentOutputDevice().isEmpty()) {
                            return;
                        }

                        const QString retryType = settingsManager.getAudioMasterDeviceType();
                        const QString retryOutput = settingsManager.getAudioMasterOutputDevice();
                        const bool restored = runtime.audioDeviceService->applySettings(
                            retryType,
                            retryOutput,
                            settingsManager.getAudioSampleRate(),
                            settingsManager.getAudioBufferSize(),
                            settingsManager.getAudioMasterFirstChannel(),
                            settingsManager.getAudioHeadphonesFirstChannel(),
                            settingsManager.getAudioBoothFirstChannel());
                        if (restored) {
                            qDebug() << "[startup] Audio preference restored on retry"
                                     << delayMs << "ms:"
                                     << runtime.audioDeviceService->currentDeviceType()
                                     << "/" << runtime.audioDeviceService->currentOutputDevice();
                        } else {
                            qWarning() << "[startup] Audio preference retry failed after"
                                       << delayMs << "ms:"
                                       << runtime.audioDeviceService->lastError();
                        }
                    });
                }
            }

            runtime.controlClock->start();
            qDebug() << "[startup] Audio device setup finished" << startupTimer.elapsed() << "ms";
        });
    };

    engine.load(url);
    logStartupStep("QML load requested");

    if (!engine.rootObjects().isEmpty()) {
        runtime.rootObjectForStartup = engine.rootObjects().first();
        if (auto* rootWindow = qobject_cast<QWindow*>(engine.rootObjects().first())) {
            qDebug() << "[main] Root window found, setting size and visibility";

            if (auto* quickWindow = qobject_cast<QQuickWindow*>(rootWindow)) {
                const bool usingVulkan = (QQuickWindow::graphicsApi() == QSGRendererInterface::Vulkan);
                if (usingVulkan) {
                    const QString cacheMode = qEnvironmentVariable("BROCKDJ_VK_CACHE").trimmed().toLower();
                    const bool resetCache = (cacheMode == "reset");
                    const bool enableCache = resetCache || cacheMode.isEmpty()
                        || cacheMode == "1" || cacheMode == "on" || cacheMode == "true";

                    if (enableCache) {
                        const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
                        QDir().mkpath(cacheDir);
                        const QString cacheFile = cacheDir + "/vk_pipeline_cache.bin";
                        if (resetCache)
                            QFile::remove(cacheFile);

                        QQuickGraphicsConfiguration cfg = quickWindow->graphicsConfiguration();
                        cfg.setPipelineCacheSaveFile(cacheFile);
                        if (!resetCache)
                            cfg.setPipelineCacheLoadFile(cacheFile);
                        quickWindow->setGraphicsConfiguration(cfg);

                        QFileInfo cacheInfo(cacheFile);
                        if (cacheInfo.exists())
                            qDebug() << "[main] Vulkan pipeline cache:" << cacheFile << cacheInfo.size() << "bytes";
                        else
                            qDebug() << "[main] Vulkan pipeline cache (new):" << cacheFile;
                    } else {
                        qDebug() << "[main] Vulkan pipeline cache disabled (BROCKDJ_VK_CACHE=0)";
                    }
                }

                QObject::connect(
                    quickWindow,
                    &QQuickWindow::sceneGraphInitialized,
                    &app,
                    [quickWindow, &startupTimer, renderDiagnostics]() {
                    qDebug() << "[startup] Scene graph initialized" << startupTimer.elapsed() << "ms";
                    if (renderDiagnostics) {
                        const auto* renderer = quickWindow->rendererInterface();
                        qInfo() << "[render-diagnostics] scene graph initialized"
                                << "api=" << graphicsApiName(
                                       renderer ? renderer->graphicsApi()
                                                : QSGRendererInterface::Unknown)
                                << "thread=" << QThread::currentThread();
                    }
                    },
                    static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::SingleShotConnection));

                if (renderDiagnostics) {
                    auto* diagnosticsFilter = new RenderDiagnosticsEventFilter(quickWindow);
                    quickWindow->installEventFilter(diagnosticsFilter);
                    QObject::connect(
                        quickWindow, &QQuickWindow::sceneGraphInvalidated,
                        &app, [] {
                            qInfo() << "[render-diagnostics] scene graph invalidated"
                                    << "thread=" << QThread::currentThread();
                        }, Qt::DirectConnection);
                    QObject::connect(
                        quickWindow, &QWindow::screenChanged,
                        &app, [quickWindow](QScreen* screen) {
                            qInfo() << "[render-diagnostics] screen changed"
                                    << "screen=" << (screen ? screen->name() : QString())
                                    << "dpr=" << quickWindow->devicePixelRatio()
                                    << "refresh=" << (screen ? screen->refreshRate() : 0.0);
                        });
                }

                QObject::connect(
                    quickWindow,
                    &QQuickWindow::sceneGraphError,
                    &app,
                    [&startupTimer](QQuickWindow::SceneGraphError error, const QString& message) {
                    qWarning() << "[startup] Scene graph error" << error << message
                               << "at" << startupTimer.elapsed() << "ms";
                    },
                    Qt::DirectConnection);

                QObject::connect(
                    quickWindow,
                    &QQuickWindow::beforeRendering,
                    &app,
                    [&startupTimer]() {
                    qDebug() << "[startup] First render started" << startupTimer.elapsed() << "ms";
                    },
                    static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::SingleShotConnection));

                QObject::connect(
                    quickWindow,
                    &QQuickWindow::afterRendering,
                    &app,
                    [&startupTimer]() {
                    qDebug() << "[startup] FIRST FRAME RENDERED" << startupTimer.elapsed() << "ms";
                    },
                    static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::SingleShotConnection));

                QObject::connect(
                    quickWindow,
                    &QQuickWindow::afterRendering,
                    &app,
                    [&app, &initialiseRuntime]() {
                    QMetaObject::invokeMethod(&app, initialiseRuntime, Qt::QueuedConnection);
                    },
                    static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::SingleShotConnection));
            }

            rootWindow->show();
            logStartupStep("Root window shown");

#if defined(Q_OS_MACOS)
            // macOS requires extra steps to properly show the window
            rootWindow->raise();
            rootWindow->requestActivate();
            qDebug() << "[main] macOS: Window raised and activated";
#endif
        } else {
            qWarning() << "[main] Root object is not a QWindow!";
        }
    } else {
        qCritical() << "[main] No root objects found after loading QML!";
        settingsManager.markCleanShutdown();
        settingsManager.shutdown();
        QCoreApplication::exit(-1);
        return -1;
    }

    auto* exitGate = new AppExitGate(&app);
    runtime.exitGate = exitGate;
    exitGate->setHandler([&runtime](bool manualBackup) {
        ApplicationLifecycle::performExitTeardown(runtime, manualBackup);
        QCoreApplication::quit();
    });
    engine.rootContext()->setContextProperty("appExit", exitGate);

    const int ret = app.exec();
    ApplicationLifecycle::shutdownApplication(runtime);

    return ret;
}
