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

#include "DjEngine.h"
#include "WaveformItem.h"
#include "LibraryManager.h"
#include "CoverArtProvider.h"
#include "FxManager.h"

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    std::cout << "========================================" << std::endl;
    std::cout << "RAMSBROCK DJ ENGINE - INITIAL BUILD TEST" << std::endl;
    std::cout << "JUCE Version:   " << juce::SystemStats::getJUCEVersion() << std::endl;
    std::cout << "C++ Standard:   " << __cplusplus << std::endl;
    std::cout << "========================================" << std::endl;

    // ── Global text rendering quality ────────────────────────────────────────
    // Qt's built-in curve renderer (signed-distance-field) scales perfectly
    // at any size and works well with Vulkan.  NativeTextRendering can look
    // blurry on Linux + Vulkan, so we explicitly use CurveTextRendering.
    QQuickWindow::setTextRenderType(QQuickWindow::CurveTextRendering);

    qputenv("QSG_INFO", "1");

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);

    QGuiApplication app(argc, argv);

    // Set a global default font with proper hinting strategy
    QFont defaultFont = app.font();
    defaultFont.setHintingPreference(QFont::PreferFullHinting);
    defaultFont.setStyleStrategy(QFont::PreferAntialias);
    app.setFont(defaultFont);

    juce::MessageManager::getInstance();

    // DjEngines on the heap so we control their destruction order explicitly.
    auto deckA = std::make_unique<DjEngine>();
    auto deckB = std::make_unique<DjEngine>();

    // Cover art provider is owned by the QML engine after addImageProvider().
    auto* coverProvider = new CoverArtProvider();
    deckA->setCoverArtProvider(coverProvider, "deckA");
    deckB->setCoverArtProvider(coverProvider, "deckB");

    QQmlApplicationEngine engine;

    engine.addImageProvider("coverart", coverProvider);

    engine.rootContext()->setContextProperty("deckA", deckA.get());
    engine.rootContext()->setContextProperty("deckB", deckB.get());

    LibraryManager libraryManager;
    engine.rootContext()->setContextProperty("libraryManager", &libraryManager);

    FxManager fxManager;
    fxManager.registerEngines(deckA.get(), deckB.get());
    engine.rootContext()->setContextProperty("fxManager", &fxManager);

    const QUrl url(u"qrc:/DJSoftware/src/qml/main.qml"_s);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(url);

    int ret = app.exec();

    // ── Shutdown order (critical for avoiding JUCE assertion failures) ────────
    // 1. Tear down the QML scene first — it holds raw pointers to the DjEngines
    //    via context properties.  After this, no QML binding can touch them.
    // 2. Destroy the DjEngines — each one tears down its AudioDeviceManager,
    //    AudioTransportSource, WaveformAnalyzer etc. while the JUCE
    //    MessageManager is still alive (required for clean device shutdown).
    // 3. Clean up JUCE singletons last.
    engine.rootContext()->setContextProperty("deckA", static_cast<QObject*>(nullptr));
    engine.rootContext()->setContextProperty("deckB", static_cast<QObject*>(nullptr));

    deckB.reset();
    deckA.reset();

    juce::MessageManager::deleteInstance();
    juce::DeletedAtShutdown::deleteAll();

    return ret;
}
