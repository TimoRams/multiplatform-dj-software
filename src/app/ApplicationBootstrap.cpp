#include "ApplicationBootstrap.h"
#include "ApplicationLifecycle.h"
#include "platform/PosixSignalHandler.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>

// Qt Includes
#include <QByteArray>
#include <QGuiApplication>
#include <QLoggingCategory>
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
#include <QTemporaryDir>
#include <QtGlobal>

#include "deck/DjEngine.h"
#include "audio/AudioEngine.h"
#include "audio/TimeStretchProcessor.h"
#include "library/LibraryManager.h"
#include "library/devices/DeviceLibraryManager.h"
#include "library/MediaIoScheduler.h"
#include "library/LibraryCoverService.h"
#include "library/LibraryPreviewPlayer.h"
#include "fx/FxManager.h"
#include "link/LinkManager.h"
#include "platform/SystemMonitor.h"
#include "controllers/midi/ParameterStore.h"
#include "MixerControl.h"
#include "controllers/midi/MidiControllerManager.h"
#include "controllers/ControllerIntegrationManager.h"
#include "SettingsManager.h"
#include "library/LibraryDatabase.h"
#include "library/LibraryTableModel.h"
#include "library/LibraryAnalysisManager.h"
#include "app/CursorControl.h"
#include "audio/device/AudioDeviceService.h"
#include "audio/cache/AudioPageCache.h"
#include "deck/sync/DeckSync.h"
#include "app/ControlClock.h"
#include "app/UiPreferences.h"
#include "app/RenderPressurePolicy.h"

using namespace Qt::StringLiterals;

namespace {
std::uint64_t audioCacheBudgetBytesFromEnv()
{
    constexpr std::uint64_t kMinMb = 64;
    constexpr std::uint64_t kMaxMb = 2048;
    const auto installedMemoryMb = static_cast<std::uint64_t>(
        std::max(0, juce::SystemStats::getMemorySizeInMegabytes()));
    // A larger read-ahead cache prevents disk I/O bursts from competing with
    // audio on modern systems, but keeps the former 256 MiB default on
    // memory-constrained machines.
    const std::uint64_t defaultMb = installedMemoryMb >= 16ull * 1024ull
        ? 512 : 256;

    bool ok = false;
    const int configuredMb = qEnvironmentVariableIntValue("BROCKDJ_AUDIO_CACHE_MB", &ok);
    if (!ok || configuredMb <= 0)
        return defaultMb * 1024ull * 1024ull;

    const auto boundedMb = std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(configuredMb), kMinMb, kMaxMb);
    return boundedMb * 1024ull * 1024ull;
}

int audioRealtimePriorityFromEnv()
{
    bool ok = false;
    const int configured = qEnvironmentVariableIntValue("BROCKDJ_AUDIO_RT_PRIORITY", &ok);
    return platform::AudioThreadScheduling::normalizeRealtimePriority(
        ok ? configured : platform::AudioThreadScheduling::kDefaultRealtimePriority);
}

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
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    case QSGRendererInterface::Direct3D12: return "d3d12";
#endif
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        case QEvent::DevicePixelRatioChange:
#endif
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
    else {
        const QByteArray formatted = qFormatLogMessage(type, context, message).toLocal8Bit();
        std::fprintf(stderr, "%s\n", formatted.constData());
    }
}

#if defined(Q_OS_LINUX)
void configureLinuxVulkanBackend(bool& useVulkan,
                                 QString& requestedVkIcd)
{
    QString rhiBackend = qEnvironmentVariable("BROCKDJ_RHI_BACKEND").trimmed().toLower();
    if (rhiBackend.isEmpty())
        rhiBackend = qEnvironmentVariable("QSG_RHI_BACKEND").trimmed().toLower();
    if (rhiBackend.isEmpty()) {
#if defined(Q_PROCESSOR_ARM_64)
        rhiBackend = QStringLiteral("auto");
#else
        rhiBackend = QStringLiteral("vulkan");
#endif
    }

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
        qDebug() << "[startup] RHI backend delegated to Qt";
    } else {
        qWarning() << "[startup] Unknown RHI backend value; preserving Qt backend selection:"
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
    bool startupCloseSmoke = false;
    bool startupEarlyCloseSmoke = false;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QStringLiteral("--startup-close-smoke")
            || argument == QStringLiteral("--ci-startup-smoke-test")) {
            startupCloseSmoke = true;
            break;
        } else if (argument == QStringLiteral("--ci-startup-early-close-test")) {
            startupCloseSmoke = true;
            startupEarlyCloseSmoke = true;
            break;
        }
    }

    std::unique_ptr<QTemporaryDir> startupSmokeDirectory;
    if (startupCloseSmoke) {
        startupSmokeDirectory = std::make_unique<QTemporaryDir>();
        if (!startupSmokeDirectory->isValid()) {
            qCritical() << "[startup-smoke] Could not create isolated configuration directory";
            return 2;
        }
        const QString isolatedPath = startupSmokeDirectory->path();
        const QByteArray isolatedHome = isolatedPath.toLocal8Bit();
#if defined(Q_OS_WIN)
        qputenv("USERPROFILE", isolatedHome);
        qputenv("APPDATA", QDir(isolatedPath)
                                .filePath(QStringLiteral("AppData/Roaming")).toLocal8Bit());
#else
        qputenv("HOME", isolatedHome);
#endif
        qputenv("XDG_CONFIG_HOME", QDir(isolatedPath)
                                       .filePath(QStringLiteral("config")).toLocal8Bit());
        qputenv("XDG_DATA_HOME", QDir(isolatedPath)
                                     .filePath(QStringLiteral("data")).toLocal8Bit());
        qputenv("XDG_CACHE_HOME", QDir(isolatedPath)
                                      .filePath(QStringLiteral("cache")).toLocal8Bit());
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QUICK_BACKEND", "software");
        qunsetenv("QSG_RHI_BACKEND");
        qputenv("BROCKDJ_RHI_BACKEND", "auto");
    }

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

    const QtMessageHandler previousHandler =
        qInstallMessageHandler(filteredMessageHandler);
    if (previousHandler != filteredMessageHandler)
        g_previousMessageHandler = previousHandler;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    QQuickWindow::setTextRenderType(QQuickWindow::CurveTextRendering);
#else
    QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);
#endif

    configureQtRuntimeDefaults();

#if defined(Q_OS_LINUX)
    // Keep desktop Vulkan as the default; ARM64 delegates backend selection to
    // Qt unless an explicit BrockDJ or Qt backend override is present.
    configureLinuxVulkanBackend(useVulkan, requestedVkIcd);
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);

    // Must run before QGuiApplication; pairs with QT_QUICK_CONTROLS_STYLE above.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QGuiApplication app(argc, argv);
    const bool renderDiagnostics = renderDiagnosticsEnabled();
    if (renderDiagnostics) {
        QString requestedRhi = qEnvironmentVariable("BROCKDJ_RHI_BACKEND").trimmed();
        if (requestedRhi.isEmpty())
            requestedRhi = qEnvironmentVariable("QSG_RHI_BACKEND").trimmed();
        if (requestedRhi.isEmpty()) {
#if defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM_64)
            requestedRhi = QStringLiteral("auto");
#elif defined(Q_OS_LINUX)
            requestedRhi = QStringLiteral("vulkan");
#else
            requestedRhi = QStringLiteral("Qt default");
#endif
        }
        qInfo() << "[render-diagnostics] Qt=" << qVersion()
                << "platform=" << QGuiApplication::platformName()
                << "session=" << qEnvironmentVariable("XDG_SESSION_TYPE")
                << "requestedRhi=" << requestedRhi
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
    runtime.libraryTableModel = std::make_unique<LibraryTableModel>();
    runtime.libraryAnalysisManager = std::make_unique<LibraryAnalysisManager>();
    runtime.deviceLibraryManager = std::make_unique<DeviceLibraryManager>(
        !startupCloseSmoke, nullptr);
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
    const auto audioCacheBudgetBytes = audioCacheBudgetBytesFromEnv();
    runtime.audioPageCache = std::make_unique<AudioPageCache>(audioCacheBudgetBytes);
    qInfo() << "[startup] Audio page cache budget:" << (audioCacheBudgetBytes / (1024ull * 1024ull))
            << "MB";
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
    QObject::connect(runtime.audioDeviceService.get(), &AudioDeviceService::errorChanged,
                     &app, [&runtime]() {
                         if (runtime.rootObjectForStartup && runtime.audioDeviceService)
                             runtime.rootObjectForStartup->setProperty(
                                 "startupAudioError", runtime.audioDeviceService->lastError());
                     });
    runtime.renderPressurePolicy = std::make_unique<RenderPressurePolicy>(
        *runtime.controlClock, *runtime.audioDeviceService);
    QObject::connect(runtime.audioDeviceService.get(), &AudioDeviceService::configurationChanged,
                     &app, [&runtime, &app] {
                         QTimer::singleShot(50, &app, [&runtime] {
                             if (runtime.stopping || !runtime.audioEngine
                                 || !runtime.audioDeviceService
                                 || !runtime.audioDeviceService->manager().getCurrentAudioDevice()) {
                                 return;
                             }

                             AudioEngine::requestRealtimeThreadScheduling(
                                 audioRealtimePriorityFromEnv());
                             const auto status = AudioEngine::realtimeThreadSchedulingStatus();
                             switch (status.state) {
                             case platform::AudioThreadSchedulingState::Active:
                             case platform::AudioThreadSchedulingState::AlreadyRealtime:
                                 qInfo() << "[audio] realtime scheduling"
                                         << platform::audioThreadSchedulingStateName(status.state)
                                         << "priority=" << status.priority;
                                 break;
                             case platform::AudioThreadSchedulingState::PermissionDenied:
                                 qWarning() << "[audio] realtime scheduling unavailable; configure"
                                            << "RLIMIT_RTPRIO/CAP_SYS_NICE for BrockDJ. errno="
                                            << status.nativeError;
                                 break;
                             case platform::AudioThreadSchedulingState::WaitingForCallback:
                                 qWarning() << "[audio] realtime scheduling deferred;"
                                            << "the device callback has not started";
                                 break;
                             case platform::AudioThreadSchedulingState::Failed:
                                 qWarning() << "[audio] realtime scheduling request failed. errno="
                                            << status.nativeError;
                                 break;
                             case platform::AudioThreadSchedulingState::Unsupported:
                                 break;
                             }
                         });
                     });
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
    const auto applyRuntimeBackgroundProfile = [&runtime](Qt::ApplicationState state) {
        // Keep realtime control/display ticks alive while unfocused. Hard
        // throttling is only for truly hidden/suspended app states.
        const bool backgroundMode = state == Qt::ApplicationHidden
            || state == Qt::ApplicationSuspended;
        if (runtime.controlClock)
            runtime.controlClock->setBackgroundMode(backgroundMode);
        if (runtime.renderPressurePolicy)
            runtime.renderPressurePolicy->setApplicationActive(!backgroundMode);
        const bool throttleBackgroundWork = backgroundMode
            || (runtime.renderPressurePolicy
                && !runtime.renderPressurePolicy->waveformRasterWorkEnabled());
        for (DjEngine* deck : {runtime.deckA.get(), runtime.deckB.get(),
                               runtime.deckC.get(), runtime.deckD.get()}) {
            if (deck)
                deck->setBackgroundOptimizationEnabled(throttleBackgroundWork);
        }
    };
    QObject::connect(&app, &QGuiApplication::applicationStateChanged,
                     &app, applyRuntimeBackgroundProfile);
    applyRuntimeBackgroundProfile(app.applicationState());
    QObject::connect(runtime.renderPressurePolicy.get(),
                     &RenderPressurePolicy::tierChanged,
                     &app,
                     [&app, applyRuntimeBackgroundProfile]() {
                         applyRuntimeBackgroundProfile(app.applicationState());
                     });

    engine.addImageProvider("coverart", runtime.coverProvider.release());
    logStartupStep("Cover art provider installed");

    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::SettingsManager, &settingsManager);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::AppConfig, &appConfig);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::DeckA, nullptr);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::DeckB, nullptr);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::DeckC, nullptr);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::DeckD, nullptr);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::LibraryManager, runtime.libraryManager.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::LibraryDatabase, nullptr);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::LibraryModel, runtime.libraryTableModel.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::LibraryAnalyzer, runtime.libraryAnalysisManager.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::DeviceLibraryManager, runtime.deviceLibraryManager.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::FxManager, runtime.fxManager.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::LinkManager, runtime.linkManager.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::SystemMonitor, runtime.sysMonitor.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::ParameterStore, runtime.parameterStore.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::MidiManager, nullptr);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::ControllerManager, nullptr);
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::CursorControl, runtime.cursorControl.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::UiScaleController, runtime.uiScaleController.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::WaveformZoomController, runtime.waveformZoomController.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::RenderPressurePolicy, runtime.renderPressurePolicy.get());
    qmlRegisterSingletonInstance("BrockDJ.Mixer", 1, 0, "Control", runtime.mixerControl.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::MixerControl, runtime.mixerControl.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::ControlClock, runtime.controlClock.get());
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::LibraryCover, runtime.libraryCoverService.get());

    const auto url = QUrl(u"qrc:/DJSoftware/src/qml/main.qml"_s);
    bool startupSmokeReady = false;
    std::atomic_bool startupSoftwareFrameRendered{false};
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    auto initialiseRuntime = [&]() {
        if (runtime.runtimeInitStarted || runtime.stopping)
            return;
        if (startupCloseSmoke
            && !startupSoftwareFrameRendered.load(std::memory_order_acquire)) {
            qCritical() << "[startup-smoke] Qt Quick did not render a software frame";
            app.exit(2);
            return;
        }
        runtime.runtimeInitStarted = true;

        if (!runtime.libraryDb->open())
            qWarning() << "[main] LibraryDatabase failed to open – library features disabled.";
        logStartupStep("LibraryDatabase open attempted");

        runtime.libraryDb->setTableModel(runtime.libraryTableModel.get());

        QObject::connect(runtime.libraryDb.get(), &LibraryDatabase::trackMetaChanged,
                         runtime.libraryTableModel.get(), &LibraryTableModel::refreshMetaForTrack);

        runtime.libraryAnalysisManager->setLibraryDatabase(runtime.libraryDb.get());
        ApplicationLifecycle::setQmlContextProperty(
            engine, QmlContextProperty::LibraryDatabase, runtime.libraryDb.get());

        runtime.libraryTableModel->refresh();
        logStartupStep("LibraryTableModel refreshed");

        if (runtime.rootObjectForStartup)
            runtime.rootObjectForStartup->setProperty("startupLibraryReady", true);

        QTimer::singleShot(0, &app, [&]() {
            if (runtime.stopping)
                return;
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
            applyRuntimeBackgroundProfile(app.applicationState());
            logStartupStep("DjEngines constructed");

            for (const auto [deck, name] : std::array<std::pair<DjEngine*, const char*>, 4>{{
                    {runtime.deckA.get(), "deckA"}, {runtime.deckB.get(), "deckB"},
                    {runtime.deckC.get(), "deckC"}, {runtime.deckD.get(), "deckD"}}})
                deck->setCoverArtProvider(runtime.coverProviderPtr, name);

            ApplicationLifecycle::setQmlContextProperty(
                engine, QmlContextProperty::DeckA, runtime.deckA.get());
            ApplicationLifecycle::setQmlContextProperty(
                engine, QmlContextProperty::DeckB, runtime.deckB.get());
            ApplicationLifecycle::setQmlContextProperty(
                engine, QmlContextProperty::DeckC, runtime.deckC.get());
            ApplicationLifecycle::setQmlContextProperty(
                engine, QmlContextProperty::DeckD, runtime.deckD.get());

            if (!startupCloseSmoke) {
                auto* midi = new MidiControllerManager(runtime.parameterStore.get(),
                                                       *runtime.controlClock, &app);
                QQmlEngine::setObjectOwnership(midi, QQmlEngine::CppOwnership);
                runtime.midiManager = midi;
                runtime.midiManager->connectDecks(
                    runtime.deckA.get(), runtime.deckB.get(),
                    runtime.deckC.get(), runtime.deckD.get());
                ApplicationLifecycle::setQmlContextProperty(
                    engine, QmlContextProperty::MidiManager, runtime.midiManager.data());

                runtime.controllerManager = std::make_unique<ControllerIntegrationManager>(
                    *runtime.controlClock);
                runtime.controllerManager->setDecks(runtime.deckA.get(), runtime.deckB.get());
                ApplicationLifecycle::setQmlContextProperty(
                    engine, QmlContextProperty::ControllerManager, runtime.controllerManager.get());
                QObject::connect(&settingsManager,
                                 &SettingsManager::controllerSettingsChanged,
                                 runtime.controllerManager.get(),
                                 [&settingsManager, controller = runtime.controllerManager.get()] {
                                     controller->setFlx10Enabled(
                                         settingsManager.flx10ControllerSupportEnabled());
                                 });
                runtime.controllerManager->setFlx10Enabled(
                    settingsManager.flx10ControllerSupportEnabled());
            } else {
                qInfo() << "[startup-smoke] MIDI and controller hardware discovery skipped";
            }

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
            if (runtime.midiManager)
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
            ApplicationLifecycle::setQmlContextProperty(
                engine, QmlContextProperty::LibraryPreview, runtime.libraryPreviewPlayer.get());
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
            const bool audioSettingsApplied = startupCloseSmoke
                || runtime.deckA->applyAudioDeviceSettings(
                    preferredAudioType,
                    preferredAudioOutput,
                    settingsManager.getAudioSampleRate(),
                    settingsManager.getAudioBufferSize(),
                    settingsManager.getAudioMasterFirstChannel(),
                    settingsManager.getAudioHeadphonesFirstChannel(),
                    settingsManager.getAudioBoothFirstChannel());

            if (startupCloseSmoke) {
                qInfo() << "[startup-smoke] Audio hardware initialization skipped";
            } else if (audioSettingsApplied
                       && !runtime.audioDeviceService->currentOutputDevice().trimmed().isEmpty()) {
                qDebug() << "[startup] Audio preference restored:"
                         << "preferred=" << preferredAudioType << "/" << preferredAudioOutput
                         << "active=" << runtime.audioDeviceService->currentDeviceType()
                         << "/" << runtime.audioDeviceService->currentOutputDevice();
            } else {
                const QString audioError = runtime.audioDeviceService->lastError().isEmpty()
                    ? QStringLiteral("No audio output device is active.")
                    : runtime.audioDeviceService->lastError();
                if (runtime.rootObjectForStartup)
                    runtime.rootObjectForStartup->setProperty("startupAudioError", audioError);
                qWarning() << "[startup] Audio preference could not be restored:"
                           << preferredAudioType << "/" << preferredAudioOutput
                           << audioError;

                // Some Linux audio backends become enumerable shortly after the
                // GUI is ready. Retry a bounded number of times; never poll or
                // replace the user's preferred device with a fallback.
                for (const int delayMs : {750, 2500}) {
                    QTimer::singleShot(delayMs, &app, [&runtime, &settingsManager, delayMs]() {
                        if (runtime.stopping || !runtime.audioDeviceService || !runtime.deckA
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
                        if (restored && !runtime.audioDeviceService->currentOutputDevice()
                                              .trimmed().isEmpty()) {
                            if (runtime.rootObjectForStartup)
                                runtime.rootObjectForStartup->setProperty("startupAudioError", QString());
                            qDebug() << "[startup] Audio preference restored on retry"
                                     << delayMs << "ms:"
                                     << runtime.audioDeviceService->currentDeviceType()
                                     << "/" << runtime.audioDeviceService->currentOutputDevice();
                        } else {
                            const QString audioError = runtime.audioDeviceService->lastError().isEmpty()
                                ? QStringLiteral("No audio output device is active.")
                                : runtime.audioDeviceService->lastError();
                            if (runtime.rootObjectForStartup)
                                runtime.rootObjectForStartup->setProperty(
                                    "startupAudioError", audioError);
                            qWarning() << "[startup] Audio preference retry failed after"
                                       << delayMs << "ms:"
                                       << audioError;
                        }
                    });
                }
            }

            runtime.controlClock->start();
            qDebug() << "[startup] Audio device setup finished" << startupTimer.elapsed() << "ms";
            if (runtime.rootObjectForStartup)
                runtime.rootObjectForStartup->setProperty("startupReady", true);
            if (startupCloseSmoke) {
                startupSmokeReady = true;
                qInfo() << "[startup-smoke] Runtime ready; closing initialized root";
                if (!startupEarlyCloseSmoke) {
                    QTimer::singleShot(0, &app, [&runtime, &app] {
                        auto* window = qobject_cast<QWindow*>(
                            runtime.rootObjectForStartup.data());
                        if (!window) {
                            qCritical() << "[startup-smoke] Initialized root is not a window";
                            app.exit(2);
                            return;
                        }
                        window->setProperty("allowDirectClose", true);
                        if (!window->close()) {
                            qCritical() << "[startup-smoke] Initialized root rejected close";
                            app.exit(2);
                            return;
                        }
                        qInfo() << "[startup-smoke] Initialized root close accepted";
                    });
                }
            }
        });
    };

    QTimer startupSmokeTimeout;
    if (startupCloseSmoke) {
        startupSmokeTimeout.setSingleShot(true);
        QObject::connect(&startupSmokeTimeout, &QTimer::timeout, &app, [&app]() {
            qCritical() << "[startup-smoke] Startup/close path exceeded its deadline";
            app.exit(2);
        });
        startupSmokeTimeout.start(startupEarlyCloseSmoke ? 10000 : 30000);
    }

    engine.load(url);
    logStartupStep("QML load requested");

    if (!engine.rootObjects().isEmpty()) {
        runtime.rootObjectForStartup = engine.rootObjects().first();
        if (auto* rootWindow = qobject_cast<QWindow*>(engine.rootObjects().first())) {
            qDebug() << "[main] Root window found, setting size and visibility";

            if (auto* quickWindow = qobject_cast<QQuickWindow*>(rootWindow)) {
                QObject::connect(quickWindow, &QWindow::windowStateChanged, &app,
                                 [&runtime](Qt::WindowState state) {
                                     if (runtime.renderPressurePolicy) {
                                         runtime.renderPressurePolicy->setWindowMinimized(
                                             state == Qt::WindowMinimized);
                                     }
                                 });
                runtime.renderPressurePolicy->setWindowMinimized(
                    quickWindow->windowState() == Qt::WindowMinimized);

                const bool usingVulkan = (QQuickWindow::graphicsApi() == QSGRendererInterface::Vulkan);
                if (usingVulkan) {
                    // The application has no Qt Quick 3D content. Avoiding the
                    // otherwise unused depth attachment saves shared iGPU memory
                    // bandwidth and a Vulkan render-target transition per frame.
                    QQuickGraphicsConfiguration cfg = quickWindow->graphicsConfiguration();
                    cfg.setDepthBufferFor2D(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
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

                        cfg.setPipelineCacheSaveFile(cacheFile);
                        if (!resetCache)
                            cfg.setPipelineCacheLoadFile(cacheFile);

                        QFileInfo cacheInfo(cacheFile);
                        if (cacheInfo.exists())
                            qDebug() << "[main] Vulkan pipeline cache:" << cacheFile << cacheInfo.size() << "bytes";
                        else
                            qDebug() << "[main] Vulkan pipeline cache (new):" << cacheFile;
                    } else {
                        qDebug() << "[main] Vulkan pipeline cache disabled (BROCKDJ_VK_CACHE=0)";
                    }
#endif
                    quickWindow->setGraphicsConfiguration(cfg);
                }

                QObject::connect(
                    quickWindow,
                    &QQuickWindow::sceneGraphInitialized,
                    &app,
                    [quickWindow, &startupTimer, renderDiagnostics]() {
                    const auto* renderer = quickWindow->rendererInterface();
                    const auto graphicsApi = renderer
                        ? renderer->graphicsApi() : QSGRendererInterface::Unknown;
                    qInfo() << "[startup] Scene graph initialized"
                            << startupTimer.elapsed() << "ms"
                            << "backend=" << graphicsApiName(graphicsApi);
                    if (renderDiagnostics) {
                        qInfo() << "[render-diagnostics] scene graph initialized"
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
                    [&app, &startupTimer](QQuickWindow::SceneGraphError error,
                                          const QString& message) {
                    qWarning() << "[startup] Scene graph error" << error << message
                               << "at" << startupTimer.elapsed() << "ms";
                    QMetaObject::invokeMethod(&app, [&app] { app.exit(-1); },
                                              Qt::QueuedConnection);
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
                    [&startupTimer, quickWindow, startupCloseSmoke,
                     &startupSoftwareFrameRendered, &app]() {
                    qDebug() << "[startup] FIRST FRAME RENDERED" << startupTimer.elapsed() << "ms";
                    if (startupCloseSmoke) {
                        const auto* renderer = quickWindow->rendererInterface();
                        if (!renderer
                            || renderer->graphicsApi() != QSGRendererInterface::Software) {
                            qCritical() << "[startup-smoke] First frame used a non-software backend:"
                                        << graphicsApiName(renderer
                                            ? renderer->graphicsApi()
                                            : QSGRendererInterface::Unknown);
                            QMetaObject::invokeMethod(&app, [&app] { app.exit(2); },
                                                      Qt::QueuedConnection);
                            return;
                        }
                        startupSoftwareFrameRendered.store(true, std::memory_order_release);
                    }
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

            if (startupEarlyCloseSmoke) {
                runtime.stopping = true;
                rootWindow->setProperty("allowDirectClose", true);
                QTimer::singleShot(0, &app, [rootWindow, &runtime, &app] {
                    if (!rootWindow->close()) {
                        qCritical() << "[startup-smoke] Early root close was rejected";
                        app.exit(2);
                        return;
                    }
                    qInfo() << "[startup-smoke] Early root close accepted";
                });
            }

#if defined(Q_OS_MACOS)
            // macOS requires extra steps to properly show the window
            rootWindow->raise();
            rootWindow->requestActivate();
            qDebug() << "[main] macOS: Window raised and activated";
#endif
        } else {
            qCritical() << "[main] Root object is not a QWindow!";
            ApplicationLifecycle::shutdownApplication(runtime);
            return -1;
        }
    } else {
        qCritical() << "[main] No root objects found after loading QML!";
        ApplicationLifecycle::shutdownApplication(runtime);
        return -1;
    }

    auto* exitGate = new AppExitGate(&app);
    runtime.exitGate = exitGate;
    exitGate->setHandler([&runtime](bool manualBackup) {
        ApplicationLifecycle::performExitTeardown(runtime, manualBackup);
        QCoreApplication::quit();
    });
    ApplicationLifecycle::setQmlContextProperty(
        engine, QmlContextProperty::AppExit, exitGate);

    const int ret = app.exec();
    startupSmokeTimeout.stop();
    ApplicationLifecycle::shutdownApplication(runtime);

    if (startupCloseSmoke && !startupEarlyCloseSmoke && !startupSmokeReady && ret == 0)
        return 2;
    return ret;
}
