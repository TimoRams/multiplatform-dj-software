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
#include "LibraryManager.h"
#include "CoverArtProvider.h"
#include "FxManager.h"
#include "link/LinkManager.h"
#include "SystemMonitor.h"
#include "ParameterStore.h"
#include "MidiControllerManager.h"
#include "SettingsManager.h"
#include "LibraryDatabase.h"
#include "LibraryTableModel.h"

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
    QQuickWindow::setTextRenderType(QQuickWindow::CurveTextRendering);

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR_ROUNDING_POLICY"))
        qputenv("QT_SCALE_FACTOR_ROUNDING_POLICY", "RoundPreferFloor");

    qputenv("QSG_INFO", "1");

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);

    QGuiApplication app(argc, argv);

    // Set a global default font with proper hinting strategy
    QFont defaultFont = app.font();
    defaultFont.setHintingPreference(QFont::PreferFullHinting);
    defaultFont.setStyleStrategy(QFont::PreferAntialias);
    app.setFont(defaultFont);

    juce::ScopedJuceInitialiser_GUI juceInit;

    SettingsManager::getInstance().init();

    auto deckA = std::make_unique<DjEngine>();
    auto deckB = std::make_unique<DjEngine>();

    auto coverProvider = std::make_unique<CoverArtProvider>();
    deckA->setCoverArtProvider(coverProvider.get(), "deckA");
    deckB->setCoverArtProvider(coverProvider.get(), "deckB");

    QQmlApplicationEngine engine;

    ParameterStore parameterStore;
    MidiControllerManager midiManager(&parameterStore);

    engine.addImageProvider("coverart", coverProvider.release());

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

    const int ret = app.exec();

    engine.rootContext()->setContextProperty("deckA", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckB", static_cast<QObject*>(nullptr));

    deckB.reset();
    deckA.reset();

    juce::MessageManager::deleteInstance();
    juce::DeletedAtShutdown::deleteAll();

    return ret;
}
