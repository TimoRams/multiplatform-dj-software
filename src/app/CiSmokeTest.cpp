#include "ApplicationBootstrap.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
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

    const QString qmlResource = QStringLiteral(":/DJSoftware/src/qml/main.qml");
    if (!QFile::exists(qmlResource))
        return fail(QStringLiteral("embedded main QML resource is missing"));

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
