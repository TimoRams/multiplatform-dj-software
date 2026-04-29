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
#include <QtGlobal>

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
    // Enforce Vulkan on Linux/Windows.
    const QString rhiBackend = qEnvironmentVariable("QSG_RHI_BACKEND");
    if (rhiBackend.compare("vulkan", Qt::CaseInsensitive) != 0)
        qputenv("QSG_RHI_BACKEND", "vulkan");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);

    QGuiApplication app(argc, argv);

    // Set a global default font with proper hinting strategy
    QFont defaultFont = app.font();
    defaultFont.setHintingPreference(QFont::PreferFullHinting);
    defaultFont.setStyleStrategy(QFont::PreferAntialias);
    app.setFont(defaultFont);

    juce::ScopedJuceInitialiser_GUI juceInit;

    SettingsManager::getInstance().init();

    auto& settingsManager = SettingsManager::getInstance();

    auto deckA = std::make_unique<DjEngine>();
    auto deckB = std::make_unique<DjEngine>();

    deckA->applyAudioDeviceSettings(settingsManager.getAudioMasterDeviceType(),
                                    settingsManager.getAudioMasterOutputDevice(),
                                    settingsManager.getAudioSampleRate(),
                                    settingsManager.getAudioBufferSize(),
                                    settingsManager.getAudioMasterFirstChannel(),
                                    settingsManager.getAudioHeadphonesFirstChannel(),
                                    settingsManager.getAudioBoothFirstChannel());

    auto coverProvider = std::make_unique<CoverArtProvider>();
    deckA->setCoverArtProvider(coverProvider.get(), "deckA");
    deckB->setCoverArtProvider(coverProvider.get(), "deckB");

    QQmlApplicationEngine engine;

    ParameterStore parameterStore;
    MidiControllerManager midiManager(&parameterStore);

    engine.addImageProvider("coverart", coverProvider.release());

    engine.rootContext()->setContextProperty("settingsManager", &settingsManager);
    engine.rootContext()->setContextProperty("deckA", deckA.get());
    engine.rootContext()->setContextProperty("deckB", deckB.get());

    LibraryManager libraryManager;
    engine.rootContext()->setContextProperty("libraryManager", &libraryManager);

    LibraryDatabase libraryDb;
    if (!libraryDb.open())
        qWarning() << "[main] LibraryDatabase failed to open – library features disabled.";

    LibraryTableModel libraryTableModel("library_conn");
    libraryDb.setTableModel(&libraryTableModel);
    libraryTableModel.refresh();

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

    if (!engine.rootObjects().isEmpty()) {
        if (auto* rootWindow = qobject_cast<QWindow*>(engine.rootObjects().first()))
            rootWindow->show();
    }

    const int ret = app.exec();

    engine.rootContext()->setContextProperty("deckA", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckB", static_cast<QObject*>(nullptr));

    deckB.reset();
    deckA.reset();

    DjEngine::shutdownSharedAudioDeviceManager();

    juce::MessageManager::deleteInstance();
    juce::DeletedAtShutdown::deleteAll();

    return ret;
}
