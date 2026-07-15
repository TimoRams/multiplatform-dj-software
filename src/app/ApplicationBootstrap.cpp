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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QQuickGraphicsConfiguration>
#include <QtGlobal>
#include <QVersionNumber>
#if defined(Q_OS_LINUX)
#include <QVulkanInstance>
#endif

#include "DjEngine.h"
#include "DjMasterBus.h"
#include "library/LibraryManager.h"
#include "io/MediaIoScheduler.h"
#include "library/CoverArtProvider.h"
#include "library/LibraryCoverService.h"
#include "library/LibraryPreviewPlayer.h"
#include "fx/FxManager.h"
#include "link/LinkManager.h"
#include "SystemMonitor.h"
#include "midi/ParameterStore.h"
#include "midi/MixerParameterBridge.h"
#include "MixerControl.h"
#include "midi/MidiControllerManager.h"
#include "controllers/ControllerIntegrationManager.h"
#include "SettingsManager.h"
#include "app/AppConfig.h"
#include "app/AppExitGate.h"
#include "library/LibraryDatabase.h"
#include "library/LibraryTableModel.h"
#include "library/LibraryAnalysisManager.h"
#include "app/CursorControl.h"
#include "audio/device/AudioDeviceService.h"
#include "audio/cache/AudioPageCache.h"
#include "engine/sync/SyncCoordinator.h"
#include "app/ControlClock.h"
#include "app/UiScaleController.h"
#include "app/WaveformZoomController.h"

using namespace Qt::StringLiterals;

namespace {
QtMessageHandler g_previousMessageHandler = nullptr;
const QString kBreezeDialOverrideWarning = QStringLiteral("Member fillColor of the object BreezeDial overrides a member of the base object");

void setEnvDefault(const char* name, const char* value)
{
    if (qEnvironmentVariableIsEmpty(name))
        qputenv(name, value);
}

void configureQtRuntimeDefaults()
{
    // Cross-platform UI: never inherit the host OS Quick Controls theme (e.g. KDE Breeze).
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    setEnvDefault("QT_SCALE_FACTOR_ROUNDING_POLICY", "RoundPreferFloor");

    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES")) {
        qputenv("QT_LOGGING_RULES",
                "qt.scenegraph.general=false;"
                "qt.rhi.general=false");
    }
}

QString pickDefaultVulkanIcd()
{
#if defined(Q_OS_LINUX)
    const QString icdDirPath = QStringLiteral("/usr/share/vulkan/icd.d");
    QDir icdDir(icdDirPath);
    if (!icdDir.exists())
        return {};

    const QStringList files = icdDir.entryList({QStringLiteral("*.json")}, QDir::Files);
    if (files.isEmpty())
        return {};

    const auto hasFile = [&files](const QString& name) { return files.contains(name); };
    const auto filePath = [&icdDir](const QString& name) { return icdDir.absoluteFilePath(name); };

    const QString glVendor = qEnvironmentVariable("__GLX_VENDOR_LIBRARY_NAME").trimmed().toLower();
    const bool wantNvidia = qEnvironmentVariable("__NV_PRIME_RENDER_OFFLOAD") == "1"
        || qEnvironmentVariable("DRI_PRIME") == "1"
        || glVendor == "nvidia";

    if (wantNvidia && hasFile("nvidia_icd.json"))
        return filePath("nvidia_icd.json");

    if (hasFile("intel_icd.json"))
        return filePath("intel_icd.json");

    if (hasFile("intel_hasvk_icd.json"))
        return filePath("intel_hasvk_icd.json");

    if (files.size() == 1)
        return filePath(files.first());
#endif

    return {};
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
                                 QVersionNumber& requestedVkApi,
                                 QString& requestedVkApiRaw,
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
        qWarning() << "[startup] Unknown BROCKDJ_RHI_BACKEND value, forcing vulkan:" << rhiBackend;
        qputenv("QSG_RHI_BACKEND", "vulkan");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        useVulkan = true;
    }

    if (!useVulkan)
        return;

    requestedVkIcd = qEnvironmentVariable("BROCKDJ_VK_ICD").trimmed();
    if (requestedVkIcd.isEmpty() && qEnvironmentVariableIsEmpty("VK_ICD_FILENAMES")) {
        const QString autoMode = qEnvironmentVariable("BROCKDJ_VK_ICD_AUTO").trimmed().toLower();
        const bool allowAuto = autoMode.isEmpty() || autoMode == "1" || autoMode == "true" || autoMode == "on";
        if (allowAuto)
            requestedVkIcd = pickDefaultVulkanIcd();
    }

    if (!requestedVkIcd.isEmpty())
        qputenv("VK_ICD_FILENAMES", requestedVkIcd.toUtf8());

    requestedVkApiRaw = qEnvironmentVariable("BROCKDJ_VK_API").trimmed();
    QString apiEnv = requestedVkApiRaw;
    apiEnv.remove('"');
    apiEnv.remove('\'');
    apiEnv = apiEnv.trimmed().toLower();

    if (!apiEnv.isEmpty()) {
        if (apiEnv == "latest")
            requestedVkApi = QVersionNumber(1, 4);
        else
            requestedVkApi = QVersionNumber::fromString(apiEnv);
    }
}
#endif
}

int runApplication(int argc, char *argv[])
{
    bool useVulkan = false;
    QVersionNumber requestedVkApi;
    QString requestedVkApiRaw;
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
    // Vulkan is required for production; allow env override for diagnostics.
    configureLinuxVulkanBackend(useVulkan, requestedVkApi, requestedVkApiRaw, requestedVkIcd);
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);

    // Must run before QGuiApplication; pairs with QT_QUICK_CONTROLS_STYLE above.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QGuiApplication app(argc, argv);
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
    std::unique_ptr<QVulkanInstance> vkInstance;
    if (useVulkan) {
        if (!requestedVkIcd.isEmpty())
            qDebug() << "[startup] Vulkan ICD override:" << requestedVkIcd;

        if (!requestedVkApiRaw.isEmpty()) {
            if (!requestedVkApi.isNull())
                qDebug() << "[startup] Vulkan API override:" << requestedVkApi;
            else
                qWarning() << "[startup] Invalid BROCKDJ_VK_API value:" << requestedVkApiRaw;
        }

        vkInstance = std::make_unique<QVulkanInstance>();
        if (!requestedVkApi.isNull())
            vkInstance->setApiVersion(requestedVkApi);

        QElapsedTimer vkTimer;
        vkTimer.start();
        const bool vkOk = vkInstance->create();
        qDebug() << "[startup] Vulkan instance create" << (vkOk ? "ok" : "failed")
                 << vkTimer.elapsed() << "ms";

        if (!vkOk)
            vkInstance.reset();
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
    runtime.masterBus = std::make_unique<DjMasterBus>();
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
                         DjMasterBus::setOutputRouting(master, booth, headphones);
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
                                                       *runtime.controlClock,
                                                       *runtime.syncCoordinator, 0);
            runtime.deckB = std::make_unique<DjEngine>(*runtime.audioDeviceService, *runtime.audioPageCache,
                                                       *runtime.controlClock,
                                                       *runtime.syncCoordinator, 1);
            runtime.deckC = std::make_unique<DjEngine>(*runtime.audioDeviceService, *runtime.audioPageCache,
                                                       *runtime.controlClock,
                                                       *runtime.syncCoordinator, 2);
            runtime.deckD = std::make_unique<DjEngine>(*runtime.audioDeviceService, *runtime.audioPageCache,
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
            runtime.midiManager->connectDecks(runtime.deckA.get(), runtime.deckB.get());
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

            for (DjEngine* deck : {runtime.deckA.get(), runtime.deckB.get(),
                                   runtime.deckC.get(), runtime.deckD.get()}) {
                deck->setLibraryDatabase(runtime.libraryDb.get());
                deck->setLibraryCoverService(runtime.libraryCoverService.get());
            }

            runtime.fxManager->registerEngines(runtime.deckA.get(), runtime.deckB.get(),
                                               runtime.deckC.get(), runtime.deckD.get());

            runtime.libraryPreviewPlayer = std::make_unique<LibraryPreviewPlayer>(
                *runtime.controlClock, &app);
            runtime.previewRegistration = runtime.masterBus->registerAuxEndpoint(
                *runtime.libraryPreviewPlayer);
            engine.rootContext()->setContextProperty("libraryPreview",
                                                     runtime.libraryPreviewPlayer.get());
            const std::array<DjEngine*, 4> masterBusDecks {
                runtime.deckA.get(), runtime.deckB.get(), runtime.deckC.get(), runtime.deckD.get()
            };
            for (std::size_t index = 0; index < masterBusDecks.size(); ++index)
                runtime.deckRegistrations[index] = runtime.masterBus->registerDeck(
                    masterBusDecks[index]->audioEndpoint(), static_cast<int>(index));

            runtime.mixerParameterBridge = std::make_unique<MixerParameterBridge>(
                runtime.parameterStore.get());
            runtime.mixerParameterBridge->setDecks(runtime.deckA.get(), runtime.deckB.get(),
                                                   runtime.deckC.get(), runtime.deckD.get());
            runtime.mixerParameterBridge->setMixerControl(runtime.mixerControl.get());

            runtime.mixerControl->setDecks(runtime.deckA.get(), runtime.deckB.get(),
                                           runtime.deckC.get(), runtime.deckD.get());

            runtime.deckA->applyAudioDeviceSettings(settingsManager.getAudioMasterDeviceType(),
                                            settingsManager.getAudioMasterOutputDevice(),
                                            settingsManager.getAudioSampleRate(),
                                            settingsManager.getAudioBufferSize(),
                                            settingsManager.getAudioMasterFirstChannel(),
                                            settingsManager.getAudioHeadphonesFirstChannel(),
                                            settingsManager.getAudioBoothFirstChannel());
            const int actualSR  = runtime.deckA->getCurrentAudioSampleRate();
            const int actualBuf = runtime.deckA->getCurrentAudioBufferSize();
            if (actualSR  > 0) settingsManager.setAudioSampleRate(actualSR);
            if (actualBuf > 0) settingsManager.setAudioBufferSize(actualBuf);

            runtime.masterBus->registerCallback(runtime.audioDeviceService->manager());
            runtime.controlClock->start();
            qDebug() << "[startup] Audio device settings applied" << startupTimer.elapsed() << "ms";
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
#if defined(Q_OS_LINUX)
                if (usingVulkan && vkInstance)
                    quickWindow->setVulkanInstance(vkInstance.get());
#endif
                if (usingVulkan) {
                    const QString cacheMode = qEnvironmentVariable("BROCKDJ_VK_CACHE").trimmed().toLower();
                    const bool enableCache = cacheMode.isEmpty() || cacheMode == "1" || cacheMode == "on" || cacheMode == "true";
                    const bool resetCache = (cacheMode == "reset");

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
                    [&startupTimer]() {
                    qDebug() << "[startup] Scene graph initialized" << startupTimer.elapsed() << "ms";
                    },
                    static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::SingleShotConnection));

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
