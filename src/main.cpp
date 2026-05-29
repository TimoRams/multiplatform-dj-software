#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <array>
#include <memory>

// Qt Includes
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QQmlContext>
#include <QFont>
#include <QIcon>
#include <QSize>
#include <QElapsedTimer>
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
#include "audio/DjMasterBus.h"
#include "WaveformItem.h"
#include "library/LibraryManager.h"
#include "library/CoverArtProvider.h"
#include "fx/FxManager.h"
#include "link/LinkManager.h"
#include "SystemMonitor.h"
#include "midi/ParameterStore.h"
#include "midi/MidiControllerManager.h"
#include "SettingsManager.h"
#include "app/AppConfig.h"
#include "library/LibraryDatabase.h"
#include "library/LibraryTableModel.h"
#include "library/LibraryAnalysisManager.h"
#include "app/CursorControl.h"

using namespace Qt::StringLiterals;

namespace {
QtMessageHandler g_previousMessageHandler = nullptr;
const QString kBreezeDialOverrideWarning = QStringLiteral("Member fillColor of the object BreezeDial overrides a member of the base object");

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
}

int main(int argc, char *argv[])
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

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR_ROUNDING_POLICY"))
        qputenv("QT_SCALE_FACTOR_ROUNDING_POLICY", "RoundPreferFloor");

    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES")) {
        qputenv("QT_LOGGING_RULES",
                "qt.scenegraph.general=false;"
                "qt.rhi.general=false");
    }

#if defined(Q_OS_LINUX)
    // Vulkan is required for production; allow env override for diagnostics.
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

    if (useVulkan) {
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
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);

    QGuiApplication app(argc, argv);
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

    std::unique_ptr<DjEngine> deckA;
    std::unique_ptr<DjEngine> deckB;
    std::unique_ptr<DjEngine> deckC;
    std::unique_ptr<DjEngine> deckD;
    std::unique_ptr<DjMasterBus> masterBus;
    auto parameterStore = std::make_unique<ParameterStore>();
    std::unique_ptr<MidiControllerManager> midiManager; // constructed after app.exec() — CoreMIDI needs CFRunLoop
    auto libraryManager = std::make_unique<LibraryManager>();
    auto libraryDb = std::make_unique<LibraryDatabase>();
    auto libraryTableModel = std::make_unique<LibraryTableModel>("library_conn");
    auto libraryAnalysisManager = std::make_unique<LibraryAnalysisManager>();
    auto fxManager = std::make_unique<FxManager>();
    auto linkManager = std::make_unique<LinkManager>();
    auto sysMonitor = std::make_unique<SystemMonitor>();
    auto cursorControl = std::make_unique<CursorControl>();
    auto coverProvider = std::make_unique<CoverArtProvider>();
    CoverArtProvider* coverProviderPtr = coverProvider.get();
    QPointer<QObject> rootObjectForStartup;
    bool runtimeInitStarted = false;

    engine.addImageProvider("coverart", coverProvider.release());
    logStartupStep("Cover art provider installed");

    engine.rootContext()->setContextProperty("settingsManager", &settingsManager);
    engine.rootContext()->setContextProperty("appConfig", &appConfig);
    engine.rootContext()->setContextProperty("deckA", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckB", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckC", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckD", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("libraryManager", libraryManager.get());
    engine.rootContext()->setContextProperty("libraryDb",    static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("libraryModel", libraryTableModel.get());
    engine.rootContext()->setContextProperty("libraryAnalyzer", libraryAnalysisManager.get());
    engine.rootContext()->setContextProperty("fxManager", fxManager.get());
    engine.rootContext()->setContextProperty("linkManager", linkManager.get());
    engine.rootContext()->setContextProperty("sysMonitor", sysMonitor.get());
    engine.rootContext()->setContextProperty("parameterStore", parameterStore.get());
    engine.rootContext()->setContextProperty("midiManager", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("cursorControl", cursorControl.get());

    const auto url = QUrl(u"qrc:/DJSoftware/src/qml/main.qml"_s);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    auto initialiseRuntime = [&]() {
        if (runtimeInitStarted)
            return;
        runtimeInitStarted = true;

        if (!libraryDb->open())
            qWarning() << "[main] LibraryDatabase failed to open – library features disabled.";
        logStartupStep("LibraryDatabase open attempted");

        libraryDb->setTableModel(libraryTableModel.get());

        // Keep library view in sync when rating/color/notes/energy change.
        QObject::connect(libraryDb.get(), &LibraryDatabase::trackMetaChanged,
                         libraryTableModel.get(), &LibraryTableModel::refreshMetaForTrack);

        libraryAnalysisManager->setLibraryDatabase(libraryDb.get());
        engine.rootContext()->setContextProperty("libraryDb", libraryDb.get());

        libraryTableModel->refresh();
        logStartupStep("LibraryTableModel refreshed");

        if (rootObjectForStartup)
            rootObjectForStartup->setProperty("startupLibraryReady", true);

        QTimer::singleShot(0, &app, [&]() {
            deckA = std::make_unique<DjEngine>();
            deckB = std::make_unique<DjEngine>();
            deckC = std::make_unique<DjEngine>();
            deckD = std::make_unique<DjEngine>();
            logStartupStep("DjEngines constructed");

            for (const auto [deck, name] : std::array<std::pair<DjEngine*, const char*>, 4>{{
                    {deckA.get(), "deckA"}, {deckB.get(), "deckB"},
                    {deckC.get(), "deckC"}, {deckD.get(), "deckD"}}})
                deck->setCoverArtProvider(coverProviderPtr, name);

            engine.rootContext()->setContextProperty("deckA", deckA.get());
            engine.rootContext()->setContextProperty("deckB", deckB.get());
            engine.rootContext()->setContextProperty("deckC", deckC.get());
            engine.rootContext()->setContextProperty("deckD", deckD.get());

            // Defer MIDI init: CoreMIDI on macOS needs the CFRunLoop (app.exec) running first.
            midiManager = std::make_unique<MidiControllerManager>(parameterStore.get());
            midiManager->connectDecks(deckA.get(), deckB.get());
            engine.rootContext()->setContextProperty("midiManager", midiManager.get());

            for (DjEngine* deck : {deckA.get(), deckB.get(), deckC.get(), deckD.get()})
                deck->setLibraryDatabase(libraryDb.get());

            fxManager->registerEngines(deckA.get(), deckB.get());

            masterBus = std::make_unique<DjMasterBus>();
            for (DjEngine* deck : {deckA.get(), deckB.get(), deckC.get(), deckD.get()})
                masterBus->addDeck(deck);

            deckA->applyAudioDeviceSettings(settingsManager.getAudioMasterDeviceType(),
                                            settingsManager.getAudioMasterOutputDevice(),
                                            settingsManager.getAudioSampleRate(),
                                            settingsManager.getAudioBufferSize(),
                                            settingsManager.getAudioMasterFirstChannel(),
                                            settingsManager.getAudioHeadphonesFirstChannel(),
                                            settingsManager.getAudioBoothFirstChannel());
            // Sync back what the driver actually opened.
            const int actualSR  = deckA->getCurrentAudioSampleRate();
            const int actualBuf = deckA->getCurrentAudioBufferSize();
            if (actualSR  > 0) settingsManager.setAudioSampleRate(actualSR);
            if (actualBuf > 0) settingsManager.setAudioBufferSize(actualBuf);

            masterBus->registerCallback(DjEngine::getSharedAudioDeviceManager());
            qDebug() << "[startup] Audio device settings applied" << startupTimer.elapsed() << "ms";
        });
    };

    engine.load(url);
    logStartupStep("QML load requested");

    if (!engine.rootObjects().isEmpty()) {
        rootObjectForStartup = engine.rootObjects().first();
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

    const int ret = app.exec();

    engine.rootContext()->setContextProperty("deckA", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckB", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckC", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckD", static_cast<QObject*>(nullptr));

    if (masterBus)
        masterBus->unregisterCallback(DjEngine::getSharedAudioDeviceManager());
    masterBus.reset();

    deckD.reset();
    deckC.reset();
    deckB.reset();
    deckA.reset();

    DjEngine::shutdownSharedAudioDeviceManager();

    juce::MessageManager::deleteInstance();
    juce::DeletedAtShutdown::deleteAll();

    settingsManager.markCleanShutdown();
    settingsManager.shutdown();

    return ret;
}
