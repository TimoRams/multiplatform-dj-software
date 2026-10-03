#include "ApplicationBootstrap.h"
#include "controllers/flx10/Flx10ControllerIdentity.h"
#include "fx/FxManager.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QtGlobal>

namespace {
int fail(const QString& message)
{
    qCritical().noquote() << "[ci-smoke]" << message;
    return 1;
}

bool testFxManagerControls(QString& error)
{
    const auto require = [&error](bool condition, const QString& message) {
        if (!condition && error.isEmpty())
            error = message;
        return condition;
    };

    FxManager manager;
    int deck1BChangedCount = 0;
    int deck2BChangedCount = 0;
    int deck1AChangedCount = 0;
    int unit1EnabledChangedCount = 0;
    int unit2EnabledChangedCount = 0;
    int unit1WetDryChangedCount = 0;
    int unit2WetDryChangedCount = 0;
    QObject::connect(&manager, &FxManager::deck1BChanged,
                     [&] { ++deck1BChangedCount; });
    QObject::connect(&manager, &FxManager::deck2BChanged,
                     [&] { ++deck2BChangedCount; });
    QObject::connect(&manager, &FxManager::deck1AChanged,
                     [&] { ++deck1AChangedCount; });
    QObject::connect(&manager, &FxManager::enabled1Changed,
                     [&] { ++unit1EnabledChangedCount; });
    QObject::connect(&manager, &FxManager::enabled2Changed,
                     [&] { ++unit2EnabledChangedCount; });
    QObject::connect(&manager, &FxManager::wetDry1Changed,
                     [&] { ++unit1WetDryChangedCount; });
    QObject::connect(&manager, &FxManager::wetDry2Changed,
                     [&] { ++unit2WetDryChangedCount; });

    const QMetaObject* const metaObject = manager.metaObject();
    const int deck1BPropertyIndex = metaObject->indexOfProperty("deck1B");
    const int deck2BPropertyIndex = metaObject->indexOfProperty("deck2B");
    const int enabledPropertyIndex = metaObject->indexOfProperty("enabled1");
    if (!require(deck1BPropertyIndex >= 0 && deck2BPropertyIndex >= 0 &&
                     enabledPropertyIndex >= 0,
                 QStringLiteral("FxManager QObject properties are missing")))
        return false;
    const QMetaProperty deck1BProperty =
        metaObject->property(deck1BPropertyIndex);
    const QMetaProperty deck2BProperty =
        metaObject->property(deck2BPropertyIndex);
    if (!require(deck1BProperty.isValid() && deck1BProperty.isWritable() &&
                     deck2BProperty.isValid() && deck2BProperty.isWritable(),
                 QStringLiteral("FxManager deck assignment property is not writable")) ||
        !require(enabledPropertyIndex >= 0 &&
                     !metaObject->property(enabledPropertyIndex).isWritable(),
                 QStringLiteral("FxManager enabled property is unexpectedly writable")))
        return false;

    deck1BProperty.write(&manager, true);
    deck1BProperty.write(&manager, true);
    deck2BProperty.write(&manager, true);
    deck2BProperty.write(&manager, true);
    manager.setDeckAssignment(1, 1, true);
    manager.setDeckAssignment(1, 1, true);
    if (!require(deck1BProperty.read(&manager).toBool() &&
                     deck2BProperty.read(&manager).toBool() &&
                     deck1BChangedCount == 1 && deck2BChangedCount == 1,
                 QStringLiteral("FX assignment properties/signals did not de-duplicate per unit")) ||
        !require(manager.deck1A() && deck1AChangedCount == 1,
                 QStringLiteral("setDeckAssignment did not de-duplicate assignment signals")))
        return false;

    manager.setEffectType(1, QStringLiteral("Echo"));
    manager.setEffectType(2, QStringLiteral("Reverb"));
    manager.setWetDry(1, 0.7f);
    manager.setWetDry(2, 0.4f);
    if (!require(manager.effectType1() == QStringLiteral("Echo") &&
                     manager.effectType2() == QStringLiteral("Reverb") &&
                     qFuzzyCompare(manager.wetDry1(), 0.7f) &&
                     qFuzzyCompare(manager.wetDry2(), 0.4f),
                 QStringLiteral("FX units did not retain independent type and mix state")) ||
        !require(!manager.enabled1() && !manager.enabled2() &&
                     unit1EnabledChangedCount == 0 && unit2EnabledChangedCount == 0,
                 QStringLiteral("setting FX type or mix implicitly enabled a unit")))
        return false;

    manager.setUnitEnabled(1, true);
    if (!require(manager.enabled1() && !manager.enabled2() &&
                     unit1EnabledChangedCount == 1 && unit2EnabledChangedCount == 0 &&
                     unit1WetDryChangedCount == 1 && unit2WetDryChangedCount == 1,
                 QStringLiteral("enabling unit 1 changed unit 2 or emitted unexpected signals")))
        return false;

    manager.setUnitEnabled(2, true);
    manager.setUnitEnabled(1, false);
    manager.setWetDry(1, 0.2f);
    if (!require(!manager.enabled1() && manager.enabled2() &&
                     unit1EnabledChangedCount == 2 && unit2EnabledChangedCount == 1 &&
                     qFuzzyCompare(manager.wetDry1(), 0.2f) &&
                     qFuzzyCompare(manager.wetDry2(), 0.4f) &&
                     unit1WetDryChangedCount == 2 && unit2WetDryChangedCount == 1,
                 QStringLiteral("FX unit enable and mix updates were not isolated")))
        return false;

    return true;
}
}

int runCiSmokeTest(int argc, char* argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND"))
        qputenv("QSG_RHI_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("BrockDJ CI smoke test"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BROCKDJ_VERSION));

    QString fxManagerError;
    if (!testFxManagerControls(fxManagerError))
        return fail(fxManagerError);

    const QString qmlResource = QStringLiteral(":/DJSoftware/src/qml/main.qml");
    if (!QFile::exists(qmlResource))
        return fail(QStringLiteral("embedded main QML resource is missing"));
    if (!QFile::exists(flx10::kMappingResource))
        return fail(QStringLiteral("embedded controller mapping resource is missing"));

    QQmlEngine qmlEngine;
    QQmlComponent component(&qmlEngine, QUrl(QStringLiteral("qrc%1").arg(qmlResource)));
    while (component.isLoading())
        QCoreApplication::processEvents();
    if (component.isError())
        return fail(QStringLiteral("main QML failed to load:\n%1")
                        .arg(component.errorString()));

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
        return fail(QStringLiteral("temporary directory creation failed"));

    const QString connectionName = QStringLiteral("brockdj_ci_smoke");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(temporaryDirectory.filePath(QStringLiteral("library.sqlite")));
        if (!database.open())
            return fail(QStringLiteral("temporary SQLite database failed to open: %1")
                            .arg(database.lastError().text()));

        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "CREATE TABLE smoke_check (id INTEGER PRIMARY KEY, value TEXT NOT NULL)")))
            return fail(QStringLiteral("temporary SQLite initialization failed: %1")
                            .arg(query.lastError().text()));
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    qInfo().noquote() << "BrockDJ" << BROCKDJ_VERSION << BROCKDJ_BUILD_ARCH
                      << "package smoke test passed";
    return 0;
}
