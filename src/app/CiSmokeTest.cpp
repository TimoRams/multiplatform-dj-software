#include "ApplicationBootstrap.h"
#include "controllers/flx10/Flx10ControllerIdentity.h"
#include "fx/FxManager.h"
#include "app/ControlClock.h"
#include "controllers/midi/MidiControllerManager.h"
#include "controllers/midi/ParameterStore.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QProcess>
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
    for (const int unit : {1, 2}) {
        manager.setUnitEnabled(unit, true);
        if (!require(manager.unitEnabled(unit)
                         && (unit == 1 ? manager.wetDry1() : manager.wetDry2()) == 0.0f,
                     QStringLiteral("FX cold-start enable changed zero mix")))
            return false;
        manager.setUnitEnabled(unit, false);
    }
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

bool testBeatFxFirstHardwareEngage(QString& error)
{
    ControlClock clock;
    ParameterStore store;
    FxManager fx;
    MidiControllerManager midi(&store, clock, nullptr,
                               MidiControllerManager::HardwareAccess::Disabled);
    midi.connectDecks(nullptr, nullptr, nullptr, nullptr);
    midi.connectFxManager(&fx);
    const auto send = [&store](const char* id, float value) {
        store.setMidiParameter(QString::fromLatin1(id), value);
    };
    const auto require = [&error](bool condition, const QString& message) {
        if (!condition && error.isEmpty())
            error = message;
        return condition;
    };
    if (!require(fx.soundColorMode() == QStringLiteral("None"),
                 QStringLiteral("Sound Color was selected at startup")))
        return false;
    midi.selectMapping(flx10::kMappingLabel);
    const auto sendColorWire = [&midi](int note, int velocity) {
        const juce::uint8 bytes[] {0x96, static_cast<juce::uint8>(note),
                                  static_cast<juce::uint8>(velocity)};
        static_cast<juce::MidiInputCallback&>(midi).handleIncomingMidiMessage(
            nullptr, juce::MidiMessage(bytes, 3));
        QCoreApplication::sendPostedEvents(&midi, QEvent::MetaCall);
    };
    const QStringList colorModes {
        QStringLiteral("Space"), QStringLiteral("D.Echo"), QStringLiteral("Crush"),
        QStringLiteral("Pitch"), QStringLiteral("Noise"), QStringLiteral("Filter")
    };
    for (int note = 0; note < colorModes.size(); ++note) {
        sendColorWire(note, 127);
        if (!require(fx.soundColorMode() == colorModes[note],
                     QStringLiteral("Sound Color wire ON failed for %1").arg(colorModes[note])))
            return false;
        sendColorWire(note, 127);
        if (!require(fx.soundColorMode() == colorModes[note],
                     QStringLiteral("repeated Sound Color wire ON toggled off")))
            return false;
        sendColorWire(note, 0);
        if (!require(fx.soundColorMode() == QStringLiteral("None"),
                     QStringLiteral("Sound Color zero-velocity wire OFF was dropped")))
            return false;
    }
    sendColorWire(5, 127);
    sendColorWire(4, 127);
    sendColorWire(5, 0);
    if (!require(fx.soundColorMode() == QStringLiteral("Noise"),
                 QStringLiteral("late wire OFF for Filter cleared Noise")))
        return false;
    sendColorWire(4, 0);
    sendColorWire(3, 127);
    sendColorWire(3, 0);
    sendColorWire(3, 127);
    if (!require(fx.soundColorMode() == QStringLiteral("Pitch"),
                 QStringLiteral("Sound Color wire re-selection required two presses")))
        return false;
    sendColorWire(3, 0);
    if (!require(fx.soundColorMode() == QStringLiteral("None"),
                 QStringLiteral("Sound Color wire OFF left a mode selected")))
        return false;
    send("deckA_sound_color", 1.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("None"),
                 QStringLiteral("turning an unassigned Color knob selected an effect")))
        return false;
    send("sound_color_fx_filter", 1.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Filter"),
                 QStringLiteral("Sound Color button failed to select Filter")))
        return false;
    send("sound_color_fx_filter", 1.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Filter"),
                 QStringLiteral("repeated Sound Color ON toggled off the selection")))
        return false;
    send("sound_color_fx_filter", 0.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("None"),
                 QStringLiteral("Sound Color OFF failed to deselect")))
        return false;
    send("sound_color_fx_filter", 1.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Filter"),
                 QStringLiteral("Sound Color ON after OFF failed to select")))
        return false;
    send("sound_color_fx_noise", 1.0f);
    send("sound_color_fx_filter", 0.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Noise"),
                 QStringLiteral("previous Sound Color OFF cleared the new selection")))
        return false;
    send("sound_color_fx_crush", 1.0f);
    send("sound_color_fx_noise", 0.0f);
    if (!require(fx.soundColorMode() == QStringLiteral("Crush"),
                 QStringLiteral("Sound Color mode replacement failed")))
        return false;
    send("sound_color_fx_crush", 0.0f);
    send("beat_fx_select_2", 1.0f);
    send("beat_fx_channel_deck_a", 1.0f);
    send("beat_fx_level_depth", 0.7f);
    if (!require(!fx.enabled1(), QStringLiteral("hardware FX selectors/knob engaged the unit")))
        return false;

    // This is the cold-start path: wetDry1 is still zero, while the physical
    // knob has already reported 0.7. Its synchronous notification used to
    // replace the pending ON with the unit's old disabled state.
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && midi.beatFxActive() && fx.deck1A()
                     && fx.effectType1() == QStringLiteral("Echo")
                     && qFuzzyCompare(fx.wetDry1(), 0.7f),
                 QStringLiteral("first hardware FX ON was cancelled before UI activation")))
        return false;
    send("beat_fx_on", 0.0f);
    send("beat_fx_level_depth", 0.3f);
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && midi.beatFxActive()
                     && qFuzzyCompare(fx.wetDry1(), 0.3f),
                 QStringLiteral("hardware FX re-engage lost the new knob depth")))
        return false;
    fx.setUnitEnabled(1, false);
    fx.setWetDry1(0.0f);
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && midi.beatFxActive() && fx.wetDry1() == 0.0f,
                 QStringLiteral("hardware FX ON raised zero UI depth")))
        return false;
    send("beat_fx_on", 0.0f);
    send("beat_fx_level_depth", 0.0f);
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && midi.beatFxActive() && fx.wetDry1() == 0.0f,
                 QStringLiteral("hardware FX ON raised the knob's zero depth")))
        return false;
    send("beat_fx_level_depth", 0.0005f);
    if (!require(fx.enabled1() && qFuzzyCompare(fx.wetDry1(), 0.0005f),
                 QStringLiteral("fine hardware FX depth was replaced by a default")))
        return false;
    send("beat_fx_on", 0.0f);
    send("beat_fx_on", 1.0f);
    if (!require(fx.enabled1() && qFuzzyCompare(fx.wetDry1(), 0.0005f),
                 QStringLiteral("hardware FX re-engage raised fine depth")))
        return false;
    send("beat_fx_channel_master", 1.0f);
    send("beat_fx_level_depth", 0.4f);
    if (!require(fx.enabled1() && !fx.deck1A() && !fx.deck1B()
                     && !fx.deck1C() && !fx.deck1D(),
                 QStringLiteral("Master FX also left per-deck instances assigned")))
        return false;
    send("beat_fx_select_5", 1.0f);
    const float previousDivision = fx.beatDiv1();
    fx.setPrimaryParam(1, 0.01f);
    const QList<float> reverbSteps {0.01f, 0.10f, 0.25f, 0.50f, 0.75f, 0.90f, 1.0f};
    for (const float expected : reverbSteps) {
        if (expected != reverbSteps.front())
            send("beat_fx_beat_plus", 1.0f);
        if (!require(qFuzzyCompare(fx.primaryParam1(), expected)
                         && fx.beatDiv1() == previousDivision,
                     QStringLiteral("Reverb BEAT plus changed timing instead of percentage")))
            return false;
    }
    send("beat_fx_beat_plus", 1.0f);
    if (!require(fx.primaryParam1() == 1.0f,
                 QStringLiteral("Reverb percentage exceeded 100 percent")))
        return false;
    for (auto it = reverbSteps.crbegin() + 1; it != reverbSteps.crend(); ++it) {
        send("beat_fx_beat_minus", 1.0f);
        if (!require(qFuzzyCompare(fx.primaryParam1(), *it),
                     QStringLiteral("Reverb BEAT minus skipped a percentage step")))
            return false;
    }
    send("beat_fx_beat_minus", 1.0f);
    send("beat_fx_level_depth", 0.8f);
    send("beat_fx_on", 0.0f);
    send("beat_fx_on", 1.0f);
    if (!require(qFuzzyCompare(fx.primaryParam1(), 0.01f)
                     && qFuzzyCompare(fx.wetDry1(), 0.8f),
                 QStringLiteral("Reverb mix or ON overwrote its percentage")))
        return false;
    send("beat_fx_select_2", 1.0f);
    send("beat_fx_beat_plus", 1.0f);
    if (!require(fx.beatDiv1() > previousDivision,
                 QStringLiteral("Echo BEAT plus no longer controls timing")))
        return false;
    fx.setUnitEnabled(1, false);
    return require(!midi.beatFxActive(),
                   QStringLiteral("UI FX OFF did not update hardware-side state"));
}
}

int runCiSmokeTest(int argc, char* argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("BrockDJ CI smoke test"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BROCKDJ_VERSION));

    QString fxManagerError;
    if (!testFxManagerControls(fxManagerError))
        return fail(fxManagerError);
    if (!testBeatFxFirstHardwareEngage(fxManagerError))
        return fail(fxManagerError);

    const QString qmlResource = QStringLiteral(":/DJSoftware/src/qml/main.qml");
    if (!QFile::exists(qmlResource))
        return fail(QStringLiteral("embedded main QML resource is missing"));
    if (!QFile::exists(flx10::kMappingResource))
        return fail(QStringLiteral("embedded controller mapping resource is missing"));

    QQmlEngine qmlEngine;
    configureEmbeddedQmlEngine(qmlEngine);
    QQmlComponent component(&qmlEngine, QUrl(QStringLiteral("qrc%1").arg(qmlResource)));
    while (component.isLoading())
        QCoreApplication::processEvents();
    if (component.isError())
        return fail(QStringLiteral("main QML failed to load (Qt %1; import paths: %2):\n%3")
                        .arg(QString::fromLatin1(qVersion()),
                             qmlEngine.importPathList().join(QStringLiteral(", ")),
                             component.errorString()));

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

    const auto runStartupSubprocess = [](const QString& flag, int timeoutMs,
                                         QString& error) {
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(), {flag});
        if (!process.waitForStarted(5000)) {
            error = QStringLiteral("isolated startup smoke could not start: %1")
                        .arg(process.errorString());
            return false;
        }
        if (!process.waitForFinished(timeoutMs)) {
            process.terminate();
            if (!process.waitForFinished(2000)) {
                process.kill();
                process.waitForFinished(2000);
            }
            const QString output = QString::fromLocal8Bit(process.readAllStandardOutput())
                + QString::fromLocal8Bit(process.readAllStandardError());
            error = QStringLiteral("isolated startup smoke exceeded its deadline (%1):\n%2")
                        .arg(flag, output);
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            error = QStringLiteral("isolated startup smoke failed (%1):\n%2")
                        .arg(flag,
                             QString::fromLocal8Bit(process.readAllStandardOutput())
                                 + QString::fromLocal8Bit(process.readAllStandardError()));
            return false;
        }
        return true;
    };

    QString startupSmokeError;
    if (!runStartupSubprocess(QStringLiteral("--ci-startup-smoke-test"), 35000,
                              startupSmokeError))
        return fail(startupSmokeError);
    if (!runStartupSubprocess(QStringLiteral("--ci-startup-early-close-test"), 15000,
                              startupSmokeError))
        return fail(startupSmokeError);

    qInfo().noquote() << "BrockDJ" << BROCKDJ_VERSION << BROCKDJ_BUILD_ARCH
                      << "package, startup-close, and early-close smoke tests passed";
    return 0;
}
