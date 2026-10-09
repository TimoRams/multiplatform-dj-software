#include <QGuiApplication>
#include <QMouseEvent>
#include <QColor>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <QSGRendererInterface>
#include <QUrl>
#include <QtTest/qtest.h>
#include <QtTest/qtesttouch.h>
#include "app/ControlClock.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

#ifndef BROCKDJ_SOURCE_DIR
#error BROCKDJ_SOURCE_DIR is required
#endif

class ScrollingWaveformItemStub : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject* engine READ engine WRITE setEngine)
    Q_PROPERTY(qreal pixelsPerPoint READ pixelsPerPoint WRITE setPixelsPerPoint)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor)
    Q_PROPERTY(int renderStyle READ renderStyle WRITE setRenderStyle)
    Q_PROPERTY(qreal playheadPosition READ playheadPosition WRITE setPlayheadPosition)
    Q_PROPERTY(bool rasterWorkEnabled READ rasterWorkEnabled WRITE setRasterWorkEnabled)
    Q_PROPERTY(bool slipPreview READ slipPreview WRITE setSlipPreview)
    Q_PROPERTY(bool contentReady READ contentReady CONSTANT)
    Q_PROPERTY(qreal effectivePixelsPerSecond READ effectivePixelsPerSecond CONSTANT)

public:
    QObject* engine() const { return m_engine; }
    void setEngine(QObject* value) { m_engine = value; }
    qreal pixelsPerPoint() const { return m_pixelsPerPoint; }
    void setPixelsPerPoint(qreal value) { m_pixelsPerPoint = value; }
    QColor backgroundColor() const { return m_backgroundColor; }
    void setBackgroundColor(const QColor& value) { m_backgroundColor = value; }
    int renderStyle() const { return m_renderStyle; }
    void setRenderStyle(int value) { m_renderStyle = value; }
    qreal playheadPosition() const { return m_playheadPosition; }
    void setPlayheadPosition(qreal value) { m_playheadPosition = value; }
    bool rasterWorkEnabled() const { return m_rasterWorkEnabled; }
    void setRasterWorkEnabled(bool value) { m_rasterWorkEnabled = value; }
    bool slipPreview() const { return m_slipPreview; }
    void setSlipPreview(bool value) { m_slipPreview = value; }
    bool contentReady() const { return true; }
    qreal effectivePixelsPerSecond() const { return 100.0; }

    Q_INVOKABLE void requestUpdate() {}
    Q_INVOKABLE qreal screenDeltaToSeconds(qreal delta) const { return delta / 100.0; }
    Q_INVOKABLE qreal timelineSecondsAtX(qreal x, qreal playhead) const
    {
        return playhead + (x - width() * m_playheadPosition) / effectivePixelsPerSecond();
    }

private:
    QObject* m_engine = nullptr;
    qreal m_pixelsPerPoint = 0.22;
    QColor m_backgroundColor;
    int m_renderStyle = 0;
    qreal m_playheadPosition = 0.5;
    bool m_rasterWorkEnabled = true;
    bool m_slipPreview = false;
};

namespace {
std::string read(const char* relative)
{
    std::ifstream file(std::string(BROCKDJ_SOURCE_DIR) + "/" + relative);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
std::size_t occurrences(const std::string& value, const std::string& needle)
{
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::string componentSection(const std::string& source, const std::string& name)
{
    const auto begin = source.find("component " + name + ":");
    if (begin == std::string::npos)
        return {};
    const auto open = source.find('{', begin);
    if (open == std::string::npos)
        return {};

    std::size_t depth = 0;
    char quote = '\0';
    bool escaped = false;
    bool lineComment = false;
    bool blockComment = false;
    for (std::size_t index = open; index < source.size(); ++index) {
        const char current = source[index];
        const char next = index + 1 < source.size() ? source[index + 1] : '\0';
        if (lineComment) {
            if (current == '\n')
                lineComment = false;
            continue;
        }
        if (blockComment) {
            if (current == '*' && next == '/') {
                blockComment = false;
                ++index;
            }
            continue;
        }
        if (quote != '\0') {
            if (escaped)
                escaped = false;
            else if (current == '\\')
                escaped = true;
            else if (current == quote)
                quote = '\0';
            continue;
        }
        if (current == '/' && next == '/') {
            lineComment = true;
            ++index;
        } else if (current == '/' && next == '*') {
            blockComment = true;
            ++index;
        } else if (current == '"' || current == '\'') {
            quote = current;
        } else if (current == '{') {
            ++depth;
        } else if (current == '}' && --depth == 0) {
            return source.substr(begin, index + 1 - begin);
        }
    }
    return {};
}

bool extractFunctions(const std::string& source,
                      std::initializer_list<const char*> names, QString& methods)
{
    for (const char* name : names) {
        const auto begin = source.find(std::string("    function ") + name + "(");
        const auto end = source.find("\n    }", begin);
        if (!require(begin != std::string::npos && end != std::string::npos,
                     "QML function under test exists"))
            return false;
        methods += QString::fromStdString(source.substr(begin, end + 6 - begin));
        methods += '\n';
    }
    return true;
}

bool libraryActionTests(const std::string& source)
{
    QString actions;
    if (!extractFunctions(source, {"deckLetterForModifiers", "loadCursorTrackToDeck",
                                   "confirmLibrarySelection"}, actions))
        return false;
    QQmlEngine engine;
    QQmlComponent component(&engine);
    // Execute the production action bodies with observable deck/navigation sinks.
    component.setData((QStringLiteral(R"(
        import QtQuick
        QtObject {
            property string activeTab: "library"
            property string usbFocusArea: "tracks"
            property string cursorPath: "fixture.wav"
            property string cursorTrackId: "local-id"
            property string lastDeck: ""
            property string lastPath: ""
            property string lastTrackId: ""
            property int loads: 0
            property int navigations: 0
            function getCursorFilePath() { return cursorPath }
            function getCursorTrackId() { return cursorTrackId }
            function activateUsbCursor() { ++navigations }
            function loadTrackToDeck(deck, path, trackId) {
                lastDeck = deck
                lastPath = path
                lastTrackId = trackId
                ++loads
            }
        )") + actions + '}').toUtf8(), QUrl());
    std::unique_ptr<QObject> library(component.create());
    if (!require(library != nullptr, "library action harness instantiates")) {
        std::cerr << component.errorString().toStdString();
        return false;
    }
    const struct {
        int modifiers;
        const char* deck;
    } cases[] {
        {Qt::NoModifier, "A"},
        {Qt::ShiftModifier, "B"},
        {Qt::ControlModifier, "C"},
        {Qt::AltModifier, "D"},
        {Qt::ShiftModifier | Qt::ControlModifier, "B"},
        {Qt::ShiftModifier | Qt::AltModifier, "B"},
        {Qt::ControlModifier | Qt::AltModifier, "C"},
        {Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier, "B"}
    };
    bool ok = true;
    for (const auto& test : cases) {
        QVariant result;
        ok &= require(QMetaObject::invokeMethod(library.get(), "deckLetterForModifiers",
            Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, QVariant(test.modifiers)))
                && result.toString() == QString::fromLatin1(test.deck),
            "library keyboard deck selection preserves modifier priority");
    }
    const auto confirm = [&library](const char* deck) {
        return QMetaObject::invokeMethod(library.get(), "confirmLibrarySelection",
            Q_ARG(QVariant, QVariant(QString::fromLatin1(deck))));
    };
    ok &= require(confirm("B") && library->property("loads").toInt() == 1
                      && library->property("lastDeck").toString() == QStringLiteral("B")
                      && library->property("lastPath").toString() == QStringLiteral("fixture.wav")
                      && library->property("lastTrackId").toString() == QStringLiteral("local-id"),
                  "shared library action forwards local track identity unchanged");
    library->setProperty("activeTab", "usb");
    library->setProperty("usbFocusArea", "primary");
    library->setProperty("cursorPath", "rekordbox:device:track");
    library->setProperty("cursorTrackId", "external-id");
    ok &= require(confirm("C") && library->property("loads").toInt() == 1
                      && library->property("navigations").toInt() == 1,
                  "USB confirmation outside tracks navigates instead of loading");
    library->setProperty("usbFocusArea", "tracks");
    ok &= require(confirm("D") && library->property("loads").toInt() == 2
                      && library->property("lastDeck").toString() == QStringLiteral("D")
                      && library->property("lastPath").toString() == QStringLiteral("rekordbox:device:track")
                      && library->property("lastTrackId").toString() == QStringLiteral("external-id"),
                  "USB track confirmation forwards external identity unchanged");
    return ok;
}

bool audioRoleTests(const std::string& source)
{
    const auto comboBoxSource = read("src/qml/components/ComboBox.qml");
    if (!require(comboBoxSource.find(
                     "control.displayText.length > 0 ? control.displayText : control.placeholderText")
                     != std::string::npos,
                 "empty audio combo boxes render an explicit placeholder"))
        return false;
    const auto begin = source.find("    readonly property var audioRoleDefinitions:");
    const auto end = source.find("\n    ]", begin);
    if (!require(begin != std::string::npos && end != std::string::npos,
                 "settings audio-role definitions exist"))
        return false;
    QString methods = QString::fromStdString(source.substr(begin, end + 6 - begin));
    methods += '\n';
    if (!extractFunctions(source, {"getRoleSelections", "setRoleSelections",
            "roleDefinition", "roleRow", "selectedRoleSelections", "parseFirstChannel",
            "pairTextForFirstChannel", "indexForText", "firstRealOutput",
            "getOutputPairOptions", "outputPairCacheKey", "refreshRoleChannelPairs",
            "refreshRoleOutputAndPairs", "refreshAudioBackendStatus"}, methods))
        return false;
    const auto roleModel = source.find("model: settingsWindow.audioRoleDefinitions");
    const auto roleRepeater = source.rfind("Repeater {", roleModel);
    const auto repeaterId = source.find("id: audioRoleRepeater", roleRepeater);
    const auto repeaterEnd = source.find("\n                              }", roleModel);
    const auto rowBegin = source.find("    component AudioRoleRow: RowLayout {");
    const auto rowEnd = source.find("\n    }", rowBegin);
    if (!require(roleModel != std::string::npos && roleRepeater != std::string::npos
                      && repeaterId < roleModel && repeaterEnd != std::string::npos
                      && rowBegin != std::string::npos && rowEnd != std::string::npos,
                 "audio-role lookup is wired to its row repeater, not the category repeater"))
        return false;
    qmlRegisterType(QUrl::fromLocalFile(QString::fromUtf8(BROCKDJ_SOURCE_DIR)
        + QStringLiteral("/src/qml/components/ComboBox.qml")),
        "DJSoftware", 1, 0, "ComboBox");
    QQmlEngine engine;
    QQmlComponent component(&engine);
    const QString properties = QStringLiteral(R"(
            id: settingsWindow
            property string pendingAudioDeviceType: "ALSA"
            property string pendingMasterOutputDevice: ""
            property int pendingMasterFirstChannel: 1
            property string pendingHeadphonesOutputDevice: ""
            property int pendingHeadphonesFirstChannel: -1
            property string pendingBoothOutputDevice: ""
            property int pendingBoothFirstChannel: -1
            property var audioOutputDeviceOptions: ["None", "USB", "Other"]
            property var masterChannelPairOptions: ["None", "1-2", "3-4"]
            property var headphonesChannelPairOptions: ["None", "1-2", "3-4"]
            property var boothChannelPairOptions: ["None", "7-8"]
            property var outputChannelPairsCache: ({})
            property bool audioUiSyncing: false
            property var enumeratedOutputDevices: ["None"]
            property var audioDeviceTypeOptions: ["ALSA", "JACK"]
            property bool audioOutputsAvailable: false
            property bool selectedAudioBackendHasOutputs: false
            property var audioBackendOutputSummary: []
            property var enumeratedOutputDevicesByType: ({
                ALSA: ["None"],
                JACK: ["None", "USB"]
            })
            property var rows: [
                {outputCombo: {currentIndex: 0}, channelCombo: {currentIndex: 0}},
                {outputCombo: {currentIndex: 0}, channelCombo: {currentIndex: 0}},
                {outputCombo: {currentIndex: 0}, channelCombo: {currentIndex: 0}}
            ]
            property var deckA: ({
                getAvailableAudioOutputDevices: function(type) {
                    return settingsWindow.enumeratedOutputDevicesByType[type]
                        || settingsWindow.enumeratedOutputDevices
                },
                getAvailableOutputChannelPairs: function(type, device) {
                    if (device === "USB") return ["None", "1-2", "3-4"]
                    if (device === "Other") return ["None", "7-8"]
                    return ["None"]
                }
            })
        )");
    component.setData((QStringLiteral("import QtQuick\nQtObject {")
        + properties + QStringLiteral(R"(
            property var audioRoleRepeater: ({
                itemAt: function(index) { return settingsWindow.rows[index] }
            })
        )") + methods + '}').toUtf8(), QUrl());
    std::unique_ptr<QObject> settings(component.create());
    if (!require(settings != nullptr, "audio-role action harness instantiates")) {
        std::cerr << component.errorString().toStdString();
        return false;
    }
    const auto evaluate = [&settings](const char* script) {
        QQmlExpression expression(QQmlEngine::contextForObject(settings.get()),
                                  settings.get(), QString::fromUtf8(script));
        const QVariant result = expression.evaluate();
        if (expression.hasError()) {
            std::cerr << expression.error().toString().toStdString() << '\n';
            return false;
        }
        return result.toBool();
    };
    bool ok = true;
    ok &= require(evaluate(
        "setRoleSelections('master', 'USB', 1);"
        "setRoleSelections('headphones', 'Other', 7);"
        "setRoleSelections('booth', 'None', -1);"
        "pendingMasterOutputDevice === 'USB' && pendingMasterFirstChannel === 1"
        " && pendingHeadphonesOutputDevice === 'Other' && pendingHeadphonesFirstChannel === 7"
        " && pendingBoothOutputDevice === 'None' && pendingBoothFirstChannel === -1"),
        "audio-role setters update independent pending properties");
    ok &= require(evaluate(
        "roleRow('master') === rows[0] && roleRow('headphones') === rows[1]"
        " && roleRow('booth') === rows[2]"),
        "role rows resolve through the repeater without a second lifetime registry");
    ok &= require(evaluate(
        "var s = selectedRoleSelections('master');"
        "s.outputDevice === 'None' && s.firstChannel === -1"
        " && pendingMasterOutputDevice === 'USB' && pendingMasterFirstChannel === 1"),
        "Apply selection reads combos without mutating pending preferences");
    ok &= require(evaluate(
        "rows[0].outputCombo.currentIndex = 1; rows[0].channelCombo.currentIndex = 2;"
        "var s = selectedRoleSelections('master');"
        "s.outputDevice === 'USB' && s.firstChannel === 3"),
        "selected role returns the actual output and channel pair");
    ok &= require(evaluate(
        "rows[0].outputCombo.currentIndex = 99; rows[0].channelCombo.currentIndex = 99;"
        "var s = selectedRoleSelections('master');"
        "s.outputDevice === 'USB' && s.firstChannel === 1"),
        "stale combo indexes cannot replace pending routing with undefined values");
    ok &= require(evaluate(
        "rows[1] = null; var s = selectedRoleSelections('headphones');"
        "s.outputDevice === 'Other' && s.firstChannel === 7"),
        "rows not yet instantiated retain their pending selection");
    ok &= require(evaluate(
        "rows[1] = {outputCombo: {currentIndex: 0}, channelCombo: {currentIndex: 0}};"
        "setRoleSelections('master', 'USB', -1); refreshRoleChannelPairs('master');"
        "pendingMasterFirstChannel === 1 && rows[0].channelCombo.currentIndex === 1"
        " && masterChannelPairOptions.length === 3 && !audioUiSyncing"),
        "Master on a real device normalizes None to its first available pair");
    ok &= require(evaluate(
        "setRoleSelections('headphones', 'USB', -1); refreshRoleChannelPairs('headphones');"
        "pendingHeadphonesFirstChannel === -1 && rows[1].channelCombo.currentIndex === 0"
        " && headphonesChannelPairOptions.length === 3 && !audioUiSyncing"),
        "Headphones None remains disabled rather than inheriting Master normalization");
    ok &= require(evaluate(
        "setRoleSelections('booth', 'Other', 7); refreshRoleChannelPairs('booth');"
        "pendingBoothFirstChannel === 7 && rows[2].channelCombo.currentIndex === 1"
        " && boothChannelPairOptions[1] === '7-8' && !audioUiSyncing"),
        "Booth retains its independent device channel pairs");
    ok &= require(evaluate(
        "audioOutputDeviceOptions = ['None'];"
        "setRoleSelections('master', 'Saved USB', 3); refreshRoleOutputAndPairs('master');"
        "pendingMasterOutputDevice === 'Saved USB' && rows[0].outputCombo.currentIndex === 0"),
        "incomplete device enumeration does not overwrite a saved output name");
    ok &= require(evaluate(
        "audioOutputDeviceOptions = ['None', 'Saved USB'];"
        "enumeratedOutputDevicesByType = ({ALSA: ['None'], JACK: ['None']});"
        "pendingAudioDeviceType = 'ALSA'; refreshAudioBackendStatus();"
        "!audioOutputsAvailable && !selectedAudioBackendHasOutputs"),
        "a remembered name does not count as an enumerated backend device");
    ok &= require(evaluate(
        "enumeratedOutputDevicesByType = ({ALSA: ['None'], JACK: ['None', 'USB']});"
        "pendingAudioDeviceType = 'ALSA'; refreshAudioBackendStatus();"
        "audioOutputsAvailable && !selectedAudioBackendHasOutputs"
        " && pendingAudioDeviceType === 'ALSA'"
        " && audioBackendOutputSummary.join('|').indexOf('JACK: USB') >= 0"),
        "an empty selected API is distinguished from devices found on another API without switching selection");
    ok &= require(evaluate(
        "pendingAudioDeviceType = 'JACK'; refreshAudioBackendStatus();"
        "audioOutputsAvailable && selectedAudioBackendHasOutputs"),
        "the selected backend is reported as available when its own output list is populated");
    ok &= require(evaluate(
        "audioDeviceTypeOptions = []; refreshAudioBackendStatus();"
        "!audioOutputsAvailable && !selectedAudioBackendHasOutputs"
        " && audioBackendOutputSummary.length === 0"),
        "an empty backend list is distinguished from a backend that has no output devices");
    ok &= require(evaluate(
        "audioOutputDeviceOptions = ['None', 'USB', 'Other'];"
        "setRoleSelections('master', 'Unavailable', 3); refreshRoleOutputAndPairs('master');"
        "pendingMasterOutputDevice === 'USB' && pendingMasterFirstChannel === 3"
        " && rows[0].outputCombo.currentIndex === 1 && rows[0].channelCombo.currentIndex === 2"
        " && !audioUiSyncing"),
        "completed device enumeration reconciles output and pair indexes together");
    ok &= require(evaluate(
        "try { getRoleSelections('unknown'); false }"
        " catch (error) { error.message === 'Unknown audio output role: unknown' }"),
        "invalid internal roles report errors instead of looking like disabled outputs");

    QQmlComponent visualComponent(&engine);
    visualComponent.setData((QStringLiteral(
        "import QtQuick\nimport QtQuick.Layouts\nimport DJSoftware\nItem {")
        + properties + methods
        + QString::fromStdString(source.substr(rowBegin, rowEnd + 6 - rowBegin)) + '\n'
        + QString::fromStdString(source.substr(roleRepeater, repeaterEnd + 32 - roleRepeater))
        + '}').toUtf8(), QUrl());
    settings.reset(visualComponent.create());
    if (!require(settings != nullptr, "production audio-role rows instantiate")) {
        std::cerr << visualComponent.errorString().toStdString();
        return false;
    }
    ok &= require(evaluate(
        "audioRoleRepeater.count === 3 && roleRow('master').roleDefinition.key === 'master'"
        " && roleRow('headphones').roleDefinition.key === 'headphones'"
        " && roleRow('booth').roleDefinition.key === 'booth'"),
        "the real row repeater exposes all three independent roles");
    ok &= require(evaluate(
        "setRoleSelections('master', 'USB', 3); refreshRoleOutputAndPairs('master');"
        "var s = selectedRoleSelections('master');"
        "s.outputDevice === 'USB' && s.firstChannel === 3"
        " && roleRow('master').outputCombo.currentIndex === 1"
        " && roleRow('master').channelCombo.currentIndex === 2"),
        "device refresh updates the actual role combos used by Apply");
    ok &= require(evaluate(
        "roleRow('headphones').outputCombo.currentIndex = 2;"
        "roleRow('headphones').channelCombo.currentIndex = 1;"
        "pendingHeadphonesOutputDevice === 'Other' && pendingHeadphonesFirstChannel === 7"
        " && pendingMasterOutputDevice === 'USB' && pendingMasterFirstChannel === 3"),
        "production combo callbacks update only their assigned role");
    return ok;
}

bool sliderCleanupTests()
{
    const QString componentDirectory = QString::fromUtf8(BROCKDJ_SOURCE_DIR)
        + QStringLiteral("/src/qml/components/");
    qmlRegisterSingletonType(QUrl::fromLocalFile(
        componentDirectory + QStringLiteral("UiTheme.qml")),
        "DJSoftware", 1, 0, "UiTheme");

    QQmlEngine engine;
    QQmlComponent cursorComponent(&engine);
    cursorComponent.setData(R"(
        import QtQml
        QtObject {
            property int hideCalls: 0
            property int restoreCalls: 0
            property int moveCalls: 0
            property real lastX: 0
            property real lastY: 0
            function hideCursor() { ++hideCalls }
            function restoreCursor() { ++restoreCalls }
            function moveCursor(x, y) {
                ++moveCalls
                lastX = x
                lastY = y
            }
        })", QUrl());
    std::unique_ptr<QObject> cursor(cursorComponent.create());
    if (!require(cursor != nullptr, "slider test cursor loads")) {
        std::cerr << cursorComponent.errorString().toStdString();
        return false;
    }
    engine.rootContext()->setContextProperty(QStringLiteral("cursorControl"),
                                             cursor.get());
    QQmlComponent sliderComponent(&engine, QUrl::fromLocalFile(
        componentDirectory + QStringLiteral("Slider.qml")));
    std::unique_ptr<QObject> slider(sliderComponent.create());
    if (!require(slider != nullptr, "shared slider instantiates")) {
        std::cerr << sliderComponent.errorString().toStdString();
        return false;
    }
    const auto evaluate = [&slider](const char* script) {
        QQmlExpression expression(QQmlEngine::contextForObject(slider.get()),
                                  slider.get(), QString::fromUtf8(script));
        const QVariant value = expression.evaluate();
        if (expression.hasError()) {
            std::cerr << expression.error().toString().toStdString() << '\n';
            return false;
        }
        return value.toBool();
    };
    bool ok = true;
    ok &= require(evaluate(
        "sliderDrag._active = true; sliderDrag._cursorHidden = true;"
        "sliderDrag._cursorService = cursorControl;"
        "dragActive = true; sliderDrag.canceled();"
        "!dragActive && !sliderDrag._active && !sliderDrag._cursorHidden"),
        "cancellation clears slider drag and restores its cursor");
    ok &= require(cursor->property("restoreCalls").toInt() == 1
                      && cursor->property("moveCalls").toInt() == 0,
                  "cancellation never teleports the cursor");
    ok &= require(evaluate(
        "sliderDrag._active = true; sliderDrag._cursorHidden = true;"
        "sliderDrag._cursorService = cursorControl;"
        "sliderDrag._pressGX = 123; sliderDrag._pressGY = 45;"
        "dragActive = true; sliderDrag.released(null);"
        "!dragActive && !sliderDrag._active && !sliderDrag._cursorHidden"),
        "mouse release clears drag state");
    ok &= require(cursor->property("restoreCalls").toInt() == 2
                      && cursor->property("moveCalls").toInt() == 1
                      && cursor->property("lastX").toDouble() == 123
                      && cursor->property("lastY").toDouble() == 45,
                  "mouse release restores the press position once");
    ok &= require(evaluate(
        "sliderDrag._active = true; dragActive = true;"
        "sliderDrag.released(null); sliderDrag.canceled();"
        "!dragActive && !sliderDrag._active"),
        "drag without a hidden cursor and repeated cleanup are safe");
    ok &= require(cursor->property("restoreCalls").toInt() == 2
                      && cursor->property("moveCalls").toInt() == 1,
                  "touch-style release and redundant cleanup leave the cursor alone");

    QQuickWindow window;
    window.resize(400, 300);
    auto* sliderItem = qobject_cast<QQuickItem*>(slider.get());
    if (!require(sliderItem != nullptr, "slider is a visual item"))
        return false;
    sliderItem->setParentItem(window.contentItem());
    sliderItem->setPosition(QPointF(10, 10));
    sliderItem->setSize(QSizeF(150, 22));
    slider->setProperty("value", 0.5);
    window.show();
    QCoreApplication::processEvents();
    const auto sendMouse = [&window](QEvent::Type type, QPointF position,
                                    Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, window.mapToGlobal(position),
                          button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &event);
    };
    sendMouse(QEvent::MouseButtonPress, {50, 20}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, {80, 20}, Qt::NoButton, Qt::LeftButton);
    ok &= require(slider->property("dragActive").toBool()
                      && std::abs(slider->property("value").toDouble() - 0.7) < 1e-6,
                  "shared slider keeps relative mouse drag scaling");
    sendMouse(QEvent::MouseButtonRelease, {80, 20}, Qt::LeftButton, Qt::NoButton);
    ok &= require(!slider->property("dragActive").toBool()
                      && cursor->property("hideCalls").toInt() == 1
                      && cursor->property("restoreCalls").toInt() == 3
                      && cursor->property("moveCalls").toInt() == 2,
                  "real mouse drag restores the hidden cursor exactly once");

    sliderItem->setSize(QSizeF(22, 150));
    slider->setProperty("orientation", Qt::Vertical);
    slider->setProperty("from", 1.0);
    slider->setProperty("to", -1.0);
    slider->setProperty("value", 0.0);
    sendMouse(QEvent::MouseButtonPress, {20, 70}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, {20, 40}, Qt::NoButton, Qt::LeftButton);
    ok &= require(slider->property("dragActive").toBool()
                      && std::abs(slider->property("value").toDouble() + 0.4) < 1e-6,
                  "vertical inverted sliders retain their direction and scaling");
    sliderItem->setEnabled(false);
    ok &= require(!slider->property("dragActive").toBool()
                      && cursor->property("hideCalls").toInt() == 2
                      && cursor->property("restoreCalls").toInt() == 4
                      && cursor->property("moveCalls").toInt() == 2,
                  "disabling a dragging slider cancels without teleporting");
    sendMouse(QEvent::MouseButtonRelease, {20, 40}, Qt::LeftButton, Qt::NoButton);
    sliderItem->setEnabled(true);
    sliderItem->setSize(QSizeF(150, 22));
    slider->setProperty("orientation", Qt::Horizontal);
    slider->setProperty("from", 0.0);
    slider->setProperty("to", 1.0);

    auto* touchDevice = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
    slider->setProperty("value", 0.5);
    QTest::touchEvent(&window, touchDevice).press(0, {50, 20}, &window);
    ok &= require(evaluate("sliderTouch.active && sliderDrag._touchDrag"),
                  "slider records the touchscreen source when the press begins");
    QTest::touchEvent(&window, touchDevice).move(0, {80, 20}, &window);
    const bool touchMoved = QTest::qWaitFor([&slider] {
        return slider->property("dragActive").toBool();
    }, 1000);
    if (!slider->property("dragActive").toBool()
        || std::abs(slider->property("value").toDouble() - 0.7) >= 1e-6) {
        QQmlExpression state(QQmlEngine::contextForObject(slider.get()), slider.get(),
            QStringLiteral("JSON.stringify({value: value, active: dragActive,"
                           "pressed: sliderDrag.pressed, pressValue: sliderDrag._pressVal,"
                           "pressX: sliderDrag._pressGX, point: sliderTouch.point.position})"));
        std::cerr << "Touch drag state: "
                  << state.evaluate().toString().toStdString() << '\n';
    }
    ok &= require(touchMoved
                      && std::abs(slider->property("value").toDouble() - 0.7) < 1e-6,
                  "touch drag shares the mouse relative scaling");
    QTest::touchEvent(&window, touchDevice).release(0, {80, 20}, &window);
    const bool touchReleased = QTest::qWaitFor([&slider] {
        return !slider->property("dragActive").toBool();
    }, 1000);
    ok &= require(touchReleased
                      && cursor->property("hideCalls").toInt() == 2
                      && cursor->property("restoreCalls").toInt() == 4
                      && cursor->property("moveCalls").toInt() == 2,
                  "real touch release never hides or teleports the mouse cursor");
    QTest::touchEvent(&window, touchDevice).press(0, {50, 20}, &window);
    QTest::touchEvent(&window, touchDevice).release(0, {80, 20}, &window);
    const bool shortTouchReleased = QTest::qWaitFor([&evaluate] {
        return evaluate("!sliderTouch.active && !sliderDrag.pressed");
    }, 1000);
    ok &= require(shortTouchReleased
                      && cursor->property("hideCalls").toInt() == 2
                      && cursor->property("restoreCalls").toInt() == 4
                      && cursor->property("moveCalls").toInt() == 2,
                  "touch release without an intermediate move cannot become a mouse drag");

    ok &= require(evaluate(
        "sliderDrag._active = true; sliderDrag._cursorHidden = true;"
        "sliderDrag._cursorService = cursorControl;"
        "dragActive = true; true"),
        "destruction case arms the drag");
    engine.rootContext()->setContextProperty(QStringLiteral("cursorControl"),
                                             static_cast<QObject*>(nullptr));
    slider.reset();
    ok &= require(cursor->property("restoreCalls").toInt() == 5
                      && cursor->property("moveCalls").toInt() == 2,
                  "active slider teardown restores even after context bindings are cleared");
    return ok;
}

std::unique_ptr<QObject> createQmlObject(QQmlEngine& engine, const QString& source,
                                         const char* description)
{
    QQmlComponent component(&engine);
    component.setData(source.toUtf8(), QUrl());
    std::unique_ptr<QObject> object(component.create());
    if (!require(object != nullptr, description))
        std::cerr << component.errorString().toStdString() << '\n';
    return object;
}

bool clickItem(QQuickWindow& window, QObject* object, const char* description)
{
    auto* item = qobject_cast<QQuickItem*>(object);
    if (!require(item != nullptr && item->isVisible(), description))
        return false;
    const QPointF center = item->mapToScene(item->boundingRect().center());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
    QCoreApplication::processEvents();
    return true;
}

bool headerPublicationTests(const std::string& header)
{
    QString methods;
    if (!extractFunctions(header, {"deckBeatInfo"}, methods))
        return false;
    const auto connectionsBegin = header.find("    Connections {");
    const auto connectionsEnd = header.find("    Component.onCompleted:", connectionsBegin);
    if (!require(connectionsBegin != std::string::npos && connectionsEnd != std::string::npos,
                 "production header publication connections exist"))
        return false;
    QQmlEngine engine;
    ControlClock clock;
    engine.rootContext()->setContextProperty("controlClock", &clock);
    auto deck = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property var atomicState: ({position: 0})
            property var trackData: ({isBpmAnalyzed: true, bpm: 120,
                                     sampleRate: 48000, firstBeatSample: 0})
            property real masterVuLevelL: 0
            property real masterVuLevelR: 0
            signal progressChanged()
            signal playingChanged()
            function getPlayheadPositionAtomic() { return atomicState.position }
            function moveAtomic(position) { atomicState.position = position }
        }
    )"), "header atomic deck sink instantiates");
    if (!deck)
        return false;
    engine.rootContext()->setContextProperty("deckA", deck.get());
    engine.rootContext()->setContextProperty("deckB", deck.get());
    const auto meterBegin = header.find("        property real levelL: deckA");
    const auto meterEnd = header.find("\n        property bool clipNow:", meterBegin);
    if (!require(meterBegin != std::string::npos && meterEnd != std::string::npos,
                 "production master meter bindings exist"))
        return false;
    auto host = createQmlObject(engine, QStringLiteral(R"(
        import QtQuick
        Item {
            id: root
            property int beatUiTick: 0
            property string currentTime: ""
            property var beatInfo: deckBeatInfo(deckA)
            function refreshLatencyInfo() {}
    )") + methods
        + QString::fromStdString(header.substr(connectionsBegin, connectionsEnd - connectionsBegin))
        + QString::fromStdString(header.substr(meterBegin, meterEnd - meterBegin))
        + QStringLiteral("\n}"), "real header tick and meter bindings instantiate");
    if (!host)
        return false;
    double peak = 0.0;
    ControlClock::Callbacks callbacks;
    callbacks.meters = [&](const ControlTickContext&) {
        deck->setProperty("masterVuLevelL", peak);
        deck->setProperty("masterVuLevelR", peak * 0.5);
    };
    auto registration = clock.registerCallbacks(std::move(callbacks));
    auto move = [&](double position) {
        QQmlExpression expression(QQmlEngine::contextForObject(deck.get()), deck.get(),
                                   QStringLiteral("moveAtomic(%1)").arg(position));
        expression.evaluate();
    };
    auto bar = [&] {
        QQmlExpression expression(QQmlEngine::contextForObject(host.get()), host.get(),
                                  QStringLiteral("beatInfo.barNumber"));
        return expression.evaluate().toInt();
    };
    clock.advanceForTesting(0.004);
    move(8.5);
    peak = 0.8;
    for (int index = 0; index < 60; ++index)
        clock.advanceForTesting(1.0 / 60.0);
    bool ok = require(bar() == 5 && host->property("levelL").toDouble() == 0.8
                          && host->property("levelR").toDouble() == 0.4,
                      "sustained severely late ticks refresh actual header bar and master VU bindings");
    move(0.5);
    peak = 0;
    clock.advanceForTesting(2.0);
    ok &= require(bar() == 1 && host->property("levelL").toDouble() == 0,
                  "a long stall coalesces directly to current seek position and silence");
    move(16.5);
    QMetaObject::invokeMethod(deck.get(), "progressChanged");
    ok &= require(bar() == 9, "paused seek invalidates header immediately without a clock tick");
    move(2.5);
    QMetaObject::invokeMethod(deck.get(), "playingChanged");
    ok &= require(bar() == 2, "play transition invalidates the atomic header position");
    deck->setProperty("trackData", QVariantMap{{"isBpmAnalyzed", false}});
    QQmlExpression valid(QQmlEngine::contextForObject(host.get()), host.get(),
                          QStringLiteral("beatInfo.valid"));
    ok &= require(!valid.evaluate().toBool(), "eject or unanalyzed track clears the header count");
    return ok;
}

bool hamburgerToggleTests(const std::string& header)
{
    const auto popupBegin = header.find("    Popup {\n        id: viewMenuPopup");
    const auto popupContent = header.find("        contentItem: Column {", popupBegin);
    const auto buttonId = header.find("            id: viewMenuBtn");
    const auto buttonBegin = header.rfind("        Rectangle {", buttonId);
    const auto buttonEnd = header.find("\n        }", buttonId);
    if (!require(popupBegin != std::string::npos && popupContent != std::string::npos
                     && buttonBegin != std::string::npos && buttonEnd != std::string::npos,
                 "production hamburger popup and button exist"))
        return false;
    QQmlEngine engine;
    const QString source = QStringLiteral(R"(
        import QtQuick
        import QtQuick.Layouts
        import QtQuick.Controls
        Item {
            id: root
            width: 400; height: 300
            property int btnH: 40
    )") + QString::fromStdString(header.substr(popupBegin, popupContent - popupBegin))
        + QStringLiteral("height: 100\n    }\n")
        + QString::fromStdString(header.substr(buttonBegin, buttonEnd + 10 - buttonBegin))
        + QStringLiteral("\n}");
    auto host = createQmlObject(engine, source, "production hamburger controls instantiate");
    if (!host)
        return false;
    QQuickWindow window;
    window.resize(400, 300);
    auto* item = qobject_cast<QQuickItem*>(host.get());
    item->setParentItem(window.contentItem());
    QQmlExpression setup(QQmlEngine::contextForObject(host.get()), host.get(),
        QStringLiteral("viewMenuBtn.x = 350; viewMenuBtn.width = 40;"
                       "viewMenuBtn.height = 40; true"));
    setup.evaluate();
    window.show();
    QCoreApplication::processEvents();
    const auto visible = [&] {
        QQmlExpression expression(QQmlEngine::contextForObject(host.get()), host.get(),
                                  QStringLiteral("viewMenuPopup.visible"));
        return expression.evaluate().toBool();
    };
    bool ok = true;
    for (int repeat = 0; repeat < 2; ++repeat) {
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {370, 20});
        ok &= require(QTest::qWaitFor(visible), "hamburger click opens the menu");
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {370, 20});
        ok &= require(QTest::qWaitFor([&] { return !visible(); }),
                      "second hamburger click closes instead of reopening the menu");
    }
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {370, 20});
    ok &= require(QTest::qWaitFor(visible), "hamburger reopens after toggling");
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {20, 200});
    ok &= require(QTest::qWaitFor([&] { return !visible(); }), "outside click still closes menu");
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {370, 20});
    ok &= require(QTest::qWaitFor(visible), "hamburger reopens after outside click");
    QTest::keyClick(&window, Qt::Key_Escape);
    ok &= require(QTest::qWaitFor([&] { return !visible(); }), "Escape still closes menu");
    return ok;
}

bool aioDeckControlTests(const std::string& workspace)
{
    const auto button = componentSection(workspace, "AioActionButton");
    const auto controls = componentSection(workspace, "AioDeckControls");
    if (!require(!button.empty() && !controls.empty(),
                 "AIO transport and common button components exist"))
        return false;
    qmlRegisterType(QUrl::fromLocalFile(QString::fromUtf8(BROCKDJ_SOURCE_DIR)
                        + QStringLiteral("/src/qml/components/Button.qml")),
                    "DJSoftware", 1, 0, "Button");
    QQmlEngine engine;
    const QString sinkSource = QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool hasTrack: true
            property bool isPlaying: false
            property bool keylock: false
            property bool slipActive: false
            property bool quantizeEnabled: false
            property real beatJumpBeats: 4
            property real lastJump: 0
            property int cuePresses: 0
            property int cueReleases: 0
            function togglePlay() { isPlaying = !isPlaying }
            function cueButtonPress() { ++cuePresses }
            function cueButtonRelease() { ++cueReleases }
            function beatJump(beats) { lastJump = beats }
            function setSlip(active) { slipActive = active }
        }
    )");
    auto deck1 = createQmlObject(engine, sinkSource, "AIO deck 1 sink instantiates");
    auto deck2 = createQmlObject(engine, sinkSource, "AIO deck 2 sink instantiates");
    if (!deck1 || !deck2)
        return false;
    engine.rootContext()->setContextProperty("testDeck1", deck1.get());
    engine.rootContext()->setContextProperty("testDeck2", deck2.get());
    QQmlComponent component(&engine);
    component.setData((QStringLiteral(R"(
        import QtQuick
        import QtQuick.Layouts
        import DJSoftware
        Item {
            width: 800; height: 42
    )") + QString::fromStdString(button) + QStringLiteral("\n")
        + QString::fromStdString(controls) + QStringLiteral(R"(
            RowLayout {
                anchors.fill: parent
                AioDeckControls {
                    objectName: "first"; Layout.fillWidth: true
                    engine: testDeck1; deckLabel: "1"; accent: "#168FC4"
                }
                AioDeckControls {
                    objectName: "second"; Layout.fillWidth: true
                    engine: testDeck2; deckLabel: "2"; accent: "#E99128"
                }
            }
        }
    )")).toUtf8(), QUrl());
    std::unique_ptr<QObject> bar(component.create());
    if (!require(bar != nullptr, "production AIO controls instantiate")) {
        std::cerr << component.errorString().toStdString();
        return false;
    }
    QQuickWindow window;
    window.resize(800, 42);
    auto* item = qobject_cast<QQuickItem*>(bar.get());
    item->setParentItem(window.contentItem());
    window.show();
    QCoreApplication::processEvents();
    auto* first = bar->findChild<QObject*>("first");
    auto* second = bar->findChild<QObject*>("second");
    bool ok = require(first && second, "AIO controls have independent deck halves");
    if (!first || !second)
        return false;
    const double buttonWidth = first->findChild<QObject*>("aioPlay")->property("width").toDouble();
    const auto equalButtonSizes = [&] {
        for (auto* half : {first, second}) {
            for (const char* name : {"aioPlay", "aioCue", "aioJumpBack", "aioJumpForward",
                                     "aioKeylock", "aioSlip", "aioQuantize"}) {
                auto* button = half->findChild<QObject*>(name);
                if (!button || std::abs(button->property("width").toDouble() - buttonWidth) > 1.0
                    || button->property("height").toDouble() != 32.0)
                    return false;
            }
        }
        return buttonWidth > 40.0;
    };
    ok &= require(equalButtonSizes(), "AIO buttons divide available width equally");
    for (const auto& mode : {std::pair{"aioKeylock", "keylock"},
                             std::pair{"aioSlip", "slipActive"},
                             std::pair{"aioQuantize", "quantizeEnabled"}}) {
        auto* firstButton = first->findChild<QObject*>(mode.first);
        auto* secondButton = second->findChild<QObject*>(mode.first);
        ok &= clickItem(window, firstButton, "AIO mode button is visible");
        ok &= require(deck1->property(mode.second).toBool()
                          && firstButton->property("checked").toBool()
                          && !deck2->property(mode.second).toBool()
                          && !secondButton->property("checked").toBool(),
                      "AIO mode toggle and active indicator target only deck 1");
        ok &= clickItem(window, firstButton, "AIO mode toggles off");
        ok &= require(!deck1->property(mode.second).toBool()
                          && !firstButton->property("checked").toBool(),
                      "AIO mode can be disabled again");
        ok &= clickItem(window, secondButton, "AIO deck 2 mode button is visible");
        ok &= require(deck2->property(mode.second).toBool()
                          && secondButton->property("checked").toBool()
                          && !deck1->property(mode.second).toBool(),
                      "AIO mode targets deck 2 independently");
        deck1->setProperty(mode.second, true);
        QCoreApplication::processEvents();
        ok &= require(firstButton->property("checked").toBool(),
                      "AIO mode indicator follows external engine changes after clicks");
        deck1->setProperty(mode.second, false);
        deck2->setProperty(mode.second, false);
    }
    QTest::qWait(30);
    ok &= require(equalButtonSizes(), "AIO mode toggles do not resize any buttons");
    ok &= clickItem(window, first->findChild<QObject*>("aioPlay"), "AIO play is visible");
    ok &= require(deck1->property("isPlaying").toBool()
                      && !deck2->property("isPlaying").toBool(), "AIO play targets deck 1 only");
    QTest::qWait(30);
    ok &= require(equalButtonSizes(), "PLAY to PAUSE does not resize AIO buttons");
    ok &= clickItem(window, first->findChild<QObject*>("aioPlay"), "AIO pause is visible");
    ok &= require(!deck1->property("isPlaying").toBool(), "AIO play toggles back to pause");
    deck1->setProperty("beatJumpBeats", 8);
    deck2->setProperty("beatJumpBeats", 0.5);
    QTest::qWait(30);
    ok &= require(equalButtonSizes(), "different beatjump labels do not resize AIO buttons");
    ok &= clickItem(window, first->findChild<QObject*>("aioJumpBack"), "AIO backward jump is visible");
    ok &= clickItem(window, second->findChild<QObject*>("aioJumpForward"), "AIO forward jump is visible");
    ok &= require(deck1->property("lastJump").toDouble() == -8
                      && deck2->property("lastJump").toDouble() == 0.5,
                  "AIO beatjump reads each deck's current side-panel range");
    auto* cue = qobject_cast<QQuickItem*>(first->findChild<QObject*>("aioCue"));
    const QPoint cuePoint = cue->mapToScene(cue->boundingRect().center()).toPoint();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cuePoint);
    QTest::qWait(30);
    ok &= require(equalButtonSizes(), "held CUE styling does not resize AIO buttons");
    ok &= require(deck1->property("cuePresses").toInt() == 1
                      && deck1->property("cueReleases").toInt() == 0,
                  "AIO cue starts on press and remains held");
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, cuePoint);
    ok &= require(deck1->property("cueReleases").toInt() == 1,
                  "AIO cue releases once on mouse release");
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cuePoint);
    item->setVisible(false);
    QCoreApplication::processEvents();
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, cuePoint);
    ok &= require(deck1->property("cueReleases").toInt() == 2,
                  "hiding AIO controls ends held cue without a duplicate release");
    item->setVisible(true);
    auto* touchDevice = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
    QTest::touchEvent(&window, touchDevice).press(0, cuePoint, &window);
    ok &= require(QTest::qWaitFor([&] { return deck1->property("cuePresses").toInt() == 3; }),
                  "touchscreen cue starts on press");
    QTest::touchEvent(&window, touchDevice).release(0, cuePoint, &window);
    ok &= require(QTest::qWaitFor([&] { return deck1->property("cueReleases").toInt() == 3; }),
                  "touchscreen cue releases the held deck");
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cuePoint);
    first->setProperty("engine", QVariant::fromValue(deck2.get()));
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, cuePoint);
    ok &= require(deck1->property("cueReleases").toInt() == 4
                      && deck2->property("cueReleases").toInt() == 0,
                  "rebinding a held CUE releases the original deck only");
    first->setProperty("engine", QVariant::fromValue(deck1.get()));
    deck1->setProperty("hasTrack", false);
    ok &= require(!first->findChild<QObject*>("aioPlay")->property("enabled").toBool()
                      && !first->findChild<QObject*>("aioCue")->property("enabled").toBool(),
                  "unloaded AIO deck controls are disabled");
    for (const char* name : {"aioKeylock", "aioSlip", "aioQuantize"})
        ok &= require(!first->findChild<QObject*>(name)->property("enabled").toBool(),
                      "unloaded AIO mode buttons are disabled");
    deck1->setProperty("hasTrack", true);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cuePoint);
    bar.reset();
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, cuePoint);
    ok &= require(deck1->property("cueReleases").toInt() == 5,
                  "destroying AIO controls releases held CUE");
    return ok;
}

bool crossfaderInlineRuntimeTests(const std::string& developmentControls)
{
    const auto crossfader = componentSection(developmentControls, "CrossfaderBar");
    const auto assignGroup = componentSection(developmentControls, "AssignGroup");
    if (!require(!crossfader.empty() && !assignGroup.empty(),
                 "development crossfader and assignment components exist"))
        return false;

    qmlRegisterType(QUrl::fromLocalFile(QString::fromUtf8(BROCKDJ_SOURCE_DIR)
                        + QStringLiteral("/src/qml/components/Slider.qml")),
                    "DJSoftware", 1, 0, "Slider");
    QQmlEngine engine;
    auto hostWindow = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject { function spViewport(value) { return value } }
    )"), "crossfader host window context instantiates");
    auto mixer = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property real appliedPosition: -2
            property int applyCalls: 0
            function syncCrossfaderState(position, a, b, c, d, sharpness, mode) {
                appliedPosition = position
            }
            function applyAllVolumes() { ++applyCalls }
        }
    )"), "crossfader mixer sink instantiates");
    if (!hostWindow || !mixer)
        return false;
    QQmlContext context(engine.rootContext());
    context.setContextProperty(QStringLiteral("testWindow"), hostWindow.get());
    context.setContextProperty(QStringLiteral("testMixer"), mixer.get());
    context.setContextProperty(QStringLiteral("parameterStore"), static_cast<QObject*>(nullptr));
    context.setContextProperty(QStringLiteral("settingsManager"), static_cast<QObject*>(nullptr));

    const QString source = QStringLiteral(R"(
        import QtQuick
        import QtQuick.Layouts
        import QtQuick.Controls
        import DJSoftware
        Item {
            width: 900; height: 36
    )") + QString::fromStdString(assignGroup) + QStringLiteral("\n")
        + QString::fromStdString(crossfader) + QStringLiteral(R"(
            CrossfaderBar {
                objectName: "hostedCrossfader"
                anchors.fill: parent
                hostWindow: testWindow
                mc: testMixer
            }
        }
    )");
    QQmlComponent component(&engine);
    component.setData(source.toUtf8(), QUrl());
    std::unique_ptr<QObject> host(component.create(&context));
    if (!require(host != nullptr, "development crossfader inline host instantiates")) {
        std::cerr << component.errorString().toStdString() << '\n';
        return false;
    }
    auto* bar = host->findChild<QObject*>(QStringLiteral("hostedCrossfader"));
    bool ok = require(bar != nullptr, "development host owns its inline crossfader");
    if (!bar)
        return false;
    ok &= require(mixer->property("applyCalls").toInt() > 0,
                  "the inline crossfader applies its state through the real mixer contract");
    bar->setProperty("cfPos", 0.35);
    ok &= require(std::abs(mixer->property("appliedPosition").toDouble() - 0.35) < 1e-6,
                  "the hosted crossfader forwards position changes to MixerControl");
    return ok;
}

bool turntableInlineRuntimeTests(const std::string& performancePads)
{
    const auto turntable = componentSection(performancePads, "TurntableIndicator");
    if (!require(!turntable.empty(), "performance platter inline component exists"))
        return false;

    QQmlEngine engine;
    auto deck = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool isPlaying: false
            property bool scratchVisualActive: false
            function getPlayheadPositionAtomic() { return 0 }
            function getVisualPositionQml() { return 0 }
            function pauseForScrub(position) {}
            function scratchBySeconds(delta) {}
            function resumeAfterScrub() {}
            signal playingChanged()
            signal progressChanged()
            signal tempoChanged()
            signal reverseChanged()
            signal trackLoaded()
        }
    )"), "turntable engine context instantiates");
    if (!deck)
        return false;
    engine.rootContext()->setContextProperty(QStringLiteral("testDeck"), deck.get());

    const QString source = QStringLiteral(R"(
        import QtQuick
        Item {
            width: 160; height: 160
    )") + QString::fromStdString(turntable) + QStringLiteral(R"(
            TurntableIndicator {
                objectName: "hostedTurntable"
                width: 120; height: 120
                engine: testDeck
            }
        }
    )");
    auto host = createQmlObject(engine, source, "performance platter inline host instantiates");
    if (!host)
        return false;
    auto* platter = qobject_cast<QQuickItem*>(
        host->findChild<QObject*>(QStringLiteral("hostedTurntable")));
    if (!require(platter != nullptr, "performance pads host owns its inline platter"))
        return false;
    QQuickWindow window;
    window.resize(160, 160);
    qobject_cast<QQuickItem*>(host.get())->setParentItem(window.contentItem());
    window.show();
    QCoreApplication::processEvents();
    bool ok = require(!platter->property("motionActive").toBool(),
                      "an idle inline platter does not run its motion clock");
    deck->setProperty("isPlaying", true);
    QCoreApplication::processEvents();
    ok &= require(platter->property("motionActive").toBool(),
                  "the hosted platter starts motion from its bound deck state");
    return ok;
}

bool appOverlaysRuntimeTests(const std::string& source)
{
    const QString componentDirectory = QString::fromUtf8(BROCKDJ_SOURCE_DIR)
        + QStringLiteral("/src/qml/components/");
    qmlRegisterSingletonType(QUrl::fromLocalFile(
        componentDirectory + QStringLiteral("UiTheme.qml")),
        "DJSoftware", 1, 0, "UiTheme");
    qmlRegisterType(QUrl::fromLocalFile(componentDirectory + QStringLiteral("Button.qml")),
                    "DJSoftware", 1, 0, "Button");
    qmlRegisterType(QUrl::fromLocalFile(QString::fromUtf8(BROCKDJ_SOURCE_DIR)
                        + QStringLiteral("/src/qml/performance/PerformanceBackdrop.qml")),
                    "DJSoftware", 1, 0, "PerformanceBackdrop");
    QQmlEngine engine;
    auto appWindow = createQmlObject(engine, QStringLiteral(R"(
        import QtQuick
        import QtQml
        QtObject {
            property bool exitPromptVisible: false
            property bool exitShutdownInProgress: false
            property bool exitManualBackupRequested: false
            property bool uncleanShutdownWarningVisible: false
            property bool startupReady: false
            property string startupAudioError: ""
            property real exitProgress: 0
            property color unifiedGray: "#202020"
            property string confirmedBackup: ""
            property int cancelCalls: 0
            function sp(value) { return value }
            function _isTextInputFocused() { return false }
            function confirmAppClose() { confirmedBackup = exitManualBackupRequested ? "yes" : "no" }
            function cancelAppClosePrompt() { ++cancelCalls; exitPromptVisible = false }
        }
    )"), "AppOverlays window sink instantiates");
    auto mainLayout = createQmlObject(engine, QStringLiteral(R"(
        import QtQuick
        Item {
            width: 800; height: 600; visible: false
            property color stripeColor: "white"
            Rectangle { anchors.fill: parent; color: "black" }
            Repeater {
                model: 50
                Rectangle {
                    required property int index
                    x: index * 16; width: 8; height: 600
                    color: parent.stripeColor
                }
            }
        }
    )"), "AppOverlays main-layout sink instantiates");
    auto appConfig = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool firstRunCompleted: false
            function completeFirstRun(dontShowAgain) { firstRunCompleted = dontShowAgain }
        }
    )"), "AppOverlays first-run sink instantiates");
    auto libraryDb = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool recoveryWarningNeeded: true
            property string recoveryWarningMessage: "Recovered from an interrupted shutdown"
        }
    )"), "AppOverlays recovery sink instantiates");
    if (!appWindow || !mainLayout || !appConfig || !libraryDb)
        return false;
    QQmlContext context(engine.rootContext());
    context.setContextProperty(QStringLiteral("appWindow"), appWindow.get());
    context.setContextProperty(QStringLiteral("mainLayout"), mainLayout.get());
    context.setContextProperty(QStringLiteral("appConfig"), appConfig.get());
    context.setContextProperty(QStringLiteral("libraryDb"), libraryDb.get());
    QQmlComponent component(&engine);
    component.setData(QByteArray::fromStdString(source), QUrl());
    // Qt 6.4 createWithInitialProperties dereferences its creator if loading failed.
    if (!require(component.isReady(), "AppOverlays QML component loads")) {
        std::cerr << component.errorString().toStdString() << '\n';
        return false;
    }
    const QVariantMap initialProperties {
        {QStringLiteral("appWindow"), QVariant::fromValue(appWindow.get())},
        {QStringLiteral("mainLayout"), QVariant::fromValue(mainLayout.get())}
    };
    std::unique_ptr<QObject> host(component.createWithInitialProperties(initialProperties, &context));
    if (!require(host != nullptr, "AppOverlays QML host instantiates with its real local components")) {
        std::cerr << component.errorString().toStdString() << '\n';
        return false;
    }

    const auto findWithProperty = [host = host.get()](const char* name) -> QObject* {
        const auto visit = [&name](auto&& self, QObject* object) -> QObject* {
            if (object->metaObject()->indexOfProperty(name) >= 0)
                return object;
            for (QObject* child : object->children()) {
                if (QObject* found = self(self, child))
                    return found;
            }
            return nullptr;
        };
        return visit(visit, host);
    };
    auto* startupObject = findWithProperty("welcomeActive");
    QObject* statusItem = nullptr;
    for (QObject* child : host->children()) {
        if (child->metaObject()->indexOfProperty("appWindow") >= 0
            && child->metaObject()->indexOfProperty("uncleanShutdownWarning") >= 0
            && child->metaObject()->indexOfProperty("welcomeActive") < 0) {
            statusItem = child;
            break;
        }
    }
    QObject* exitObject = nullptr;
    for (QObject* child : host->children()) {
        if (child->metaObject()->indexOfProperty("appWindow") >= 0
            && child->metaObject()->indexOfProperty("welcomeActive") < 0
            && child->metaObject()->indexOfProperty("uncleanShutdownWarning") < 0) {
            exitObject = child;
            break;
        }
    }
    auto* startupItem = qobject_cast<QQuickItem*>(startupObject);
    auto* exitItem = qobject_cast<QQuickItem*>(exitObject);
    if (!require(startupItem && statusItem && exitItem,
                 "AppOverlays exposes all three independently hosted local components"))
        return false;
    QObject* loadingTimer = nullptr;
    const auto findFinishLoading = [&loadingTimer](auto&& self, QObject* object) -> void {
        if (object->metaObject()->indexOfMethod("finishLoading()") >= 0) {
            loadingTimer = object;
            return;
        }
        for (QObject* child : object->children()) {
            if (!loadingTimer)
                self(self, child);
        }
    };
    findFinishLoading(findFinishLoading, host.get());
    if (!require(loadingTimer != nullptr, "live startup overlay exposes its loading completion timer"))
        return false;

    QQuickWindow window;
    window.resize(800, 600);
    qobject_cast<QQuickItem*>(mainLayout.get())->setParentItem(window.contentItem());
    qobject_cast<QQuickItem*>(host.get())->setParentItem(window.contentItem());
    qobject_cast<QQuickItem*>(host.get())->setSize(QSizeF(800, 600));
    window.show();
    QCoreApplication::processEvents();
    bool ok = require(!mainLayout->property("visible").toBool()
                          && std::abs(qobject_cast<QQuickItem*>(host.get())->z() - 1000.0) < 1e-6
                          && std::abs(exitItem->z() - 1000.0) < 1e-6
                          && std::abs(statusItem->property("z").toDouble()) < 1e-6,
                      "the live overlay host retains its startup gate and independent local layer ordering");

    QMetaObject::invokeMethod(loadingTimer, "finishLoading");
    ok &= require(!mainLayout->property("visible").toBool()
                      && !startupItem->property("welcomeActive").toBool(),
                  "startup overlay cannot report success before runtime readiness");
    appWindow->setProperty("startupReady", true);
    QMetaObject::invokeMethod(loadingTimer, "finishLoading");
    ok &= require(mainLayout->property("visible").toBool()
                      && startupItem->property("welcomeActive").toBool()
                      && appWindow->property("uncleanShutdownWarningVisible").toBool(),
                  "runtime readiness reveals the real layout, first-run welcome and recovery warning");
    appWindow->setProperty("startupAudioError", QStringLiteral("No output device is active."));
    QCoreApplication::processEvents();
    QObject* audioWarningText = nullptr;
    const auto findAudioWarning = [&audioWarningText](auto&& self, QObject* object) -> void {
        if (object->property("text").toString().startsWith(QStringLiteral("Audio is unavailable:"))) {
            audioWarningText = object;
            return;
        }
        for (QObject* child : object->children()) {
            if (!audioWarningText)
                self(self, child);
        }
    };
    findAudioWarning(findAudioWarning, host.get());
    ok &= require(audioWarningText && audioWarningText->property("visible").toBool()
                      && mainLayout->property("visible").toBool(),
                  "audio initialization errors remain visible without blocking the ready application");
    appWindow->setProperty("uncleanShutdownWarningVisible", true);
    QCoreApplication::processEvents();
    QObject* warning = statusItem->property("uncleanShutdownWarning").value<QObject*>();
    ok &= require(warning && warning->property("visible").toBool()
                      && warning->property("visibleMessage").toString()
                          == QStringLiteral("Recovered from an interrupted shutdown"),
                  "startup recovery state reaches the status overlay's live warning object");
    appWindow->setProperty("exitPromptVisible", true);
    QCoreApplication::processEvents();
    ok &= require(exitItem->isVisible() && exitItem->property("focus").toBool(),
                  "exit prompt visibility and focus follow the application window");
    auto* backdrop = exitItem->findChild<QObject*>("exitBackdrop");
    if (!require(backdrop != nullptr, "exit uses the shared GPU backdrop"))
        return false;
    ok &= require(backdrop->property("sourceItem").value<QQuickItem*>()
                      == qobject_cast<QQuickItem*>(mainLayout.get())
                      && !backdrop->property("live").toBool()
                      && backdrop->property("maximumCaptureSize").toSize() == QSize(512, 512),
                  "exit captures only mainLayout into bounded frozen textures");
    const bool software = window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    if (software)
        ok &= require(!backdrop->property("blurActive").toBool(),
                      "software exit has an opaque readable fallback");
    QQmlExpression activate(QQmlEngine::contextForObject(backdrop), backdrop,
                            QStringLiteral("graphicsAvailable = true; applicationActive = true"));
    activate.evaluate();
    QCoreApplication::processEvents();
    QPointer<QObject> capture(backdrop->findChild<QObject*>("backdropCapture"));
    ok &= require(capture && !capture->property("live").toBool()
                      && !capture->property("recursive").toBool(),
                  "exit creates a nonrecursive one-shot GPU capture");
    if (!software) {
        startupItem->setProperty("welcomeActive", false);
        appWindow->setProperty("uncleanShutdownWarningVisible", false);
        QTest::qWait(80);
        const QImage first = window.grabWindow();
        if (!require(!first.isNull(), "exit blur renders a framebuffer"))
            return false;
        const QColor firstPixel = first.pixelColor(36, 40);
        ok &= require(std::abs(firstPixel.red() - first.pixelColor(44, 40).red()) < 30,
                      "exit really blurs stripes instead of merely dimming them");
        mainLayout->setProperty("stripeColor", QColor(Qt::red));
        QTest::qWait(50);
        const QImage frozen = window.grabWindow();
        ok &= require(!frozen.isNull() && frozen.pixelColor(36, 40) == firstPixel,
                      "exit snapshot does not recapture animated backing content");
        appWindow->setProperty("exitPromptVisible", false);
        QCoreApplication::processEvents();
        appWindow->setProperty("exitPromptVisible", true);
        QTest::qWait(50);
        const QImage reopened = window.grabWindow();
        ok &= require(!reopened.isNull() && reopened.pixelColor(36, 40).green() < firstPixel.green(),
                      "reopening exit takes a fresh snapshot");
        mainLayout->setProperty("stripeColor", QColor(Qt::blue));
        qobject_cast<QQuickItem*>(mainLayout.get())->setSize(QSizeF(640, 480));
        qobject_cast<QQuickItem*>(host.get())->setSize(QSizeF(640, 480));
        window.resize(640, 480);
        QTest::qWait(80);
        const QImage resized = window.grabWindow();
        ok &= require(!resized.isNull()
                          && resized.pixelColor(36, 40).blue() > resized.pixelColor(36, 40).red()
                          && backdrop->property("captureRect").toRectF().size() == QSizeF(640, 480),
                      "resizing refreshes the frozen exit snapshot to current geometry and content");
    }
    appWindow->setProperty("exitShutdownInProgress", true);
    QCoreApplication::processEvents();
    ok &= require(QTest::qWaitFor([&] { return capture.isNull(); })
                      && !backdrop->property("sourceItem").value<QQuickItem*>(),
                  "shutdown detaches the source and destroys capture before layout teardown");
    appWindow->setProperty("exitShutdownInProgress", false);
    QCoreApplication::processEvents();
    appWindow->setProperty("exitPromptVisible", false);
    QCoreApplication::processEvents();
    ok &= require(!exitItem->isVisible(),
                  "closing the prompt leaves the status and startup overlays independent");
    return ok;
}

bool shortcutRuntimeTests(const std::string& source)
{
    const auto shortcuts = componentSection(source, "UiShortcutManager");
    if (!require(!shortcuts.empty(), "main owns a local shortcut component"))
        return false;
    QQmlEngine engine;
    auto appWindow = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool textFocused: false
            function _isTextInputFocused() { return textFocused }
        }
    )"), "shortcut focus sink instantiates");
    auto zoom = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property int resetCalls: 0
            function zoomIn() {}
            function zoomOut() {}
            function reset() { ++resetCalls }
        }
    )"), "waveform shortcut sink instantiates");
    auto scale = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property int resetCalls: 0
            function increase() {}
            function decrease() {}
            function reset() { ++resetCalls }
        }
    )"), "scale shortcut sink instantiates");
    if (!appWindow || !zoom || !scale)
        return false;
    QQmlContext context(engine.rootContext());
    context.setContextProperty(QStringLiteral("testWindow"), appWindow.get());
    context.setContextProperty(QStringLiteral("waveformZoomController"), zoom.get());
    context.setContextProperty(QStringLiteral("uiScaleController"), scale.get());

    const QString hostSource = QStringLiteral(R"(
        import QtQuick
        Item {
            width: 200; height: 100
    )") + QString::fromStdString(shortcuts) + QStringLiteral(R"(
            UiShortcutManager {
                appWindow: testWindow
            }
        }
    )");
    QQmlComponent component(&engine);
    component.setData(hostSource.toUtf8(), QUrl());
    std::unique_ptr<QObject> host(component.create(&context));
    if (!require(host != nullptr, "real inline shortcut declarations register in a host")) {
        std::cerr << component.errorString().toStdString() << '\n';
        return false;
    }
    QQuickWindow window;
    window.resize(200, 100);
    qobject_cast<QQuickItem*>(host.get())->setParentItem(window.contentItem());
    window.show();
    window.requestActivate();
    QCoreApplication::processEvents();

    QTest::keyClick(&window, Qt::Key_0, Qt::ControlModifier);
    bool ok = require(QTest::qWaitFor([&] { return zoom->property("resetCalls").toInt() == 1; }),
                      "registered Ctrl+0 shortcut invokes waveform reset");
    appWindow->setProperty("textFocused", true);
    QTest::keyClick(&window, Qt::Key_0, Qt::ControlModifier);
    QTest::qWait(30);
    ok &= require(zoom->property("resetCalls").toInt() == 1,
                  "global waveform shortcut is suppressed while a text input is focused");
    appWindow->setProperty("textFocused", false);
    QTest::keyClick(&window, Qt::Key_0, Qt::ControlModifier | Qt::ShiftModifier);
    ok &= require(QTest::qWaitFor([&] { return scale->property("resetCalls").toInt() == 1; }),
                  "registered Ctrl+Shift+0 shortcut invokes UI-scale reset");
    return ok;
}

bool deckCueFlatButtonRuntimeTests(const std::string& deckControl)
{
    const auto flatButton = componentSection(deckControl, "FlatBtn");
    QString releaseFunction;
    if (!require(!flatButton.empty()
                     && extractFunctions(deckControl, {"releaseCue"}, releaseFunction),
                 "DeckControl FlatBtn and captured-engine release function are extractable"))
        return false;

    QQmlEngine engine;
    const QString sinkSource = QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool hasTrack: true
            property int cuePresses: 0
            property int cueReleases: 0
            function cueButtonPress() { ++cuePresses }
            function cueButtonRelease() { ++cueReleases }
        }
    )");
    auto deck1 = createQmlObject(engine, sinkSource, "DeckControl cue sink one instantiates");
    auto deck2 = createQmlObject(engine, sinkSource, "DeckControl cue sink two instantiates");
    if (!deck1 || !deck2)
        return false;
    engine.rootContext()->setContextProperty(QStringLiteral("testDeck1"), deck1.get());
    engine.rootContext()->setContextProperty(QStringLiteral("testDeck2"), deck2.get());

    const QString hostSource = QStringLiteral(R"(
        import QtQuick
        import QtQuick.Layouts
        Item {
            id: deck
            width: 100; height: 40
            property var engine: testDeck1
            property var heldCueEngine: null
            readonly property color btnLineActive: "#aaaaaa"
            readonly property color btnTextActive: "#f0f0f0"
            readonly property color btnTextBright: "#b8b8b8"
            readonly property color btnBgPressed: "#444444"
            readonly property color btnBgActive: "#333333"
            readonly property color btnBg: "#222222"
            readonly property int btnH: 24
    )") + releaseFunction + QStringLiteral(R"(
            onVisibleChanged: if (!visible) releaseCue()
            onEngineChanged: releaseCue()
            Component.onDestruction: releaseCue()
    )") + QString::fromStdString(flatButton) + QStringLiteral(R"(
            FlatBtn {
                id: cue
                objectName: "deckCueButton"
                x: 10; y: 8; width: 64; height: 24
                btnText: "CUE"
                enabled: deck.engine && deck.engine.hasTrack
                onBtnPressed: {
                    if (!deck.engine || deck.heldCueEngine)
                        return
                    deck.heldCueEngine = deck.engine
                    deck.heldCueEngine.cueButtonPress()
                }
                onBtnReleased: deck.releaseCue()
                onBtnCanceled: deck.releaseCue()
                onEnabledChanged: if (!enabled) deck.releaseCue()
            }
        }
    )");
    QQmlComponent component(&engine);
    component.setData(hostSource.toUtf8(), QUrl());
    std::unique_ptr<QObject> host(component.create());
    if (!require(host != nullptr, "real DeckControl FlatBtn instantiates with its release owner")) {
        std::cerr << component.errorString().toStdString() << '\n';
        return false;
    }
    auto* owner = qobject_cast<QQuickItem*>(host.get());
    auto* cue = qobject_cast<QQuickItem*>(
        host->findChild<QObject*>(QStringLiteral("deckCueButton")));
    if (!require(owner && cue, "DeckControl host exposes its real local CUE FlatBtn"))
        return false;
    QQuickWindow window;
    window.resize(100, 40);
    owner->setParentItem(window.contentItem());
    window.show();
    QCoreApplication::processEvents();
    const QPoint point = cue->mapToScene(cue->boundingRect().center()).toPoint();
    bool ok = true;

    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point);
    ok &= require(deck1->property("cuePresses").toInt() == 1
                      && deck1->property("cueReleases").toInt() == 0,
                  "DeckControl CUE captures and presses the original engine");
    owner->setProperty("engine", QVariant::fromValue(deck2.get()));
    ok &= require(deck1->property("cueReleases").toInt() == 1
                      && deck2->property("cueReleases").toInt() == 0,
                  "changing the bound engine releases only the captured original engine");
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point);

    QObject* cueMouseArea = nullptr;
    for (QObject* child : cue->findChildren<QObject*>()) {
        if (QString::fromLatin1(child->metaObject()->className()).contains("MouseArea")) {
            cueMouseArea = child;
            break;
        }
    }
    ok &= require(cueMouseArea != nullptr, "DeckControl FlatBtn contains its real mouse input handler");
    if (!cueMouseArea)
        return false;
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point);
    auto* cueInput = qobject_cast<QQuickItem*>(cueMouseArea);
    cueInput->ungrabMouse();
    QCoreApplication::processEvents();
    ok &= require(deck2->property("cuePresses").toInt() == 1
                      && deck2->property("cueReleases").toInt() == 1,
                  "canceling the FlatBtn mouse grab releases its held engine");
    cueMouseArea->setProperty("enabled", true);

    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point);
    owner->setVisible(false);
    QCoreApplication::processEvents();
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point);
    ok &= require(deck2->property("cuePresses").toInt() == 2
                      && deck2->property("cueReleases").toInt() == 2,
                  "hiding a held DeckControl cancels CUE and releases it exactly once");
    owner->setVisible(true);

    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point);
    deck2->setProperty("hasTrack", false);
    QCoreApplication::processEvents();
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point);
    ok &= require(deck2->property("cuePresses").toInt() == 3
                      && deck2->property("cueReleases").toInt() == 3,
                  "disabling a held DeckControl CUE releases its engine once");
    deck2->setProperty("hasTrack", true);

    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point);
    owner->setParentItem(nullptr);
    host.reset();
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point);
    ok &= require(deck2->property("cuePresses").toInt() == 4
                      && deck2->property("cueReleases").toInt() == 4,
                  "destroying DeckControl while CUE is held releases the captured engine");
    return ok;
}

bool qmlMergeRuntimeTests()
{
    const QString qmlDirectory = QString::fromUtf8(BROCKDJ_SOURCE_DIR)
        + QStringLiteral("/src/qml/");
    qmlRegisterType<ScrollingWaveformItemStub>("DJSoftware", 1, 0,
                                               "ScrollingWaveformItem");
    qmlRegisterType(QUrl::fromLocalFile(qmlDirectory + QStringLiteral("components/Knob.qml")),
                    "DJSoftware", 1, 0, "Knob");
    qmlRegisterType(QUrl::fromLocalFile(qmlDirectory
                        + QStringLiteral("waveform/EnlargedWaveform.qml")),
                    "DJSoftware", 1, 0, "EnlargedWaveform");
    qmlRegisterType(QUrl::fromLocalFile(qmlDirectory
                        + QStringLiteral("performance/PerformanceBackdrop.qml")),
                    "DJSoftware", 1, 0, "PerformanceBackdrop");

    QQmlEngine engine;
    auto windowContext = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject { property int fxBarHeight: 96 }
    )"), "QML merge window context instantiates");
    auto fxManager = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool syncEnabled1: false
            property bool syncEnabled2: false
            property real beatDiv1: 0.25
            property real beatDiv2: 0.25
            property real displayBpm1: 120
            property real displayBpm2: 120
            property real primaryParam1: 0.5
            property real primaryParam2: 0.5
            property bool enabled1: false
            property bool enabled2: false
            property string effectType1: "---"
            property string effectType2: "---"
            property string soundColorMode: "Filter"
            property real soundColorParam: 0.5
            property real wetDry1: 0
            property bool deck1A: false
            property bool deck1B: false
            function setEffectType(unit, value) {
                if (unit === 1) effectType1 = value; else effectType2 = value
            }
            function setDeckAssignment(unit, deck, value) {
                if (unit === 1 && deck === 1) deck1A = value
                if (unit === 1 && deck === 2) deck1B = value
            }
            function setWetDry(unit, value) { if (unit === 1) wetDry1 = value }
            function unitEnabled(unit) { return unit === 1 ? enabled1 : enabled2 }
            function setUnitEnabled(unit, value) {
                if (unit === 1) enabled1 = value; else enabled2 = value
            }
            function setSyncEnabled(unit, value) {
                if (unit === 1) syncEnabled1 = value; else syncEnabled2 = value
            }
            function setBeatDivision(unit, value) {
                if (unit === 1) beatDiv1 = value; else beatDiv2 = value
            }
            function setPrimaryParam(unit, value) {
                if (unit === 1) primaryParam1 = value; else primaryParam2 = value
            }
            function setSoundColorMode(value) { soundColorMode = value }
            function setSoundColorParam(value) { soundColorParam = value }
        }
    )"), "QML merge FX context instantiates");
    auto waveformZoom = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property real zoomFraction: 0.5
            property string zoomLabel: "1.0x"
            function zoomIn() {}
            function zoomOut() {}
        }
    )"), "QML merge zoom context instantiates");
    auto deviceLibrary = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject { property var devices: [] }
    )"), "QML merge device context instantiates");
    auto deckA = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool hasTrack: true
            property bool readOnlyExternalTrack: false
            property bool isPlaying: false
            property string externalSourceId: ""
            property string trackKey: "Am"
            property real currentBpm: 128.4
            property real beatJumpBeats: 4
            property bool quantizeEnabled: false
            property bool beatgridLocked: false
            property real pixelsPerSecond: 0
            property var trackData: ({ isBpmAnalyzed: true, bpm: 128.4 })
            property int ejectCalls: 0
            property int lastBeatNudge: 0
            function ejectTrack() { ++ejectCalls }
            function beatJump(value) {}
            function halveBpm() {}
            function doubleBpm() {}
            function nudgeBeatgridBeats(value) { lastBeatNudge = value }
            function nudgeBeatgridMs(value) {}
            function setDownbeatAtCurrentPosition() {}
            function setManualBpm(value) {}
        }
    )"), "QML merge deck A context instantiates");
    auto slipEngine = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool isPlaying: false
            property bool scratchVisualActive: false
            property bool slipPreviewActive: true
            property real pixelsPerSecond: 0
        }
    )"), "slip-preview test engine instantiates");
    auto deckB = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool hasTrack: true
            property bool readOnlyExternalTrack: false
            property bool isPlaying: false
            property string externalSourceId: ""
            property string trackKey: "C"
            property real currentBpm: 124
            property real beatJumpBeats: 8
            property bool quantizeEnabled: false
            property bool beatgridLocked: false
            property real pixelsPerSecond: 0
            property var trackData: ({ isBpmAnalyzed: true, bpm: 124 })
            property int ejectCalls: 0
            property int lastBeatNudge: 0
            function ejectTrack() { ++ejectCalls }
            function beatJump(value) {}
            function halveBpm() {}
            function doubleBpm() {}
            function nudgeBeatgridBeats(value) { lastBeatNudge = value }
            function nudgeBeatgridMs(value) {}
            function setDownbeatAtCurrentPosition() {}
            function setManualBpm(value) {}
        }
    )"), "QML merge deck B context instantiates");
    auto settingsManager = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property int waveformRenderStyle: 0
            property real waveformPlayheadPosition: 0.5
        }
    )"), "waveform settings context instantiates");
    auto pressure = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject { property string tier: "normal" }
    )"), "backdrop pressure context instantiates");
    if (!windowContext || !fxManager || !waveformZoom || !deviceLibrary
        || !deckA || !deckB || !settingsManager || !slipEngine || !pressure)
        return false;

    QQmlContext context(engine.rootContext());
    context.setContextProperty(QStringLiteral("window"), windowContext.get());
    context.setContextProperty(QStringLiteral("settingsManager"), settingsManager.get());
    context.setContextProperty(QStringLiteral("renderPressurePolicy"), pressure.get());
    context.setContextProperty(QStringLiteral("fxManager"), fxManager.get());
    context.setContextProperty(QStringLiteral("waveformZoomController"), waveformZoom.get());
    context.setContextProperty(QStringLiteral("deviceLibraryManager"),
                               deviceLibrary.get());

    bool ok = true;
    QQuickWindow performanceWindow;
    performanceWindow.resize(900, 600);
    QQmlComponent performanceComponent(&engine, QUrl::fromLocalFile(
        qmlDirectory + QStringLiteral("performance/PerformanceWaveformScreen.qml")));
    std::unique_ptr<QObject> performance(performanceComponent.create(&context));
    if (!require(performance != nullptr, "performance waveform host instantiates")) {
        std::cerr << performanceComponent.errorString().toStdString() << '\n';
        return false;
    }
    performance->setProperty("deckAEngine", QVariant::fromValue(deckA.get()));
    performance->setProperty("deckBEngine", QVariant::fromValue(deckB.get()));
    performance->setProperty("fx", QVariant::fromValue(fxManager.get()));
    auto* performanceItem = qobject_cast<QQuickItem*>(performance.get());
    if (!require(performanceItem != nullptr, "performance waveform host is visual"))
        return false;
    performanceItem->setParentItem(performanceWindow.contentItem());
    performanceItem->setSize(QSizeF(900, 600));
    performanceWindow.show();
    QCoreApplication::processEvents();

    QObject* quickA = performance->findChild<QObject*>(QStringLiteral("deckQuickPanelA"));
    QObject* quickB = performance->findChild<QObject*>(QStringLiteral("deckQuickPanelB"));
    ok &= require(quickA != nullptr && quickB != nullptr,
                  "the screen owns two separate quick-panel instances");
    ok &= require(quickB && QMetaObject::invokeMethod(quickB, "selectedRequested")
                      && performance->property("selectedDeck").toString() == QStringLiteral("B"),
                  "deck B quick-panel selection updates the screen's selected deck");
    if (quickA)
        QMetaObject::invokeMethod(quickA, "selectedRequested");
    ok &= require(performance->property("selectedDeck").toString() == QStringLiteral("A"),
                  "deck A quick-panel selection remains independent");

    QObject* ejectArea = performance->findChild<QObject*>(QStringLiteral("deckEjectArea"));
    ok &= require(ejectArea && ejectArea->property("enabled").toBool(),
                  "a stopped deck exposes its eject action");
    deckA->setProperty("isPlaying", true);
    QCoreApplication::processEvents();
    ok &= require(ejectArea && !ejectArea->property("enabled").toBool(),
                  "a playing deck gates its eject action");
    deckA->setProperty("isPlaying", false);

    QObject* jumpMinus = performance->findChild<QObject*>(QStringLiteral("beatJumpMinus"));
    ok &= clickItem(performanceWindow, jumpMinus, "the real Beat Jump minus control is visible");
    ok &= require(std::abs(deckA->property("beatJumpBeats").toDouble() - 2.0) < 1e-6,
                  "the embedded quick panel changes its deck's Beat Jump range");

    QMetaObject::invokeMethod(performance.get(), "openGrid");
    QCoreApplication::processEvents();
    QObject* gridPanel = performance->findChild<QObject*>(
        QStringLiteral("beatgridPanelInstance"));
    QObject* gridNudge = performance->findChild<QObject*>(
        QStringLiteral("gridNudgeMinusBeat"));
    ok &= require(gridPanel && gridPanel->property("visible").toBool(),
                  "the beatgrid inline panel is shown by its real host");
    ok &= clickItem(performanceWindow, gridNudge,
                    "the real beatgrid nudge control is visible");
    ok &= require(deckA->property("lastBeatNudge").toInt() == -1,
                  "the beatgrid button dispatches its nudge to the selected engine");
    QObject* gridLock = performance->findChild<QObject*>(
        QStringLiteral("gridLockButton"));
    ok &= clickItem(performanceWindow, gridLock,
                    "the real beatgrid lock control is visible");
    ok &= require(deckA->property("beatgridLocked").toBool(),
                  "the beatgrid lock control updates its selected engine");
    ok &= require(gridPanel && QMetaObject::invokeMethod(gridPanel, "closeRequested")
                      && performance->property("leftPanel").toString() == QStringLiteral("closed"),
                  "the beatgrid close signal reaches the screen owner");
    performance->setProperty("rightPanelOpen", true);
    QCoreApplication::processEvents();
    QObject* beatFxPanel = performance->findChild<QObject*>(
        QStringLiteral("beatFxPanelInstance"));
    ok &= require(beatFxPanel && QTest::qWaitFor([&] {
                      return beatFxPanel->property("visible").toBool();
                  }),
                  "the right-side Beat FX panel remains host-controlled");

    // Resize the existing instances: construction at 900x600 hid shrinking rows.
    const auto checkPanelGeometry = [&](QObject* panel, const char* label) {
        bool valid = true;
        if (!panel)
            return false;
        for (auto* item : panel->findChildren<QQuickItem*>()) {
            const QByteArray type(item->metaObject()->className());
            if (type.contains("ColumnLayout") || type.contains("GridLayout")) {
                for (auto* row : item->childItems()) {
                    if (row->width() <= 0 || row->height() <= 0)
                        continue;
                    valid &= row->y() >= -0.5
                        && row->y() + row->height() <= item->height() + 0.5;
                    for (auto* other : item->childItems()) {
                        if (other == row || other->width() <= 0 || other->height() <= 0)
                            continue;
                        const QRectF bounds(row->position(), row->size());
                        const QRectF otherBounds(other->position(), other->size());
                        const auto overlap = bounds.intersected(otherBounds);
                        valid &= overlap.width() <= 0.5 || overlap.height() <= 0.5;
                    }
                }
            }
            if (type.startsWith("QQuickText") && item->parentItem() && item->isVisible()) {
                valid &= item->y() >= -0.5 && item->x() >= -0.5
                    && item->x() + item->width() <= item->parentItem()->width() + 0.5
                    && item->y() + item->height() <= item->parentItem()->height() + 0.5;
            }
        }
        if (!valid)
            std::cerr << label << " overflows at viewport "
                      << performanceItem->width() << 'x' << performanceItem->height()
                      << ", panel height=" << panel->property("height").toDouble() << '\n';
        return require(valid, label);
    };
    const auto revealAction = [&](QObject* panel, QQuickItem* action) {
        if (!panel || !action)
            return false;
        auto* scroll = panel->findChild<QQuickItem*>(
            panel == gridPanel ? QStringLiteral("gridScroll")
                : (panel == beatFxPanel ? QStringLiteral("fxScroll")
                                       : QStringLiteral("deckScroll")));
        if (!scroll)
            return false;
        auto* content = scroll->property("contentItem").value<QQuickItem*>();
        if (!content)
            return false;
        const qreal actionY = action->mapToItem(content, QPointF()).y();
        const qreal maximum = std::max(0.0,
            scroll->property("contentHeight").toDouble() - scroll->height());
        scroll->setProperty("contentY", std::clamp(
            actionY - (scroll->height() - action->height()) * 0.5, 0.0, maximum));
        QCoreApplication::processEvents();
        const QRectF bounds = action->mapRectToItem(scroll, QRectF(QPointF(), action->size()));
        return bounds.top() >= -0.5 && bounds.bottom() <= scroll->height() + 0.5
            && action->height() >= 24 && bounds.left() >= -0.5
            && bounds.right() <= scroll->width() + 0.5
            && scroll->property("contentWidth").toDouble() == scroll->width()
            && scroll->clip();
    };
    const auto checkReachableActions = [&](QObject* panel) {
        bool reachable = true;
        for (auto* item : panel->findChildren<QQuickItem*>()) {
            if (QByteArray(item->metaObject()->className()).startsWith("QQuickMouseArea")
                && item->isVisible() && item->isEnabled()) {
                // ScrollBar input belongs to the viewport, not the scrolling content.
                if (item->parentItem()->metaObject()->className()
                    == QByteArray("QQuickScrollBar"))
                    continue;
                reachable &= revealAction(panel, item);
            }
        }
        return require(reachable, "every sidebar action is reachable without horizontal scrolling");
    };
    for (int width : {800, 1280, 1920}) {
        for (int height : {180, 240, 320, 420, 800, 2160, 240, 600}) {
            performanceWindow.resize(width, height);
            performanceItem->setSize(QSizeF(width, height));
            performance->setProperty("leftPanel", "deck");
            QTest::qWait(220);
            ok &= checkPanelGeometry(quickA, "deck A rows stay inside their content");
            ok &= checkPanelGeometry(quickB, "deck B rows stay inside their content");
            ok &= checkPanelGeometry(beatFxPanel, "FX rows do not squeeze below their text");
            ok &= checkReachableActions(quickA);
            ok &= checkReachableActions(quickB);
            ok &= checkReachableActions(beatFxPanel);
            ok &= require(std::abs(quickA->property("height").toDouble()
                                      - performance->property("deckAHeight").toDouble()) < 0.5
                              && std::abs(quickB->property("y").toDouble()
                                      - performance->property("deckBY").toDouble()) < 0.5,
                          "quick panels remain aligned with both waveform boundaries");
            auto* quantize = quickB->findChild<QQuickItem*>(QStringLiteral("quantizeRow"));
            const bool previousQuantize = deckB->property("quantizeEnabled").toBool();
            ok &= revealAction(quickB, quantize)
                && clickItem(performanceWindow, quantize, "scrolled deck B quantize is clickable");
            ok &= require(deckB->property("quantizeEnabled").toBool() != previousQuantize,
                          "deck B bottom action stays functional after resizing");
            auto* engage = beatFxPanel->findChild<QQuickItem*>(QStringLiteral("fxEngageRow"));
            const bool previousEnabled = fxManager->property("enabled1").toBool();
            ok &= revealAction(beatFxPanel, engage)
                && clickItem(performanceWindow, engage, "scrolled FX engage is clickable");
            ok &= require(fxManager->property("enabled1").toBool() != previousEnabled,
                          "FX bottom action stays functional after resizing");
            performance->setProperty("leftPanel", "grid");
            QCoreApplication::processEvents();
            ok &= checkPanelGeometry(gridPanel, "beatgrid rows stay inside their content");
            ok &= checkReachableActions(gridPanel);
            const bool previousLocked = deckA->property("beatgridLocked").toBool();
            ok &= revealAction(gridPanel, qobject_cast<QQuickItem*>(gridLock))
                && clickItem(performanceWindow, gridLock, "scrolled grid lock is clickable");
            ok &= require(deckA->property("beatgridLocked").toBool() != previousLocked,
                          "beatgrid bottom action stays functional after resizing");
            if (width == 800 && height == 180) {
                std::cout << "Short sidebar geometry: deck viewport="
                          << quickA->findChild<QObject*>(QStringLiteral("deckScroll"))
                                 ->property("height").toDouble()
                          << ", deck content="
                          << quickA->findChild<QObject*>(QStringLiteral("deckScroll"))
                                 ->property("contentHeight").toDouble()
                          << ", grid content="
                          << gridPanel->findChild<QObject*>(QStringLiteral("gridScroll"))
                                 ->property("contentHeight").toDouble()
                          << ", FX content="
                          << beatFxPanel->findChild<QObject*>(QStringLiteral("fxScroll"))
                                 ->property("contentHeight").toDouble() << '\n';
            }
        }
    }
    performanceItem->setSize(QSizeF(900, 600));
    performanceWindow.resize(900, 600);

    auto* backing = performance->findChild<QQuickItem*>(QStringLiteral("waveformBacking"));
    auto* leftBackdrop = performance->findChild<QQuickItem*>(QStringLiteral("leftBackdrop"));
    auto* rightBackdrop = performance->findChild<QQuickItem*>(QStringLiteral("rightBackdrop"));
    ok &= require(backing && backing->findChildren<ScrollingWaveformItemStub*>().size() == 2,
                  "the isolated blur source contains both waveform renderers");
    for (auto* backdrop : {leftBackdrop, rightBackdrop}) {
        if (!require(backdrop != nullptr, "each sidebar owns a scoped backdrop")) {
            ok = false;
            continue;
        }
        ok &= require(backdrop->property("sourceItem").value<QQuickItem*>() == backing
                          && !backing->isAncestorOf(backdrop),
                      "blur captures the waveform layer, never itself or its panel");
        auto* loader = backdrop->findChild<QObject*>(QStringLiteral("backdropEffects"));
        if (!require(loader != nullptr, "backdrop has a lazy effect loader")) {
            ok = false;
            continue;
        }
        backdrop->setProperty("graphicsAvailable", false);
        QCoreApplication::processEvents();
        ok &= require(!loader->property("active").toBool()
                          && !loader->property("item").value<QObject*>(),
                      "software rendering allocates no decorative captures or shader effects");
        // Exercise lazy-object contracts on software too; GPU drawing is covered
        // by running this same harness with QT_QUICK_BACKEND=rhi when available.
        QQmlExpression forceBlur(&context, backdrop,
            QStringLiteral("graphicsAvailable = true; applicationActive = true"));
        forceBlur.evaluate();
        ok &= require(!forceBlur.hasError(), "test override removes backend/activity bindings");
        QCoreApplication::processEvents();
        QPointer<QObject> capture(
            backdrop->findChild<QObject*>(QStringLiteral("backdropCapture")));
        QPointer<QObject> intermediate(
            backdrop->findChild<QObject*>(QStringLiteral("backdropIntermediate")));
        const QSize texture = backdrop->property("captureSize").toSize();
        const QRectF crop = backdrop->property("captureRect").toRectF();
        ok &= require(capture && intermediate && texture.width() <= 128 && texture.height() <= 256
                          && capture->property("textureSize").toSize() == texture
                          && intermediate->property("textureSize").toSize() == texture
                          && !capture->property("recursive").toBool()
                          && capture->property("sourceItem").value<QQuickItem*>() == backing
                          && crop.width() <= 230 && crop.height() <= backing->height()
                          && QRectF(QPointF(), backing->size()).contains(crop),
                      "live nonrecursive capture stays cropped and both blur textures are capped");
        performanceItem->setSize(QSizeF(1920, 2160));
        QCoreApplication::processEvents();
        ok &= require(QTest::qWaitFor([&] {
            capture = backdrop->findChild<QObject*>(QStringLiteral("backdropCapture"));
            intermediate = backdrop->findChild<QObject*>(QStringLiteral("backdropIntermediate"));
            const QSize target = backdrop->property("captureSize").toSize();
            return capture && intermediate && target.height() == 256
                && capture->property("textureSize").toSize() == target
                && intermediate->property("textureSize").toSize() == target;
        }), "resized GPU capture bindings settle to the bounded target size");
        const QSize tallTexture = backdrop->property("captureSize").toSize();
        if (!capture || !intermediate) {
            ok &= require(false, "visible backdrop restores bounded capture objects after resizing");
            performanceItem->setSize(QSizeF(900, 600));
            continue;
        }
        ok &= require(tallTexture.width() <= 128 && tallTexture.height() == 256
                          && capture->property("textureSize").toSize() == tallTexture
                          && intermediate->property("textureSize").toSize() == tallTexture,
                      "a tall resized pane keeps both render targets at the hard height cap");
        performanceItem->setSize(QSizeF(900, 600));
        QCoreApplication::processEvents();
        for (const char* tier : {"elevated", "critical", "suspended"}) {
            pressure->setProperty("tier", tier);
            QCoreApplication::processEvents();
            ok &= require(!loader->property("active").toBool()
                              && !loader->property("item").value<QObject*>(),
                          "render pressure unloads all decorative capture/effect objects");
        }
        pressure->setProperty("tier", "normal");
        QCoreApplication::processEvents();
        ok &= require(loader->property("item").value<QObject*>() != nullptr,
                      "normal pressure restores the lazy frosted backdrop");
        backdrop->setProperty("applicationActive", false);
        QCoreApplication::processEvents();
        ok &= require(!loader->property("item").value<QObject*>(),
                      "background application unloads decorative blur");
        backdrop->setProperty("applicationActive", true);
        backdrop->setVisible(false);
        QCoreApplication::processEvents();
        ok &= require(!loader->property("item").value<QObject*>(),
                      "hidden sidebar unloads decorative blur");
        backdrop->setVisible(true);
        backdrop->setProperty("requested", false);
        QCoreApplication::processEvents();
        ok &= require(!loader->property("item").value<QObject*>(),
                      "collapsed sidebar unloads decorative blur immediately");
        backdrop->setProperty("graphicsAvailable", false);
    }

    if (performanceWindow.rendererInterface()->graphicsApi()
        != QSGRendererInterface::Software) {
        QQmlComponent blurProbeComponent(&engine);
        blurProbeComponent.setData(R"(
            import QtQuick
            import DJSoftware
            Item {
                width: 256; height: 128
                Item {
                    id: pattern
                    anchors.fill: parent
                    Repeater {
                        model: 32
                        Rectangle {
                            required property int index
                            x: index * 8; width: 8; height: 128
                            color: index % 2 ? "white" : "black"
                        }
                    }
                }
                PerformanceBackdrop {
                    objectName: "blurProbe"
                    width: 128; height: 128
                    sourceItem: pattern
                    applicationActive: true
                }
            }
        )", QUrl::fromLocalFile(qmlDirectory + QStringLiteral("performance/BlurProbe.qml")));
        std::unique_ptr<QObject> probe(blurProbeComponent.create(&context));
        if (!require(probe != nullptr, "RHI blur probe instantiates")) {
            std::cerr << blurProbeComponent.errorString().toStdString();
            return false;
        }
        QQuickWindow probeWindow;
        probeWindow.resize(256, 128);
        qobject_cast<QQuickItem*>(probe.get())->setParentItem(probeWindow.contentItem());
        probeWindow.show();
        auto* blur = probe->findChild<QObject*>(QStringLiteral("blurProbe"));
        ok &= require(blur && QTest::qWaitFor([&] {
                          return blur->property("blurActive").toBool();
                      }), "RHI renderer enables the actual two-pass backdrop");
        QTest::qWait(100);
        const QImage image = probeWindow.grabWindow();
        const qreal dpr = image.devicePixelRatio();
        const auto intensity = [&](int x) {
            return qGray(image.pixel(QPoint(qRound(x * dpr), qRound(64 * dpr))));
        };
        if (!require(!image.isNull(), "RHI blur probe renders a real framebuffer"))
            return false;
        const int sharpContrast = std::abs(intensity(164) - intensity(172));
        const int blurredContrast = std::abs(intensity(36) - intensity(44));
        ok &= require(sharpContrast > 200 && blurredContrast < 30,
                      "GPU backdrop reduces stripe contrast while the source stays sharp");
        std::cout << "RHI backdrop probe: sharp contrast=" << sharpContrast
                  << ", frosted contrast=" << blurredContrast << '\n';
    }

    QQuickWindow waveformWindow;
    waveformWindow.resize(800, 320);
    QQmlComponent waveformComponent(&engine, QUrl::fromLocalFile(
        qmlDirectory + QStringLiteral("waveform/EnlargedWaveform.qml")));
    std::unique_ptr<QObject> waveform(waveformComponent.create(&context));
    if (!require(waveform != nullptr, "enlarged waveform host instantiates")) {
        std::cerr << waveformComponent.errorString().toStdString() << '\n';
        return false;
    }
    auto* waveformItem = qobject_cast<QQuickItem*>(waveform.get());
    if (!require(waveformItem != nullptr, "enlarged waveform host is visual"))
        return false;
    waveformItem->setParentItem(waveformWindow.contentItem());
    waveformItem->setSize(QSizeF(800, 320));
    waveformWindow.show();
    QCoreApplication::processEvents();
    auto* playhead = waveform->findChild<QQuickItem*>(QStringLiteral("waveformPlayhead"));
    ok &= require(playhead && std::abs(playhead->x() - 400.0) < 1.0
                      && playhead->property("color").value<QColor>() == QColor(Qt::white),
                  "scrolling waveform displays a white centered playhead by default");
    settingsManager->setProperty("waveformPlayheadPosition", 0.25);
    QCoreApplication::processEvents();
    ok &= require(playhead && std::abs(playhead->x() - 200.0) < 1.0,
                  "settings move the playhead left at runtime");
    const auto renderers = waveform->findChildren<ScrollingWaveformItemStub*>();
    ok &= require(!renderers.empty()
                      && std::all_of(renderers.cbegin(), renderers.cend(), [](const auto* item) {
                          return std::abs(item->playheadPosition() - 0.25) < 1e-6;
                      }),
                  "waveform rendering shares the white line's configured anchor");
    waveformItem->setWidth(1000);
    QCoreApplication::processEvents();
    ok &= require(playhead && std::abs(playhead->x() - 250.0) < 1.0,
                  "playhead position remains proportional after resizing");
    waveform->setProperty("engine", QVariant::fromValue(slipEngine.get()));
    QCoreApplication::processEvents();
    const auto slipRenderers = waveform->findChildren<ScrollingWaveformItemStub*>();
    ok &= require(slipRenderers.size() == 2
                      && std::all_of(slipRenderers.cbegin(), slipRenderers.cend(), [](const auto* item) {
                          return std::abs(item->playheadPosition() - 0.25) < 1e-6;
                      }),
                  "lazy-loaded slip preview inherits the configured playhead position");
    QObject* editor = waveform->findChild<QObject*>(
        QStringLiteral("beatgridEditorPanel"));
    ok &= require(editor && std::abs(editor->property("occupiedWidth").toDouble() - 30.0) < 1e-6,
                  "the inline waveform editor preserves its collapsed occupied width");
    if (editor)
        editor->setProperty("expanded", true);
    QCoreApplication::processEvents();
    ok &= require(editor && editor->property("occupiedWidth").toDouble() > 30.0,
                  "the inline waveform editor expands its occupied width at runtime");

    QQuickWindow fxWindow;
    fxWindow.resize(1000, 96);
    QQmlComponent fxBarComponent(&engine, QUrl::fromLocalFile(
        qmlDirectory + QStringLiteral("mixer/FxBar.qml")));
    std::unique_ptr<QObject> fxBar(fxBarComponent.create(&context));
    if (!require(fxBar != nullptr, "FX bar host instantiates")) {
        std::cerr << fxBarComponent.errorString().toStdString() << '\n';
        return false;
    }
    auto* fxBarItem = qobject_cast<QQuickItem*>(fxBar.get());
    if (!require(fxBarItem != nullptr, "FX bar host is visual"))
        return false;
    fxBarItem->setParentItem(fxWindow.contentItem());
    fxBarItem->setWidth(1000);
    fxWindow.show();
    fxWindow.requestActivate();
    QCoreApplication::processEvents();
    ok &= require(fxBarItem->isVisible()
                      && std::abs(fxBarItem->height() - 96.0) < 1e-6,
                  "the real FX bar host is visible at its configured height");
    QObject* unit1 = fxBar->findChild<QObject*>(QStringLiteral("fxUnit1"));
    QObject* unit2 = fxBar->findChild<QObject*>(QStringLiteral("fxUnit2"));
    ok &= require(unit1 && unit2
                      && unit1->metaObject()->indexOfProperty("wetDry") >= 0
                      && unit1->metaObject()->indexOfSignal("deck1Toggled(bool)") >= 0,
                  "FX units preserve their public alias and signals");
    if (unit1 && unit2) {
        unit1->setProperty("wetDry", 0.25);
        unit2->setProperty("wetDry", 0.75);
        ok &= require(std::abs(unit1->property("wetDry").toDouble() - 0.25) < 1e-6
                          && std::abs(unit2->property("wetDry").toDouble() - 0.75) < 1e-6,
                      "the two local FX units retain separate aliased parameter state");
    }
    QObject* effectCombo = unit1
        ? unit1->findChild<QObject*>(QStringLiteral("fxEffectCombo")) : nullptr;
    QObject* effectPopup = effectCombo
        ? effectCombo->property("popup").value<QObject*>() : nullptr;
    ok &= clickItem(fxWindow, effectCombo, "the real FX selector is visible");
    QCoreApplication::processEvents();
    const bool popupReady = effectPopup && QTest::qWaitFor([&] {
                      return effectPopup->property("visible").toBool();
                  });
    if (!popupReady && effectCombo) {
        std::cerr << "FX selector: size=" << effectCombo->property("width").toDouble()
                  << 'x' << effectCombo->property("height").toDouble()
                  << " popup=" << (effectPopup != nullptr);
        if (effectPopup)
            std::cerr << " visible=" << effectPopup->property("visible").toBool()
                      << " focus=" << effectPopup->property("focus").toBool();
        std::cerr << '\n';
    }
    ok &= require(popupReady, "the embedded FX selector opens its controls popup");
    if (popupReady) {
        const int highlighted = effectCombo->property("highlightedIndex").toInt();
        QTest::keyClick(&fxWindow, Qt::Key_Down);
        ok &= require(QTest::qWaitFor([&] {
                          return effectCombo->property("highlightedIndex").toInt()
                              == highlighted + 1;
                      }), "the embedded FX popup receives keyboard navigation");
        QTest::keyClick(&fxWindow, Qt::Key_Escape);
        ok &= require(QTest::qWaitFor([&] {
                          return !effectPopup->property("visible").toBool();
                      }), "Escape closes the embedded FX popup");
    }
    return ok;
}
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    bool ok = sliderCleanupTests();
    const auto main = read("src/qml/main.qml");
    ok &= require(main.find("Math.min(window.scaledWaveformHeight, window.adaptiveWaveformHeight)")
                      != std::string::npos,
                  "the workspace waveform minimum follows its adaptive preferred height");
    const auto topHeader = read("src/qml/shell/TopHeader.qml");
    const auto appOverlays = read("src/qml/shell/AppOverlays.qml");
    ok &= headerPublicationTests(topHeader);
    ok &= hamburgerToggleTests(topHeader);
    const auto deckControl = read("src/qml/deck/DeckControl.qml");
    const auto slider = read("src/qml/components/Slider.qml");
    const auto mixerSection = read("src/qml/mixer/MixerSection.qml");
    const auto workspace = read("src/qml/performance/PerformanceWorkspace.qml");
    const auto developmentControls = read("src/qml/development/DevelopmentControlsWindow.qml");
    const auto performancePads = read("src/qml/performance/PerformancePads.qml");
    ok &= appOverlaysRuntimeTests(appOverlays);
    ok &= shortcutRuntimeTests(main);
    ok &= deckCueFlatButtonRuntimeTests(deckControl);
    ok &= aioDeckControlTests(workspace);
    ok &= crossfaderInlineRuntimeTests(developmentControls);
    ok &= turntableInlineRuntimeTests(performancePads);
    ok &= require(main.find("s(\"showAioDeckControls\", window.showAioDeckControls)") != std::string::npos
                      && main.find("b(\"showAioDeckControls\", true)") != std::string::npos
                      && main.find("onShowAioDeckControlsChanged: _scheduleUiPersist()") != std::string::npos
                      && topHeader.find("propertyName: \"showAioDeckControls\"") != std::string::npos
                      && workspace.find("readonly property bool shown: window.aioDeckControlsHeight > 0") != std::string::npos
                      && main.find("window.allInOneMode && window.showAioDeckControls ? 42 : 0") != std::string::npos,
                  "AIO bar visibility is menu-controlled, persisted and reserved only in AIO");
    const auto shortcuts = componentSection(main, "UiShortcutManager");
    const auto enlargedWaveform = read("src/qml/waveform/EnlargedWaveform.qml");
    const auto turntableIndicator = componentSection(performancePads, "TurntableIndicator");
    const auto crossfader = componentSection(developmentControls, "CrossfaderBar");
    const auto settingsPanel = read("src/qml/settings/SettingsPanel.qml");
    const auto applicationLifecycleHeader = read("src/app/ApplicationLifecycle.h");
    const auto applicationLifecycle = read("src/app/ApplicationLifecycle.cpp");
    ok &= audioRoleTests(settingsPanel);
    ok &= qmlMergeRuntimeTests();
    const auto applicationBootstrap = read("src/app/ApplicationBootstrap.cpp");
    ok &= require(applicationLifecycleHeader.find("bool exitTeardownStarted = false;")
                      != std::string::npos
                      && applicationLifecycleHeader.find("bool shutdownStarted = false;")
                          != std::string::npos
                      && applicationLifecycle.find("if (runtime.exitTeardownStarted)")
                          != std::string::npos
                      && applicationLifecycle.find("if (runtime.shutdownStarted")
                          != std::string::npos
                      && applicationLifecycle.find("std::once_flag") == std::string::npos
                      && applicationLifecycle.find("std::call_once") == std::string::npos,
                  "exit teardown and shutdown are independently idempotent per runtime, including re-entry");
    ok &= require(applicationBootstrap.find("qputenv(\"QT_QUICK_BACKEND\", \"software\")")
                      != std::string::npos
                      && applicationBootstrap.find("qunsetenv(\"QSG_RHI_BACKEND\")")
                          != std::string::npos
                      && applicationBootstrap.find("startupSoftwareFrameRendered")
                          != std::string::npos
                      && applicationBootstrap.find("QSGRendererInterface::Software")
                          != std::string::npos
                      && applicationBootstrap.find("window->setProperty(\"allowDirectClose\", true)")
                          != std::string::npos
                      && applicationBootstrap.find("if (!window->close())") != std::string::npos
                      && applicationBootstrap.find("if (!startupCloseSmoke)") != std::string::npos
                      && applicationBootstrap.find("!startupCloseSmoke, nullptr")
                          != std::string::npos
                      && applicationBootstrap.find("startupEarlyCloseSmoke")
                          != std::string::npos
                      && read("src/main.cpp").find("--ci-startup-early-close-test")
                          != std::string::npos
                      && applicationBootstrap.find("runtime.stopping = true;\n                rootWindow->setProperty(\"allowDirectClose\", true)")
                          != std::string::npos,
                  "headless smoke verifies Qt Quick software rendering, normal and early QML-policy closes, and skips hardware discovery");
    const auto library = read("src/qml/library/Library.qml");
    ok &= libraryActionTests(library);
    ok &= require(library.find("tr.touchMode && tr.rowSourceTab !== \"usb\" ? 148 : 0")
                      != std::string::npos
                      && library.find("tr.miscActionW > 0 && tr.swipeX") != std::string::npos,
                  "USB rows retain deck swipes but do not expose local mutation actions");
    const auto sourcePage = read("src/qml/library/SourcePage.qml");
    const auto waveformScreen = read("src/qml/performance/PerformanceWaveformScreen.qml");
    const auto beatgridPanel = componentSection(
        waveformScreen, "PerformanceBeatgridPanel");
    const auto performanceActionButton = componentSection(
        waveformScreen, "PerformanceActionButton");
    const auto beatFxPanel = componentSection(
        waveformScreen, "PerformanceBeatFxPanel");
    const auto deckQuickPanel = componentSection(
        waveformScreen, "PerformanceDeckQuickPanel");
    const auto beatgridEditor = componentSection(
        enlargedWaveform, "BeatgridEditorPanel");
    const auto fxBar = read("src/qml/mixer/FxBar.qml");
    const auto fxUnit = componentSection(fxBar, "FxUnit");
    const auto fxDarkButton = componentSection(fxBar, "FxDarkBtn");
    const auto fxAssignButton = componentSection(fxBar, "FxAssignBtn");
    const auto productionCmake = read("src/CMakeLists.txt");
    const auto qmlManifestStart = productionCmake.find("set(BROCKDJ_QML_FILES");
    const auto qmlManifestEnd = productionCmake.find("PARENT_SCOPE", qmlManifestStart);
    const auto qmlManifest = qmlManifestStart != std::string::npos
            && qmlManifestEnd != std::string::npos
        ? productionCmake.substr(qmlManifestStart, qmlManifestEnd - qmlManifestStart)
        : std::string{};
    const auto deckTrackInfoPanel = read("src/qml/deck/DeckTrackInfoPanel.qml");
    const auto flx10Mapping = read("src/controllers/mappings/DDJ-FLX10.brockdj.xml");
    const auto flx10MidiBridge = read("src/controllers/flx10/Flx10MidiBridge.cpp");
    const auto engineHeader = read("src/deck/DjEngine.h");
    const auto engineTransport = read("src/deck/DjEngineTransport.cpp");
    const auto midiManagerHeader = read("src/controllers/midi/MidiControllerManager.h");
    ok &= require(deckControl.find("component DeckSlider: Slider {") != std::string::npos
                      && deckControl.find("dsDragLock") == std::string::npos,
                  "deck tempo reuses shared slider interaction with styling overrides");
    const auto cueButtonBegin = deckControl.find("btnText: \"CUE\"");
    const auto cueButtonEnd = deckControl.find("\n                        }", cueButtonBegin);
    ok &= require(deckControl.find("property var heldCueEngine: null") != std::string::npos
                      && deckControl.find("var target = heldCueEngine") != std::string::npos
                      && deckControl.find("target.cueButtonRelease()") != std::string::npos
                      && deckControl.find("onVisibleChanged: if (!visible) releaseCue()")
                          != std::string::npos
                      && deckControl.find("onEngineChanged: releaseCue()") != std::string::npos
                      && deckControl.find("Component.onDestruction:") != std::string::npos
                      && cueButtonBegin != std::string::npos && cueButtonEnd != std::string::npos
                      && deckControl.find("enabled: deck.engine && deck.engine.hasTrack",
                                          cueButtonBegin) < cueButtonEnd
                      && deckControl.find("onBtnCanceled: deck.releaseCue()", cueButtonBegin)
                          < cueButtonEnd
                      && deckControl.find("onEnabledChanged: if (!enabled) deck.releaseCue()",
                                          cueButtonBegin) < cueButtonEnd,
                  "DeckControl CUE releases its captured engine on cancel, disable, hide, rebind, and teardown");
    ok &= require(slider.find("acceptedDevices: PointerDevice.TouchScreen") != std::string::npos
                      && slider.find("_touchDrag = sliderTouch.active") != std::string::npos
                      && slider.find("if (!_touchDrag)") != std::string::npos
                      && slider.find("if (!pressed)") != std::string::npos,
                  "slider excludes hover and avoids hiding the mouse cursor for touch");
    ok &= require(!beatgridPanel.empty() && !beatFxPanel.empty()
                      && !deckQuickPanel.empty() && !beatgridEditor.empty()
                      && !fxUnit.empty()
                      && occurrences(beatgridPanel, "component ") == 1
                      && occurrences(fxUnit, "component ") == 1
                      && performanceActionButton.find("required property real rowHeight")
                          != std::string::npos
                      && performanceActionButton.find("required property color accent")
                          != std::string::npos
                      && fxDarkButton.find("required property color accent")
                          != std::string::npos
                      && fxAssignButton.find("required property color accent")
                          != std::string::npos
                      && occurrences(beatgridPanel, "PerformanceActionButton {") == 8
                      && occurrences(beatgridPanel, "rowHeight: root.rowHeight") == 8
                      && occurrences(beatgridPanel, "accent: root.accentColor") == 8
                      && occurrences(fxUnit, "accent: root.accentColor") == 2,
                  "merged QML components remain local, independently scoped, and non-nested");
    ok &= require(!qmlManifest.empty()
                      && occurrences(qmlManifest, "src/qml/") == 22
                      && qmlManifest.find("src/qml/performance/PerformanceBackdrop.qml") != std::string::npos
                      && qmlManifest.find("src/qml/shell/AppOverlays.qml") != std::string::npos
                      && qmlManifest.find("PerformanceBeatgridPanel.qml") == std::string::npos
                      && qmlManifest.find("PerformanceDeckQuickPanel.qml") == std::string::npos
                      && qmlManifest.find("PerformanceBeatFxPanel.qml") == std::string::npos
                      && qmlManifest.find("BeatgridEditorPanel.qml") == std::string::npos
                      && qmlManifest.find("FxUnit.qml") == std::string::npos
                      && qmlManifest.find("StartupOverlay.qml") == std::string::npos
                      && qmlManifest.find("StatusOverlay.qml") == std::string::npos
                      && qmlManifest.find("ExitOverlay.qml") == std::string::npos
                      && qmlManifest.find("UiShortcutManager.qml") == std::string::npos
                      && qmlManifest.find("TurntableIndicator.qml") == std::string::npos
                      && qmlManifest.find("CrossfaderBar.qml") == std::string::npos,
                  "central manifest packages 22 QML resources including the shared backdrop and excludes merged components");
    ok &= require(deckQuickPanel.find("property real beatJumpBeats") == std::string::npos
                      && deckQuickPanel.find("root.engine.beatJumpBeats") != std::string::npos
                      && engineHeader.find("Q_PROPERTY(double beatJumpBeats") != std::string::npos,
                  "Beat Jump range is per-deck engine state shared by UI and controller");
    ok &= require(flx10Mapping.find("paramId=\"deckA_beatjump_4_backward\" status=\"0x90\" control=\"0x5E\" type=\"momentary\"") != std::string::npos
                      && flx10Mapping.find("paramId=\"deckB_beatjump_range_up\" status=\"0x91\" control=\"0x62\" type=\"momentary\"") != std::string::npos
                      && flx10Mapping.find("paramId=\"deckA_jog_fast_search\" status=\"0xB0\" control=\"0x29\" type=\"encoder-relative\"") != std::string::npos
                      && flx10Mapping.find("paramId=\"deckB_beatjump_search_forward\" status=\"0x91\" control=\"0x71\" type=\"momentary\"") != std::string::npos,
                  "FLX10 Beat Jump buttons retain release events for click-versus-hold routing");
    ok &= require(std::count(main.begin(), main.end(), '\n') < 660,
                  "main.qml remains a bounded application shell");
    ok &= require(main.find("PerformanceWorkspace") != std::string::npos
                      && main.find("AppOverlays {") != std::string::npos
                      && main.find("StartupOverlay {") == std::string::npos
                      && main.find("StatusOverlay {") == std::string::npos
                      && main.find("ExitOverlay {") == std::string::npos,
                  "shell routes its overlay host and performance workspace");
    ok &= require(componentSection(appOverlays, "StartupOverlay").find("z: 1000")
                      != std::string::npos
                      && appOverlays.find("elapsedMs() >= 9000") == std::string::npos
                      && appOverlays.find("if (!window.startupReady)") != std::string::npos
                      && appOverlays.find("window.startupAudioError.length > 0")
                          != std::string::npos
                      && componentSection(appOverlays, "StatusOverlay").find("z: 1001")
                          != std::string::npos
                      && componentSection(appOverlays, "ExitOverlay").find("z: 1000")
                          != std::string::npos
                      && appOverlays.find("uncleanShutdownWarning: statusOverlay.uncleanShutdownWarning")
                          != std::string::npos,
                  "the unified overlay host preserves independent layers and warning ownership");
    ok &= require(!shortcuts.empty()
                      && shortcuts.find("Ctrl+=") != std::string::npos
                      && shortcuts.find("Ctrl++") != std::string::npos
                      && shortcuts.find("Ctrl+-") != std::string::npos
                      && shortcuts.find("Ctrl+0") != std::string::npos
                      && shortcuts.find("Ctrl+Shift+=") != std::string::npos
                      && shortcuts.find("Ctrl+Shift++") != std::string::npos
                      && shortcuts.find("Ctrl+Shift+-") != std::string::npos
                      && shortcuts.find("Ctrl+Shift+0") != std::string::npos
                      && shortcuts.find("appWindow._isTextInputFocused()") != std::string::npos
                      && occurrences(main, "UiShortcutManager {") == 1
                      && occurrences(main, "context: Qt.ApplicationShortcut") >= 10,
                  "the main shell owns exactly one text-input-aware global shortcut group");
    ok &= require(!turntableIndicator.empty()
                      && performancePads.find("TurntableIndicator {") != std::string::npos
                      && !crossfader.empty()
                      && developmentControls.find("CrossfaderBar {") != std::string::npos
                      && developmentControls.find("hostWindow: root.appWindow") != std::string::npos
                      && crossfader.find("cfMult(") == std::string::npos
                      && crossfader.find("assignForChannelId(") == std::string::npos
                      && crossfader.find("multiplierForChannelId(") == std::string::npos
                      && crossfader.find("function channelGain(") != std::string::npos
                      && crossfader.find("function curveExponent(") != std::string::npos
                      && turntableIndicator.find(
                             "root.radiusForPoint(mouse.x, mouse.y) < root.minRadiusPx")
                          != std::string::npos
                      && turntableIndicator.find("root.engine.scratchBySeconds(deltaSec)")
                          != std::string::npos
                      && turntableIndicator.find("onCanceled: {") != std::string::npos,
                  "exclusive turntable and crossfader controls are local to their owning QML hosts");
    ok &= require(workspace.find("DeckControl") != std::string::npos, "workspace uses shared deck component");
    ok &= require(workspace.find("MixerSection") == std::string::npos
                      && workspace.find("CrossfaderBar") == std::string::npos
                      && workspace.find("FxBar") == std::string::npos
                      && developmentControls.find("MixerSection") != std::string::npos
                      && developmentControls.find("FxBar {") != std::string::npos
                      && developmentControls.find(
                             "visible: appWindow && appWindow.showDevelopmentControls")
                          != std::string::npos,
                  "production workspace omits hidden mixer and FX trees while development retains them");
    ok &= require(developmentControls.find("minimumWidth: 1280") != std::string::npos
                      && developmentControls.find("maximumWidth: 1280") != std::string::npos
                      && developmentControls.find("minimumHeight: 430") != std::string::npos
                      && developmentControls.find("maximumHeight: 430") != std::string::npos,
                  "development controls use a fixed window size to avoid cross-window live-resize stalls");
    ok &= require(workspace.find("id: twoDeckWaveformLoader") != std::string::npos
                      && workspace.find("id: fourDeckWaveformLoader") != std::string::npos
                      && workspace.find("active: window.fourDeckMode") != std::string::npos,
                  "mutually exclusive waveform and C/D deck trees load only in their active mode");
    ok &= require(workspace.find("id: settingsSectionLoader") != std::string::npos
                      && workspace.find("id: sourcePageLoader") != std::string::npos
                      && occurrences(workspace, "asynchronous: true") >= 2,
                  "infrequent AIO settings and source surfaces load asynchronously on demand");
    ok &= require(topHeader.find("settingsWindowFactory.createObject(null)") != std::string::npos
                      && topHeader.find("SettingsPanel {") != std::string::npos
                      && topHeader.find("SettingsWindow { id: settingsWin }") == std::string::npos,
                  "desktop settings no longer retain a hidden startup window");
    ok &= require(deckControl.find("onAir: deck.engine ? deck.engine.onAir : false")
                      != std::string::npos
                  && deckControl.find("onAir: deck.engine && deck.engine.isPlaying")
                      == std::string::npos,
                  "ON AIR UI consumes the audio routing snapshot, not transport or level");
    ok &= require(topHeader.find("deckA.masterVuLevelL") != std::string::npos
                  && topHeader.find("deckA.masterVuLevelR") != std::string::npos
                  && topHeader.find("Math.max(deckA ? deckA.vuLevelL") == std::string::npos,
                  "master VU consumes the final master output snapshot");
    ok &= require(mixerSection.find("engineA.preFaderVuLevelL") != std::string::npos
                  && mixerSection.find("normalizedDb(levelLinear)") != std::string::npos,
                  "channel UI meter consumes pre-fader peaks with dBFS height mapping");
    ok &= require(settingsPanel.find("mappingEditorFactory.createObject(null)") != std::string::npos
                      && settingsPanel.find("id: mappingEditorWindow") == std::string::npos,
                  "mapping editors are constructed only when opened");
    ok &= require(main.find("waveformZoomLevels") == std::string::npos, "legacy fixed zoom list removed");
    ok &= require(settingsPanel.find("settingsManager.setAudioConfiguration") != std::string::npos,
                  "audio settings use one atomic persistence operation");
    ok &= require(settingsPanel.find("firstRealOutput(outputOptions)") != std::string::npos,
                  "device-list reconciliation never replaces a preference with None");
    ok &= require(settingsPanel.find("pairText === \"None\"") != std::string::npos
                      && settingsPanel.find("deckB.setOutputFirstChannel(masterFirstChannel)")
                          == std::string::npos,
                  "Master routing is normalized explicitly instead of by a hidden deck side effect");
    ok &= require(settingsPanel.find("audioUiSyncing = true\n        audioOutputDeviceOptions =")
                          != std::string::npos,
                  "ComboBox model changes are guarded before they can select None");
    ok &= require(settingsPanel.find("interval: settingsWindow.audioOutputsAvailable ? 5000 : 1500")
                          != std::string::npos
                      && settingsPanel.find("audioDeviceListRetryCount") == std::string::npos
                      && settingsPanel.find("updateAudioDeviceListRetry()") != std::string::npos
                      && settingsPanel.find("Component.onCompleted: updateAudioSettingsActivity()")
                          != std::string::npos
                      && settingsPanel.find("text: \"Reload devices\"") != std::string::npos
                      && settingsPanel.find("settingsManager.refreshAudioDeviceLists()")
                          != std::string::npos
                      && settingsPanel.find("deckA.getAvailableAudioOutputDevices(backend)")
                          != std::string::npos
                      && settingsPanel.find("Layout.minimumWidth: 180") != std::string::npos
                      && settingsPanel.find("Layout.minimumWidth: 130") != std::string::npos
                      && settingsPanel.find("placeholderText: \"No device selected\"")
                          != std::string::npos
                      && settingsPanel.find("placeholderText: \"No channels\"")
                          != std::string::npos,
                  "audio output discovery is per backend and audio role selectors remain readable when empty");
    const auto audioCallbackRegistration = applicationBootstrap.find(
        "runtime.audioEngine->registerCallback(runtime.audioDeviceService->manager())");
    const auto initialAudioApply = applicationBootstrap.find("const bool audioSettingsApplied");
    ok &= require(audioCallbackRegistration != std::string::npos
                      && initialAudioApply != std::string::npos
                      && audioCallbackRegistration < initialAudioApply,
                  "audio callback is registered before the startup device is opened");
    ok &= require(main.find("resizeThrottleCounter") == std::string::npos, "resize event counter removed");
    ok &= require(shortcuts.find("Ctrl+Shift+0") != std::string::npos
                  && shortcuts.find("Ctrl+0") != std::string::npos,
                  "independent reset shortcuts exist");
    ok &= require(engineHeader.find("Q_PROPERTY(bool scratchVisualActive READ isScratchVisualActive NOTIFY scrubbingChanged)")
                      != std::string::npos,
                  "scratch visual activity is a reactive QML property");
    ok &= require(enlargedWaveform.find("root.engine.scratchVisualActive") != std::string::npos,
                  "waveform frame animations react to paused scratch state");
    ok &= require(enlargedWaveform.find("color: UiTheme.playhead") != std::string::npos
                     && occurrences(enlargedWaveform, "playheadPosition: root.playheadPosition") == 2
                     && settingsPanel.find("settingsManager.waveformPlayheadPosition = positions[index]")
                         != std::string::npos,
                  "audible and slip waveforms share the configurable white playhead");
    ok &= require(engineHeader.find("Q_PROPERTY(bool slipPreviewActive") != std::string::npos
                     && enlargedWaveform.find("id: slipWaveLoader") != std::string::npos
                     && enlargedWaveform.find("slipPreview: true") != std::string::npos
                     && enlargedWaveform.find("contentReady: slipWaveItem.contentReady") != std::string::npos
                     && enlargedWaveform.find("slipWaveLoader.item.contentReady") != std::string::npos,
                  "slip mode reveals its split only after the background waveform can render");
    ok &= require(engineHeader.find("Q_PROPERTY(bool seekPreviewActive") != std::string::npos
                     && deckTrackInfoPanel.find("beginSeekPreview") != std::string::npos
                     && deckTrackInfoPanel.find("commitSeekPreview") != std::string::npos
                     && deckTrackInfoPanel.find("onReleased: root.engine.commitSeekPreview()")
                         != std::string::npos,
                  "overview drag previews a quantized target and commits only on release");
    ok &= require(engineHeader.find("m_quantizedOverviewSeekPending") != std::string::npos
                     && engineTransport.find("scheduleQuantizedCueJump(target)")
                         != std::string::npos,
                  "quantized overview seek waits for the next source beat");
    ok &= require(deckControl.find("label: \"JUMP\"") != std::string::npos
                     && deckControl.find("beatJump(-deck.engine.beatJumpBeats)")
                         != std::string::npos
                     && deckControl.find("beatJump(deck.engine.beatJumpBeats)")
                         != std::string::npos,
                  "development deck controls expose backward and forward Beat Jump");
    ok &= require(mixerSection.find("minimumUsableHeight") != std::string::npos
                     && developmentControls.find("mixerAB.minimumUsableHeight")
                         != std::string::npos,
                  "development mixer reserves enough height above the crossfader");
    ok &= require(enlargedWaveform.find("FrameAnimation {") != std::string::npos
                      && turntableIndicator.find("FrameAnimation {") != std::string::npos
                      && enlargedWaveform.find("waveformMotionIntervalMs <= 17")
                          != std::string::npos
                      && turntableIndicator.find("motionIntervalMs <= 17")
                          != std::string::npos,
                  "moving deck visuals use the presentation clock at full quality");
    ok &= require(enlargedWaveform.find("waveformRasterWorkEnabled")
                          != std::string::npos
                      && enlargedWaveform.find("waveformMotionIntervalMs > 17")
                          != std::string::npos
                      && turntableIndicator.find("motionIntervalMs > 17")
                          != std::string::npos,
                  "audio pressure can reduce animation and suspend tile raster work");
    ok &= require(performancePads.find("[\"HOT CUE\", \"PAD FX\", \"BEATJUMP\", \"SAMPLER\"]")
                      != std::string::npos
                  && performancePads.find("selectPerformancePadMode(root.deckId, index)")
                      != std::string::npos,
                  "performance pad tabs and FLX10 mode state stay synchronized");
    ok &= require(performancePads.find("PointerDevice.TouchScreen") != std::string::npos
                  && performancePads.find("root.beginPadPress(index)") != std::string::npos
                  && performancePads.find("root.endPadPress(index)") != std::string::npos,
                  "performance pad pages and hold actions accept native touch input");
    ok &= require(midiManagerHeader.find("setPerformancePadPressed") != std::string::npos
                  && midiManagerHeader.find("consumePerformancePadPlayLatch") != std::string::npos
                  && midiManagerHeader.find("performancePadStateChanged") != std::string::npos,
                  "touch and FLX10 pads share one controller state path");
    ok &= require(performancePads.find("nextMode === 4 ? 3 : nextMode") != std::string::npos
                  && performancePads.find("root.keyShiftMode && index === 3 ? \"KEY SHIFT\"")
                      != std::string::npos
                  && performancePads.find("root.engine.keySemitoneOffset - root.keyShiftValue(index)")
                      != std::string::npos
                  && flx10MidiBridge.find("engine->keySemitoneOffset()")
                      != std::string::npos
                  && flx10MidiBridge.find("&DjEngine::keySemitoneOffsetChanged")
                      != std::string::npos
                  && flx10MidiBridge.find("mode == MidiPadMode::KeyShift") != std::string::npos,
                  "touch and hardware pads highlight the currently selected Key Shift value");
    ok &= require(main.find("onLibraryViewToggleRequested") != std::string::npos,
                  "FLX10 View action toggles the visible library surface");
    ok &= require(library.find("if (!libraryRoot.visible)") != std::string::npos
                  && library.find("waveformZoomController.zoomIn()") != std::string::npos
                  && library.find("waveformZoomController.zoomOut()") != std::string::npos,
                  "FLX10 browse encoder controls waveform zoom while the library is hidden");
    ok &= require(library.find("tileLabel: \"SOURCE\"") == std::string::npos
                      && workspace.find("librarySection.activeTab = \"library\"")
                          != std::string::npos
                      && sourcePage.find("function syncCursorToActiveSource()")
                          != std::string::npos
                      && sourcePage.find("color: active ? \"#40d84b\" : \"#d6d8df\"")
                          != std::string::npos,
                  "standalone Source reflects the active Local or USB library");
    ok &= require(library.find("readonly property bool usbTrackViewVisible")
                          != std::string::npos
                      && occurrences(library, "visible: libraryRoot.usbTrackViewVisible") == 3,
                  "USB track chrome cannot reserve space above the playlist browser");
    ok &= require(library.find("function enterUsbPlaylistFolder(folder)")
                          != std::string::npos
                      && library.find("function updateUsbPlaylistPreview()")
                          != std::string::npos
                      && library.find("id: usbPlaylistFolderPreview")
                          != std::string::npos
                      && library.find("id: usbPlaylistPreviewTracks")
                          != std::string::npos
                      && library.find("fullTrackView: libraryRoot.usbTrackViewVisible")
                          != std::string::npos,
                  "USB playlists drill through the left pane, preview tracks on the right, and show loads only in full track views");
    ok &= require(library.find("deviceLibraryManager.mountDevice(modelData.id)")
                          != std::string::npos
                      && library.find("deviceLibraryManager.ejectDevice(modelData.id)")
                          != std::string::npos
                      && library.find("modelData.operationPending ? \"…\"")
                          != std::string::npos
                      && library.find("deviceLibraryManager.selectedDeviceReady")
                          != std::string::npos,
                  "USB device rows expose guarded mount/eject actions and mounted-only navigation");
    ok &= require(occurrences(library, "component SortHeader: Rectangle") == 1
                      && library.find("component PlSortHeader:") == std::string::npos,
                  "library and playlist tables share one sortable header component");
    ok &= require(library.find("readonly property string activeSortField")
                          != std::string::npos
                      && library.find("readonly property bool activeSortAscending")
                          != std::string::npos
                      && library.find("libraryRoot.togglePlaylistSort(sh.field)")
                          != std::string::npos
                      && library.find("libraryModel.toggleSort(sh.field)")
                          != std::string::npos,
                  "shared sort headers preserve both library and playlist dispatch paths");
    ok &= require(occurrences(library, "playlistMode: true") == 6,
                  "all six playlist columns opt into playlist sorting");
    ok &= require(library.find("component AioQuickBtn:") == std::string::npos,
                  "unused AIO quick-button component stays removed");
    ok &= require(main.find("function closeTopBarPullDown()") != std::string::npos
                  && main.find("function openTopBarPullDown()") != std::string::npos
                  && main.find("function toggleTopBarPullDown()") != std::string::npos,
                  "main.qml owns the quick-access tray lifecycle");
    ok &= require(main.find("if (window.topBarPullProgress > 0.0)") != std::string::npos
                  && main.find("window.closeTopBarPullDown()") != std::string::npos,
                  "Escape closes an open quick-access tray before other navigation");
    ok &= require(topHeader.find("id: quickPerformanceMouse") != std::string::npos
                  && topHeader.find("id: quickLibraryMouse") != std::string::npos
                  && topHeader.find("id: quickSettingsMouse") != std::string::npos
                  && topHeader.find("id: quickModeMouse") != std::string::npos
                  && topHeader.find("id: quickFullscreenMouse") != std::string::npos
                  && occurrences(topHeader, "root.Window.window.closeTopBarPullDown()") >= 6,
                  "quick-access navigation and fullscreen actions close the tray");
    ok &= require(flx10Mapping.find("paramId=\"deckA_slip_reverse\" status=\"0x90\" control=\"0x15\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"library_view_toggle\" status=\"0x96\" control=\"0x7A\"")
                      != std::string::npos,
                  "FLX10 Slip Reverse and View controls use the documented notes");
    // Note 0x46, confirmed against the hardware: the switch sends 127 when it
    // latches on and 0 when it releases, so both edges must be dispatched.
    ok &= require(flx10Mapping.find("paramId=\"beat_fx_on\" status=\"0x94\" control=\"0x46\" type=\"momentary\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_beat_minus\" status=\"0x94\" control=\"0x4A\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_beat_plus\" status=\"0x94\" control=\"0x4B\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"sound_color_fx_filter\" status=\"0x96\" control=\"0x05\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_sound_color\" status=\"0xB6\" control=\"0x17\"")
                      != std::string::npos,
                  "FLX10 Beat FX state and Sound Color controls are mapped");
    ok &= require(flx10Mapping.find("paramId=\"deckA_quantize\" status=\"0x90\" control=\"0x35\"")
                      != std::string::npos
                  && flx10Mapping.find("<DeckLed name=\"quantize\" control=\"0x35\"/>")
                      != std::string::npos,
                  "FLX10 Quantize input and LED feedback share the documented note");
    ok &= require(flx10Mapping.find("paramId=\"deckA_pad_mode_sampler\" status=\"0x90\" control=\"0x22\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_pad_mode_keyshift\" status=\"0x90\" control=\"0x6F\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_pad_mode_keyshift\" status=\"0x91\" control=\"0x6F\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckC_pad_mode_keyshift\" status=\"0x92\" control=\"0x6F\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckD_pad_mode_keyshift\" status=\"0x93\" control=\"0x6F\"")
                      != std::string::npos
                  && flx10MidiBridge.find("sendMappedNoteLed(prefix + QStringLiteral(\"keyshift\")")
                      != std::string::npos
                  && flx10MidiBridge.find("mode == MidiPadMode::Sampler || mode == MidiPadMode::KeyShift")
                      == std::string::npos
                  && flx10MidiBridge.find("mode == MidiPadMode::Sampler && shiftHeld")
                      == std::string::npos,
                  "FLX10 Sampler and shifted Key Shift mode are independent MIDI commands");
    ok &= require(flx10MidiBridge.find("if (padModeForDeck(deck) == MidiPadMode::KeyShift) {")
                      != std::string::npos
                  && flx10MidiBridge.find("handleKeyShiftPad(deck, deckEngine, padIndex,")
                      != std::string::npos,
                  "FLX10 stale Hot Cue pad packets cannot cancel an explicit Key Shift mode");
    const std::string midiParameterDispatch = read("src/controllers/midi/MidiParameterDispatch.cpp");
    ok &= require(midiParameterDispatch.find("decodeFlx10KeyShiftPadWireEvent")
                      != std::string::npos
                  && midiParameterDispatch.find("channel < 7 || channel > 14")
                      != std::string::npos
                  && midiParameterDispatch.find("note >= 0x70 && note <= 0x7f")
                      != std::string::npos
                  && midiParameterDispatch.find("handleKeyShiftPad(keyShiftPad.deck")
                      != std::string::npos,
                  "physical FLX10 Key Shift pads bypass the generic parameter-store route");
    ok &= require(flx10Mapping.find("paramId=\"deckA_keyshift_range_down\" status=\"0x90\" control=\"0x2B\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_keyshift_range_up\" status=\"0x90\" control=\"0x33\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keyshift_range_down\" status=\"0x91\" control=\"0x2B\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keyshift_range_up\" status=\"0x91\" control=\"0x33\"")
                      != std::string::npos,
                  "FLX10 Key Shift PAGE controls use their mode-specific deck notes");
    ok &= require(flx10Mapping.find("paramId=\"deckA_keyshift_pad1\" status=\"0x97\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_keyshift_pad1_shift\" status=\"0x98\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keyshift_pad1\" status=\"0x99\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keyshift_pad1_shift\" status=\"0x9A\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckC_keyshift_pad1\" status=\"0x9B\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckC_keyshift_pad1_shift\" status=\"0x9C\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckD_keyshift_pad1\" status=\"0x9D\" control=\"0x70\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckD_keyshift_pad1_shift\" status=\"0x9E\" control=\"0x70\"")
                      != std::string::npos
                  && flx10MidiBridge.find("handleKeyShiftPad(deck, deckEngine, padIndex, value >= 0.5f,\n                                  shiftedKeyPad)")
                      != std::string::npos,
                  "FLX10 Key Shift pads preserve the hardware's normal/shift channel distinction");
    ok &= require(flx10Mapping.find("paramId=\"deckA_keylock\" status=\"0x90\" control=\"0x4A\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_keylock\" status=\"0x91\" control=\"0x4A\"")
                      != std::string::npos,
                  "FLX10 Key Lock uses MIX POINT LINK rather than the Key Shift mode command");
    ok &= require(flx10Mapping.find("paramId=\"deckA_tempo\" status=\"0xB0\" control=\"0x00\" type=\"fader\"/")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_tempo\" status=\"0xB0\" control=\"0x20\" type=\"fader\"/")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_tempo\" status=\"0xB1\" control=\"0x00\" type=\"fader\"/")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_tempo\" status=\"0xB1\" control=\"0x20\" type=\"fader\"/")
                      != std::string::npos,
                  "FLX10 tempo faders map both non-inverted halves of the Pioneer 14-bit data");
    ok &= require(flx10Mapping.find("paramId=\"deckA_tempo_range_cycle\" status=\"0x90\" control=\"0x60\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_tempo_range_cycle\" status=\"0x91\" control=\"0x60\"")
                      != std::string::npos,
                  "FLX10 shifted Tempo Reset cycles the hardware tempo range");
    const auto has14BitPair = [&flx10Mapping](const char* paramId,
                                              const char* status,
                                              const char* msb,
                                              const char* lsb) {
        const std::string prefix = std::string("paramId=\"") + paramId
            + "\" status=\"" + status + "\" control=\"";
        return flx10Mapping.find(prefix + msb + "\"") != std::string::npos
            && flx10Mapping.find(prefix + lsb + "\"") != std::string::npos;
    };
    ok &= require(has14BitPair("beat_fx_level_depth", "0xB4", "0x02", "0x22"),
                  "FLX10 Beat FX LEVEL/DEPTH maps its complete documented 14-bit pair");
    ok &= require(has14BitPair("deckA_gain", "0xB0", "0x04", "0x24")
                  && has14BitPair("deckB_gain", "0xB1", "0x04", "0x24")
                  && has14BitPair("deckA_eqHigh", "0xB0", "0x07", "0x27")
                  && has14BitPair("deckB_eqHigh", "0xB1", "0x07", "0x27")
                  && has14BitPair("deckA_eqMid", "0xB0", "0x0B", "0x2B")
                  && has14BitPair("deckB_eqMid", "0xB1", "0x0B", "0x2B")
                  && has14BitPair("deckA_eqLow", "0xB0", "0x0F", "0x2F")
                  && has14BitPair("deckB_eqLow", "0xB1", "0x0F", "0x2F")
                  && has14BitPair("deckA_vol", "0xB0", "0x13", "0x33")
                  && has14BitPair("deckB_vol", "0xB1", "0x13", "0x33")
                  && has14BitPair("crossfader", "0xB6", "0x1F", "0x3F")
                  && has14BitPair("headphone_mix", "0xB6", "0x0C", "0x2C")
                  && has14BitPair("headphone_level", "0xB6", "0x0D", "0x2D")
                  && has14BitPair("deckA_sound_color", "0xB6", "0x17", "0x37")
                  && has14BitPair("deckB_sound_color", "0xB6", "0x18", "0x38"),
                  "FLX10 mixer faders and knobs map complete coherent 14-bit pairs");
    ok &= require(flx10Mapping.find("paramId=\"master_level\"") == std::string::npos,
                  "FLX10 hardware Master Level never changes the software master gain");
    ok &= require(flx10Mapping.find("paramId=\"master_cue\" status=\"0x96\" control=\"0x63\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckA_headphone_cue\" status=\"0x90\" control=\"0x54\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckB_headphone_cue\" status=\"0x91\" control=\"0x54\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckC_headphone_cue\" status=\"0x92\" control=\"0x54\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"deckD_headphone_cue\" status=\"0x93\" control=\"0x54\"")
                      != std::string::npos
                  && flx10Mapping.find("<DeckLed name=\"headphone_cue\" control=\"0x54\"/>")
                      != std::string::npos,
                  "FLX10 master and all four channel headphone CUE controls use the documented notes");
    ok &= require(flx10Mapping.find("paramId=\"beat_fx_channel_deck_a\" status=\"0x94\" control=\"0x10\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_channel_deck_b\" status=\"0x94\" control=\"0x11\"")
                      != std::string::npos
                  && flx10Mapping.find("paramId=\"beat_fx_channel_master\" status=\"0x94\" control=\"0x14\"")
                      != std::string::npos,
                  "FLX10 Beat FX channel selector follows CH1, CH2 and Master notes");
    ok &= require(waveformScreen.find("property string leftPanel: \"deck\"")
                      != std::string::npos
                  && waveformScreen.find("Math.min(230, Math.max(176, width * 0.155))")
                      != std::string::npos
                  && waveformScreen.find("readonly property real handleWidth: 22")
                      != std::string::npos,
                  "performance song information is open by default with compact side handles");
    ok &= require(waveformScreen.find("clip: true") != std::string::npos
                  && waveformScreen.find(
                         "x: -width * (1.0 - root.leftPanelReveal)")
                      != std::string::npos
                  && waveformScreen.find(
                         "x: root.width - width * root.rightPanelReveal")
                      != std::string::npos
                  && waveformScreen.find("Behavior on x") == std::string::npos,
                  "collapsed performance panels do not lag into view while resizing");
    ok &= require(deckQuickPanel.find("function loadedSourceLabel()") != std::string::npos
                  && deckQuickPanel.find("engine.externalSourceId") != std::string::npos
                  && deckQuickPanel.find("deviceLibraryManager.devices") != std::string::npos
                  && deckQuickPanel.find("text: \"SOURCE\"") != std::string::npos,
                  "compact deck side panel shows the loaded Local or USB source");
    // The neighbouring beat lengths are shown either side of the current one and
    // are selectable directly, the way a player prints them.
    ok &= require(beatFxPanel.find("function setDivisionIndex(index)") != std::string::npos
                  && beatFxPanel.find("fx.setBeatDivision(1, divisions[index].value)")
                      != std::string::npos
                  && beatFxPanel.find("root.setDivisionIndex(divIndex)") != std::string::npos,
                  "compact Beat FX panel exposes selectable beat lengths");
    ok &= require(deckQuickPanel.find("root.engine.ejectTrack()") != std::string::npos
                  && deckQuickPanel.find("!engine.isPlaying") != std::string::npos,
                  "deck side panel can eject a stopped deck");
    return ok ? 0 : 1;
}

#include "qml_component_tests.moc"
