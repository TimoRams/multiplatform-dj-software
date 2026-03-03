#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <iostream>

// Qt Includes
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QQmlContext>

#include "DjEngine.h"
#include "WaveformItem.h"
#include "LibraryManager.h"
#include "CoverArtProvider.h"

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    std::cout << "========================================" << std::endl;
    std::cout << "RAMSBROCK DJ ENGINE - INITIAL BUILD TEST" << std::endl;
    std::cout << "JUCE Version: " << juce::SystemStats::getJUCEVersion() << std::endl;
    std::cout << "========================================" << std::endl;

    qputenv("QSG_INFO", "1");

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);

    QGuiApplication app(argc, argv);

    juce::MessageManager::getInstance();

    DjEngine deckA;
    DjEngine deckB;

    // Cover art provider is owned by the QML engine after addImageProvider().
    auto* coverProvider = new CoverArtProvider();
    deckA.setCoverArtProvider(coverProvider, "deckA");
    deckB.setCoverArtProvider(coverProvider, "deckB");

    QQmlApplicationEngine engine;

    engine.addImageProvider("coverart", coverProvider);

    engine.rootContext()->setContextProperty("deckA", &deckA);
    engine.rootContext()->setContextProperty("deckB", &deckB);

    LibraryManager libraryManager;
    engine.rootContext()->setContextProperty("libraryManager", &libraryManager);

    const QUrl url(u"qrc:/DJSoftware/src/main.qml"_s);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(url);

    int ret = app.exec();

    juce::MessageManager::deleteInstance();
    juce::DeletedAtShutdown::deleteAll();

    return ret;
}
