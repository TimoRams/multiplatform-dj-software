#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <iostream>
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
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQuickGraphicsConfiguration>
#include <QtGlobal>
#include <QVersionNumber>
#include <QVulkanInstance>

#include "DjEngine.h"
#include "WaveformItem.h"
#include "library/LibraryManager.h"
#include "library/CoverArtProvider.h"
#include "fx/FxManager.h"
#include "link/LinkManager.h"
#include "SystemMonitor.h"
#include "midi/ParameterStore.h"
#include "midi/MidiControllerManager.h"
#include "SettingsManager.h"
#include "library/LibraryDatabase.h"
#include "library/LibraryTableModel.h"

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

    std::cout << "========================================" << std::endl;
    std::cout << "RAMSBROCK DJ ENGINE - INITIAL BUILD TEST" << std::endl;
    std::cout << "JUCE Version:   " << juce::SystemStats::getJUCEVersion() << std::endl;
    std::cout << "C++ Standard:   " << __cplusplus << std::endl;
    std::cout << "========================================" << std::endl;

    qDebug() << "Essentia disabled by project policy; using internal analysis pipeline.";

    g_previousMessageHandler = qInstallMessageHandler(filteredMessageHandler);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    QQuickWindow::setTextRenderType(QQuickWindow::CurveTextRendering);
#else
    QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);
#endif

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR_ROUNDING_POLICY"))
        qputenv("QT_SCALE_FACTOR_ROUNDING_POLICY", "RoundPreferFloor");

    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES")) {
        qputenv("QT_LOGGING_RULES",
                "qt.scenegraph.general=false;"
                "qt.rhi.general=false");
    }

#if !defined(Q_OS_MACOS)
    // Vulkan is required for production; allow env override for diagnostics.
    QString rhiBackend = qEnvironmentVariable("RAMSBROCKDJ_RHI_BACKEND").trimmed().toLower();
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
        qDebug() << "[startup] RHI backend auto (RAMSBROCKDJ_RHI_BACKEND=auto)";
    } else {
        qWarning() << "[startup] Unknown RAMSBROCKDJ_RHI_BACKEND value, forcing vulkan:" << rhiBackend;
        qputenv("QSG_RHI_BACKEND", "vulkan");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        useVulkan = true;
    }

    if (useVulkan) {
        requestedVkIcd = qEnvironmentVariable("RAMSBROCKDJ_VK_ICD").trimmed();
        if (requestedVkIcd.isEmpty() && qEnvironmentVariableIsEmpty("VK_ICD_FILENAMES")) {
            const QString autoMode = qEnvironmentVariable("RAMSBROCKDJ_VK_ICD_AUTO").trimmed().toLower();
            const bool allowAuto = autoMode.isEmpty() || autoMode == "1" || autoMode == "true" || autoMode == "on";
            if (allowAuto)
                requestedVkIcd = pickDefaultVulkanIcd();
        }

        if (!requestedVkIcd.isEmpty())
            qputenv("VK_ICD_FILENAMES", requestedVkIcd.toUtf8());

        requestedVkApiRaw = qEnvironmentVariable("RAMSBROCKDJ_VK_API").trimmed();
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

    std::unique_ptr<QVulkanInstance> vkInstance;
    if (useVulkan) {
        if (!requestedVkIcd.isEmpty())
            qDebug() << "[startup] Vulkan ICD override:" << requestedVkIcd;

        if (!requestedVkApiRaw.isEmpty()) {
            if (!requestedVkApi.isNull())
                qDebug() << "[startup] Vulkan API override:" << requestedVkApi;
            else
                qWarning() << "[startup] Invalid RAMSBROCKDJ_VK_API value:" << requestedVkApiRaw;
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

    // Load app icon from embedded QRC (generated by scripts/generate_icons.sh).
    // Multiple sizes let Qt pick the sharpest one for each use (taskbar, title bar, dock).
    {
        QIcon appIcon;
        for (int sz : {16, 32, 48, 64, 128, 256, 512}) {
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

    auto deckA = std::make_unique<DjEngine>();
    auto deckB = std::make_unique<DjEngine>();
    logStartupStep("DjEngines constructed");

    deckA->applyAudioDeviceSettings(settingsManager.getAudioMasterDeviceType(),
                                    settingsManager.getAudioMasterOutputDevice(),
                                    settingsManager.getAudioSampleRate(),
                                    settingsManager.getAudioBufferSize(),
                                    settingsManager.getAudioMasterFirstChannel(),
                                    settingsManager.getAudioHeadphonesFirstChannel(),
                                    settingsManager.getAudioBoothFirstChannel());
    logStartupStep("Audio device settings applied");

    auto coverProvider = std::make_unique<CoverArtProvider>();
    deckA->setCoverArtProvider(coverProvider.get(), "deckA");
    deckB->setCoverArtProvider(coverProvider.get(), "deckB");

    QQmlApplicationEngine engine;

    ParameterStore parameterStore;
    MidiControllerManager midiManager(&parameterStore);

    // Route MIDI parameter changes to the appropriate deck controls.
    // Volume and crossfader are handled by MixerSection.qml via parameterStore.
    // Button events: value > 0 = press/on, value == 0 = release/off.
    QObject::connect(&parameterStore, &ParameterStore::parameterChanged,
        [deckAPtr = deckA.get(), deckBPtr = deckB.get()](const QString& id, float value)
    {
        if (id == "deckA_play") { if (value > 0.0f) deckAPtr->togglePlay(); }
        else if (id == "deckB_play") { if (value > 0.0f) deckBPtr->togglePlay(); }
        else if (id == "deckA_cue") {
            if (value > 0.0f) deckAPtr->cueButtonPress();
            else              deckAPtr->cueButtonRelease();
        }
        else if (id == "deckB_cue") {
            if (value > 0.0f) deckBPtr->cueButtonPress();
            else              deckBPtr->cueButtonRelease();
        }
        // Trim: MIDI 0-1 → engine 0-2 (center = 1.0 = unity)
        else if (id == "deckA_gain")   deckAPtr->setTrim(static_cast<double>(value) * 2.0);
        else if (id == "deckB_gain")   deckBPtr->setTrim(static_cast<double>(value) * 2.0);
        // EQ/filter: MIDI 0-1 → engine -1 to +1 (center = 0.0)
        else if (id == "deckA_eqHigh") deckAPtr->setEqHigh(static_cast<double>(value) * 2.0 - 1.0);
        else if (id == "deckB_eqHigh") deckBPtr->setEqHigh(static_cast<double>(value) * 2.0 - 1.0);
        else if (id == "deckA_eqMid")  deckAPtr->setEqMid(static_cast<double>(value) * 2.0 - 1.0);
        else if (id == "deckB_eqMid")  deckBPtr->setEqMid(static_cast<double>(value) * 2.0 - 1.0);
        else if (id == "deckA_eqLow")  deckAPtr->setEqLow(static_cast<double>(value) * 2.0 - 1.0);
        else if (id == "deckB_eqLow")  deckBPtr->setEqLow(static_cast<double>(value) * 2.0 - 1.0);
        else if (id == "deckA_filter") deckAPtr->setFilter(static_cast<double>(value) * 2.0 - 1.0);
        else if (id == "deckB_filter") deckBPtr->setFilter(static_cast<double>(value) * 2.0 - 1.0);
        // Tempo fader: MIDI 0-1 → engine -8% to +8%
        else if (id == "deckA_tempo")  deckAPtr->setTempoPercent(static_cast<double>(value) * 16.0 - 8.0);
        else if (id == "deckB_tempo")  deckBPtr->setTempoPercent(static_cast<double>(value) * 16.0 - 8.0);
    });

    engine.addImageProvider("coverart", coverProvider.release());
    logStartupStep("Cover art provider installed");

    engine.rootContext()->setContextProperty("settingsManager", &settingsManager);
    engine.rootContext()->setContextProperty("deckA", deckA.get());
    engine.rootContext()->setContextProperty("deckB", deckB.get());

    LibraryManager libraryManager;
    engine.rootContext()->setContextProperty("libraryManager", &libraryManager);

    LibraryDatabase libraryDb;
    if (!libraryDb.open())
        qWarning() << "[main] LibraryDatabase failed to open – library features disabled.";
    logStartupStep("LibraryDatabase open attempted");

    LibraryTableModel libraryTableModel("library_conn");
    libraryDb.setTableModel(&libraryTableModel);
    libraryTableModel.refresh();
    logStartupStep("LibraryTableModel refreshed");

    engine.rootContext()->setContextProperty("libraryDb",    &libraryDb);
    engine.rootContext()->setContextProperty("libraryModel", &libraryTableModel);

    deckA->setLibraryDatabase(&libraryDb);
    deckB->setLibraryDatabase(&libraryDb);

    FxManager fxManager;
    fxManager.registerEngines(deckA.get(), deckB.get());
    engine.rootContext()->setContextProperty("fxManager", &fxManager);

    LinkManager linkManager;
    engine.rootContext()->setContextProperty("linkManager", &linkManager);

    SystemMonitor sysMonitor;
    engine.rootContext()->setContextProperty("sysMonitor", &sysMonitor);

    engine.rootContext()->setContextProperty("parameterStore", &parameterStore);
    engine.rootContext()->setContextProperty("midiManager", &midiManager);

    const auto url = QUrl(u"qrc:/DJSoftware/src/qml/main.qml"_s);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(url);
    logStartupStep("QML load requested");

    if (!engine.rootObjects().isEmpty()) {
        if (auto* rootWindow = qobject_cast<QWindow*>(engine.rootObjects().first())) {
            qDebug() << "[main] Root window found, setting size and visibility";

            if (auto* quickWindow = qobject_cast<QQuickWindow*>(rootWindow)) {
                const bool usingVulkan = (QQuickWindow::graphicsApi() == QSGRendererInterface::Vulkan);
                if (usingVulkan && vkInstance)
                    quickWindow->setVulkanInstance(vkInstance.get());
                if (usingVulkan) {
                    const QString cacheMode = qEnvironmentVariable("RAMSBROCKDJ_VK_CACHE").trimmed().toLower();
                    const bool enableCache = cacheMode.isEmpty() || cacheMode == "1" || cacheMode == "on" || cacheMode == "true";
                    const bool resetCache = (cacheMode == "reset");

                    if (enableCache) {
                        // Set up Vulkan pipeline cache before show() so compiled shaders are persisted.
                        // First launch compiles everything; subsequent launches load in milliseconds.
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
                        qDebug() << "[main] Vulkan pipeline cache disabled (RAMSBROCKDJ_VK_CACHE=0)";
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
        settingsManager.shutdown();
        return -1;
    }

    const int ret = app.exec();

    settingsManager.shutdown();
    engine.rootContext()->setContextProperty("deckA", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckB", static_cast<QObject*>(nullptr));

    deckB.reset();
    deckA.reset();

    DjEngine::shutdownSharedAudioDeviceManager();

    juce::MessageManager::deleteInstance();
    juce::DeletedAtShutdown::deleteAll();

    return ret;
}
