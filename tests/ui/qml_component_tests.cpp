#include <QGuiApplication>
#include <QDir>
#include <QMouseEvent>
#include <QColor>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <QSignalSpy>
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
    Q_PROPERTY(bool rectified READ rectified WRITE setRectified)
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
    bool rectified() const { return m_rectified; }
    void setRectified(bool value) { m_rectified = value; }
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
    bool m_rectified = false;
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

bool viewportHeightScalingTests(const std::string& main)
{
    QString methods;
    if (!extractFunctions(main, {"performanceScaleForHeight"}, methods)) return false;
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData((QStringLiteral(R"(
        import QtQml
        QtObject {
            property real topBarHeight: 48
            property real baseWaveformHeight: 340
            property real baseDeckMixerHeight: 304
        )") + methods + QStringLiteral("\n}")).toUtf8(), QUrl());
    std::unique_ptr<QObject> host(component.create());
    if (!require(host != nullptr, "production viewport sizing helper instantiates")) return false;
    bool ok = true;
    for (const int height : {600, 720, 800, 900, 1080, 1440}) {
        for (const int rows : {1, 2}) {
            for (const double userScale : {0.8, 1.0, 1.4}) {
                QVariant scale;
                ok &= QMetaObject::invokeMethod(host.get(), "performanceScaleForHeight",
                    Q_RETURN_ARG(QVariant, scale), Q_ARG(QVariant, QVariant(height)),
                    Q_ARG(QVariant, QVariant(userScale)), Q_ARG(QVariant, QVariant(rows)));
                const double usedHeight = 48 + 6 + scale.toDouble() * (rows * 304 + 180 + 160);
                ok &= require(scale.toDouble() > 0 && usedHeight <= height + 0.01,
                              "both aspect ratios retain room for decks, library and waveforms at every UI scale");
            }
        }
    }
    ok &= require(main.find("rawUiScale: performanceScaleForHeight(") != std::string::npos
                      && main.find("rawUiScale: (width / baseUiWidth)") == std::string::npos,
                  "production viewport sizing uses height instead of window width");
    return ok;
}

bool desktopLibraryHeightTests(const std::string& main, const std::string& workspace)
{
    const auto sizingBegin = main.find("    readonly property bool desktopLibraryLayout:");
    const auto sizingEnd = main.find("    // MAIN LAYOUT", sizingBegin);
    QString helpers;
    if (!require(sizingBegin != std::string::npos && sizingEnd != std::string::npos,
                 "production workspace sizing bindings exist")
        || !extractFunctions(main, {"_dpr", "_snapScaleToPhysicalPixels", "_snapLengthToPhysicalPixels"}, helpers))
        return false;

    // Keep the real layout constraints; leave out the audio/rendering children.
    const auto viewport = [&](const char* beginMarker, const char* endMarker, const char* name) {
        const auto begin = workspace.find(beginMarker);
        const auto end = workspace.find(endMarker, begin);
        if (begin == std::string::npos || end == std::string::npos)
            return QString();
        return QString::fromStdString(workspace.substr(begin, end - begin))
            .replace("objectName: \"aioDeckControlBar\"", "")
            + QStringLiteral("objectName: \"") + QString::fromUtf8(name) + QStringLiteral("\"\n}\n");
    };
    const QString wave = viewport("        Item {\n            id: waveformViewport", "            Item {\n                id: waveformCanvas", "wave");
    const QString decks = viewport("        Item {\n            id: deckMixerViewport", "            readonly property real designWidth:", "decks");
    const QString extraDecks = viewport("        Item {\n            id: deckMixerViewport2", "            readonly property real designWidth:", "extraDecks");
    const QString transport = viewport("        Rectangle {\n            objectName: \"aioDeckControlBar\"", "            Rectangle {\n                anchors.top:", "transport");
    const auto libraryBegin = workspace.find("        Library {\n            id: librarySection");
    const auto libraryEnd = workspace.find("        Rectangle {\n            objectName: \"aioDeckControlBar\"", libraryBegin);
    if (!require(!wave.isEmpty() && !decks.isEmpty() && !extraDecks.isEmpty() && !transport.isEmpty()
                     && libraryBegin != std::string::npos && libraryEnd != std::string::npos,
                 "production desktop layout containers can be isolated"))
        return false;
    QString library = QString::fromStdString(workspace.substr(libraryBegin, libraryEnd - libraryBegin));
    library.replace("Library {", "Item { objectName: \"library\"");

    QQmlEngine engine;
    QQmlComponent scaleComponent(&engine);
    scaleComponent.setData("import QtQml; QtObject { property real scale: 1 }", QUrl());
    std::unique_ptr<QObject> userScale(scaleComponent.create());
    engine.rootContext()->setContextProperty("uiScaleController", userScale.get());
    QQmlComponent component(&engine);
    component.setData((QStringLiteral(R"(
        import QtQuick
        import QtQuick.Layouts
        import DJSoftware
        Item {
            id: window
            width: 1280; height: 600
            property real minimumHeight: 600
            readonly property real _refHeight: 800
            property bool allInOneMode: false
            property bool fourDeckMode: false
            property bool libraryExpanded: false
            property bool showDeckA: true
            property bool showDeckB: true
            property bool showWaveforms: true
            property bool showLibrary: true
            property bool showAioDeckControls: true
            property string activeMainTab: "performance"
            readonly property bool compactLayout: height < 720
            readonly property bool allInOnePanelActive: allInOneMode && activeMainTab !== "performance"
            readonly property bool sourcePageActive: activeMainTab === "source"
            readonly property bool effectiveLibraryVisible: allInOneMode ? activeMainTab === "library" : showLibrary
            readonly property real baseWaveformHeight: 340
            readonly property real baseDeckMixerHeight: baseUiHeight - baseWaveformHeight
        )") + helpers + QString::fromStdString(main.substr(sizingBegin, sizingEnd - sizingBegin))
        + QStringLiteral(R"(
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Item { Layout.fillWidth: true; Layout.minimumHeight: window.topBarHeight
                       Layout.preferredHeight: window.topBarHeight; Layout.maximumHeight: window.topBarHeight }
        )") + wave + decks + extraDecks + library + transport + QStringLiteral("\n}\n}")).toUtf8(), QUrl());
    std::unique_ptr<QObject> host(component.create());
    if (!require(host != nullptr, "production desktop sizing and layout constraints instantiate")) {
        std::cerr << component.errorString().toStdString();
        return false;
    }
    auto* item = qobject_cast<QQuickItem*>(host.get());
    auto* waveItem = host->findChild<QQuickItem*>("wave");
    auto* deckItem = host->findChild<QQuickItem*>("decks");
    auto* extraItem = host->findChild<QQuickItem*>("extraDecks");
    auto* libraryItem = host->findChild<QQuickItem*>("library");
    if (!require(item && waveItem && deckItem && extraItem && libraryItem,
                 "desktop layout exposes its actual viewport geometry"))
        return false;
    QQuickWindow preview;
    item->setParentItem(preview.contentItem());
    preview.show();
    bool ok = true;
    for (const double scale : {0.8, 1.0, 1.4}) {
        userScale->setProperty("scale", scale);
        for (const bool fourDeck : {false, true}) {
            host->setProperty("fourDeckMode", fourDeck);
            qreal minimumWave = 0, minimumDeck = 0;
            qreal referenceWave = 0, referenceDeck = 0, referenceLibrary = 0;
            for (const int width : {1280, 1920}) {
                for (const int height : {600, 720, 800, 900, 1080, 1440}) {
                    preview.resize(width, height);
                    item->setSize(QSizeF(width, height));
                    QTest::qWait(20);
                    if (width == 1280 && height == 600) {
                        minimumWave = waveItem->height();
                        minimumDeck = deckItem->height();
                    }
                    if (width == 1280 && height == 800) {
                        referenceWave = waveItem->height();
                        referenceDeck = deckItem->height();
                        referenceLibrary = libraryItem->height();
                        ok &= require(referenceWave > minimumWave && referenceDeck > minimumDeck,
                                      "the desktop retains the established reference size instead of shrinking every window to the smallest layout");
                    }
                    ok &= require(waveItem->height() >= minimumWave - 0.5
                                      && deckItem->height() >= minimumDeck - 0.5,
                                  "the upper desktop area never shrinks below its minimum-window size");
                    if (height >= 800 && referenceWave > 0) {
                        ok &= require(std::abs(waveItem->height() - referenceWave) < 0.5
                                          && std::abs(deckItem->height() - referenceDeck) < 0.5
                                          && (!fourDeck || std::abs(extraItem->height() - referenceDeck) < 0.5),
                                      "desktop waveforms and deck rows retain the reference height on wider and taller screens");
                        ok &= require(referenceLibrary > 0
                                          && std::abs(libraryItem->height() - referenceLibrary - (height - 800)) < 0.5,
                                      "above the reference height all additional desktop space belongs to the library");
                    }
                    ok &= require(std::abs(libraryItem->y() + libraryItem->height() - height) < 0.5,
                                  "the desktop library stays flush with the bottom of the window");
                }
            }
        }
    }
    host->setProperty("allInOneMode", true);
    QTest::qWait(20);
    ok &= require(!host->property("desktopLibraryLayout").toBool()
                      && host->property("performanceSizingHeight").toDouble() == item->height(),
                  "AIO performance retains the full available window height");
    host->setProperty("activeMainTab", "library");
    QTest::qWait(20);
    ok &= require(!waveItem->isVisible() && !deckItem->isVisible()
                      && libraryItem->height() > item->height() - host->property("topBarHeight").toDouble() - 50,
                  "the AIO library keeps its separate full-height view and compact transport");
    host->setProperty("allInOneMode", false);
    host->setProperty("showLibrary", false);
    QTest::qWait(20);
    ok &= require(!host->property("desktopLibraryLayout").toBool() && !libraryItem->isVisible()
                      && host->property("performanceSizingHeight").toDouble() == item->height(),
                  "hiding the desktop library returns the available height to performance");
    return ok;
}

bool sourceViewToggleTests(const std::string& main)
{
    QString methods;
    if (!extractFunctions(main, {"openLibrarySourceView", "toggleLibrarySourceView"}, methods)) return false;
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData((QStringLiteral(R"(
        import QtQml
        QtObject {
            property string activeMainTab: "performance"
            property string sourceReturnTab: "performance"
            readonly property bool sourcePageActive: activeMainTab === "source"
            property bool libraryExpanded: true
            property int closes: 0
            function closeTopBarPullDown() { ++closes }
        )") + methods + QStringLiteral("\n}")).toUtf8(), QUrl());
    std::unique_ptr<QObject> host(component.create());
    if (!require(host != nullptr, "source-view toggle handlers instantiate")) return false;
    bool ok = true;
    for (const auto& previous : {"performance", "library", "settings"}) {
        host->setProperty("activeMainTab", QString::fromUtf8(previous));
        QMetaObject::invokeMethod(host.get(), "toggleLibrarySourceView");
        ok &= require(host->property("sourcePageActive").toBool(), "source button opens the source selector");
        QMetaObject::invokeMethod(host.get(), "openLibrarySourceView");
        QMetaObject::invokeMethod(host.get(), "toggleLibrarySourceView");
        ok &= require(host->property("activeMainTab").toString() == QString::fromUtf8(previous),
                      "a second source-button click returns to the previous view, even after repeated open requests");
    }
    return ok;
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
            property bool touchMode: false
            property string focusedPanel: "tracks"
            function activateSidebarCursor() { ++navigations }
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
    library->setProperty("focusedPanel", "sidebar");
    const auto loadsBeforeSidebar = library->property("loads").toInt();
    ok &= require(confirm("A") && library->property("loads").toInt() == loadsBeforeSidebar
                      && library->property("navigations").toInt() == 2,
                  "confirmation activates the clicked sidebar cursor instead of an old track");
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

bool librarySwipeTests(const std::string& source)
{
    QString methods;
    for (const char* name : {"updateSwipe", "settleSwipe"}) {
        const auto begin = source.find(std::string("        function ") + name + "(");
        const auto end = source.find("\n        }", begin);
        if (!require(begin != std::string::npos && end != std::string::npos,
                     "track swipe function exists"))
            return false;
        methods += QString::fromStdString(source.substr(begin, end + 10 - begin)) + '\n';
    }
    QQmlEngine engine;
    auto row = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool scrollLocked: false
            property bool swipeLocked: false
            property bool swipeActive: false
            property real swipeX: 0
            property real swipeStartX: 0
            property real deckActionW: 184
            property real miscActionW: 148
            function closeSwipe() { swipeX = 0 }
    )") + methods + QStringLiteral("\n}"), "library swipe state instantiates");
    if (!row) return false;
    auto evaluate = [&](const QString& expression) {
        QQmlExpression result(QQmlEngine::contextForObject(row.get()), row.get(), expression);
        return result.evaluate();
    };
    bool ok = require(!evaluate("updateSwipe(3, 2)").toBool()
                      && !row->property("swipeActive").toBool(), "tap jitter does not start a swipe");
    ok &= require(!evaluate("updateSwipe(5, 24)").toBool()
                      && !evaluate("updateSwipe(90, 25)").toBool(),
                  "vertical scroll stays vertical even when the finger later moves sideways");
    evaluate("scrollLocked = false; updateSwipe(90, 4); settleSwipe()");
    ok &= require(row->property("swipeX").toDouble() == 184,
                  "right swipe reveals deck actions");
    evaluate("swipeStartX = swipeX; updateSwipe(-160, 0); settleSwipe()");
    ok &= require(row->property("swipeX").toDouble() == 0,
                  "an open row closes relative to its starting position");
    evaluate("swipeStartX = 0; updateSwipe(-80, 2); settleSwipe()");
    ok &= require(row->property("swipeX").toDouble() == -148,
                  "left swipe reveals local track actions");
    evaluate("miscActionW = 0; swipeStartX = 0; updateSwipe(-100, 0); settleSwipe()");
    ok &= require(row->property("swipeX").toDouble() == 0,
                  "USB rows cannot swipe into local mutation actions");
    evaluate("updateSwipe(1000, 0)");
    ok &= require(row->property("swipeX").toDouble() == 184,
                  "swipe travel is bounded by the action strip");
    return ok;
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
            property var beatOverride: null
            function beatDisplayInfo() {
                if (beatOverride) return beatOverride
                var beat = Math.floor(atomicState.position * 2)
                return { valid: true, beatInBar: ((beat % 4) + 4) % 4 + 1,
                         barNumber: Math.floor(beat / 4) + 1 }
            }
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
        + QStringLiteral("\nreadonly property int segs: 24\n")
        + QString::fromStdString(header.substr(header.find("        function litSegments(peak)"),
            header.find("        function segColor(i)") - header.find("        function litSegments(peak)")))
        + QStringLiteral("\n}"), "real header tick and meter bindings instantiate");
    if (!host)
        return false;
    QQmlExpression meterScale(QQmlEngine::contextForObject(host.get()), host.get(),
        QStringLiteral("litSegments(0) === 0 && litSegments(1) === 22 && litSegments(1.01) === 24"));
    if (!require(meterScale.evaluate().toBool(), "master red segments start at overload above 0 dBFS"))
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
    deck->setProperty("beatOverride", QVariantMap{
        {"valid", true}, {"beatInBar", 3}, {"barNumber", 17}});
    QMetaObject::invokeMethod(deck.get(), "progressChanged");
    ok &= require(bar() == 17, "header uses imported dynamic bar markers instead of a global BPM formula");
    deck->setProperty("trackData", QVariantMap{{"isBpmAnalyzed", false}});
    QQmlExpression valid(QQmlEngine::contextForObject(host.get()), host.get(),
                          QStringLiteral("beatInfo.valid"));
    ok &= require(!valid.evaluate().toBool(), "eject or unanalyzed track clears the header count");
    return ok;
}

bool pullDownHandleTests(const std::string& header)
{
    const auto handle = componentSection(header, "PullDownHandle");
    if (!require(!handle.empty(), "production pull-down handle exists")) return false;
    QQmlEngine engine;
    auto host = createQmlObject(engine, QStringLiteral(R"(
        import QtQuick
        Item {
            width: 400; height: 320
            property alias progress: state.topBarPullProgress
            property alias dragging: state.topBarPullDragging
            QtObject {
                id: state
                property real topBarPullProgress: 0
                property bool topBarPullDragging: false
                function openTopBarPullDown() { topBarPullProgress = 1 }
                function closeTopBarPullDown() { topBarPullProgress = 0 }
                function toggleTopBarPullDown() { topBarPullProgress = topBarPullProgress >= 0.5 ? 0 : 1 }
            }
    )") + QString::fromStdString(handle)
        + QStringLiteral("\nPullDownHandle { objectName: \"handle\"; controller: state }\n}"),
        "production moving handle instantiates");
    if (!host) return false;
    QQuickWindow window;
    window.resize(400, 320);
    qobject_cast<QQuickItem*>(host.get())->setParentItem(window.contentItem());
    window.show();
    QCoreApplication::processEvents();
    auto* grip = host->findChild<QQuickItem*>(QStringLiteral("handle"));
    if (!require(grip != nullptr, "handle is available")) return false;
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {200, 50});
    QTest::mouseMove(&window, {200, 100}, 20);
    QTest::mouseMove(&window, {200, 150}, 20);
    QCoreApplication::processEvents();
    bool ok = require(std::abs(host->property("progress").toDouble() - 100.0 / 144.0) < 0.02
                      && grip->y() > 130, "drag follows the finger using fixed coordinates while the handle moves");
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {200, 150});
    ok &= require(host->property("progress").toDouble() == 1
                      && !host->property("dragging").toBool(), "downward drag settles open");
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {200, 194});
    QTest::mouseMove(&window, {200, 94}, 20);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {200, 94});
    ok &= require(host->property("progress").toDouble() == 0, "moving handle can be dragged back closed");
    auto* touch = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
    QTest::touchEvent(&window, touch).press(0, {200, 50}, &window);
    QTest::touchEvent(&window, touch).move(0, {200, 150}, &window);
    QTest::touchEvent(&window, touch).release(0, {200, 150}, &window);
    QCoreApplication::processEvents();
    ok &= require(host->property("progress").toDouble() == 1, "touch drag opens the tray");
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {200, 194});
    ok &= require(host->property("progress").toDouble() == 0, "tapping the moving handle still toggles the tray");
    return ok;
}

bool libraryControllerPaneTests(const std::string& source)
{
    QString methods;
    if (!extractFunctions(source, {"navigateBrowserBack", "activateSidebarCursor",
                                  "selectSidebarItem", "moveSidebarCursor",
                                  "collapseSidebarItem", "confirmLibrarySelection"}, methods))
        return false;
    QQmlEngine engine;
    auto host = createQmlObject(engine, QStringLiteral(R"(
        import QtQuick
        Item {
            id: root
            property bool touchMode: false
            property string activeTab: "favorites"
            property string focusedPanel: "tracks"
            property string aioFocusZone: "tracks"
            property bool aioBrowseDrilled: false
            property string usbFocusArea: "tracks"
            property int sidebarCursorIndex: 1
            property bool sidebarCursorActive: true
            property var expandedPlaylists: ({})
            readonly property bool folderOpen: expandedPlaylists.folder === true
            property string currentPlaylistId: ""
            property string currentPlaylistName: ""
            property string currentSmartCollectionId: ""
            property string currentSmartCollectionName: ""
            property string librarySubTab: "allSongs"
            property int sourceOpens: 0
            property int loads: 0
            property int touchActivations: 0
            property string loadedDeck: ""
            property var window: ({openLibrarySourceView: function() { ++root.sourceOpens }})
            property var entries: [
                {type: "nav", tab: "library"}, {type: "nav", tab: "favorites"},
                {type: "playlist", data: {id: "folder", name: "Folder", hasChildren: true}},
                {type: "playlist", data: {id: "list", name: "Playlist", hasChildren: false}}]
            function sidebarEntryAt(i) { return entries[i] }
            function sidebarTotalCount() { return entries.length }
            function _ensureSidebarVisible(i) {}
            function setFocusedPanel(panel) { focusedPanel = panel }
            function loadPlaylistTracks() {}
            function loadSmartCollectionTracks(id) {}
            function toggleExpanded(id) {
                var next = Object.assign({}, expandedPlaylists)
                next[id] = !next[id]
                expandedPlaylists = next
            }
            function loadCursorTrackToDeck(deck) { ++loads; loadedDeck = deck }
            function aioBrowseCursorLeft() { aioFocusZone = "nav" }
            function aioBrowseCursorActivate() { ++touchActivations; aioFocusZone = "tracks" }
            function usbNavigateBack() {}
            function activateUsbCursor() {}
        )") + methods + QStringLiteral("\n}"), "controller pane navigation instantiates");
    if (!host) return false;
    auto run = [&](const QString& script) {
        QQmlExpression expression(QQmlEngine::contextForObject(host.get()), host.get(), script);
        expression.evaluate();
        return !expression.hasError();
    };
    bool ok = require(run("navigateBrowserBack()")
                      && host->property("focusedPanel").toString() == "sidebar"
                      && host->property("activeTab").toString() == "favorites"
                      && host->property("sidebarCursorIndex").toInt() == 1,
                      "BACK reaches the left sidebar without resetting the selected collection");
    ok &= require(run("moveSidebarCursor(1); confirmLibrarySelection('B')")
                      && host->property("focusedPanel").toString() == "sidebar"
                      && host->property("folderOpen").toBool()
                      && host->property("loads").toInt() == 0,
                      "encoder selects and opens a sidebar folder without loading a stale track");
    ok &= require(run("moveSidebarCursor(1); confirmLibrarySelection('B')")
                      && host->property("currentPlaylistId").toString() == "list"
                      && host->property("focusedPanel").toString() == "tracks"
                      && host->property("loads").toInt() == 0,
                      "confirming a leaf playlist moves the cursor back into its track list");
    ok &= require(run("confirmLibrarySelection('B')")
                      && host->property("loads").toInt() == 1
                      && host->property("loadedDeck").toString() == "B",
                      "next encoder press loads the selected track to the requested deck");
    ok &= require(run("navigateBrowserBack(); moveSidebarCursor(-2); confirmLibrarySelection('A')")
                      && host->property("activeTab").toString() == "favorites",
                      "controller can return left and select another collection");
    ok &= require(run("touchMode = true; navigateBrowserBack(); confirmLibrarySelection('A')")
                      && host->property("touchActivations").toInt() == 1
                      && host->property("loads").toInt() == 1,
                      "AIO confirmation navigates the left rail before allowing a track load");
    return ok;
}

bool sourceCursorPersistenceTests(const std::string& source)
{
    QString methods;
    if (!extractFunctions(source, {"rememberSourceCursor", "restoreSourceCursor",
                                   "ensureSourceCursor", "moveCursor", "goBack"}, methods))
        return false;
    QQmlEngine engine;
    auto state = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool navigationReady: true
            property string cursorDeviceId: "usb-b"
            property int sourceCursorIndex: 2
            property int serviceCursorIndex: 0
            property int serviceCount: 1
            property string sourcePane: "services"
            property var devices: [{id: "usb-a"}, {id: "usb-b"}]
            readonly property int sourceCount: 1 + devices.length
            property var deviceList: ({positionViewAtIndex: function(i, mode) {}})
            signal sourceBackRequested()
        )") + methods + QStringLiteral("\n}"), "source cursor state instantiates");
    if (!state) return false;
    auto evaluate = [&](const QString& script) {
        QQmlExpression expression(QQmlEngine::contextForObject(state.get()), state.get(), script);
        expression.evaluate();
        return !expression.hasError();
    };
    bool ok = require(evaluate("devices = [{id: 'usb-b'}, {id: 'usb-a'}]; restoreSourceCursor()")
                      && state->property("sourceCursorIndex").toInt() == 1
                      && state->property("sourcePane").toString() == QStringLiteral("services"),
                      "device refresh preserves selected USB identity and navigation pane");
    ok &= require(evaluate("goBack()") && state->property("sourceCursorIndex").toInt() == 1,
                  "back returns to the selected source instead of resetting to Local");
    ok &= require(evaluate("sourceCursorIndex = 2; rememberSourceCursor()")
                      && state->property("cursorDeviceId").toString() == QStringLiteral("usb-a"),
                  "pointer-selected source becomes the navigation origin");
    ok &= require(evaluate("devices = [{id: 'usb-b'}]; restoreSourceCursor()")
                      && state->property("sourceCursorIndex").toInt() == 0
                      && state->property("cursorDeviceId").toString().isEmpty(),
                  "removed USB source falls back to a valid local cursor");
    return ok;
}

bool latencyPopupToggleTests(const std::string& header)
{
    const auto popupBegin = header.find("    Popup {\n        id: latencyPopup");
    const auto content = header.find("        contentItem: Column {", popupBegin);
    const auto mouseId = header.find("                id: latMouse");
    const auto mouseBegin = header.rfind("            MouseArea {", mouseId);
    const auto mouseEnd = header.find("\n            }", mouseId);
    if (!require(popupBegin != std::string::npos && content != std::string::npos
                 && mouseBegin != std::string::npos && mouseEnd != std::string::npos,
                 "production latency controls exist")) return false;
    QQmlEngine engine;
    auto host = createQmlObject(engine, QStringLiteral(R"(
        import QtQuick
        import QtQuick.Controls
        Item {
            width: 400; height: 300
            id: root
            function refreshLatencyInfo() {}
    )") + QString::fromStdString(header.substr(popupBegin, content - popupBegin))
        + QStringLiteral("height: 100\n}\nRectangle { id: monitorBlock; width: 120; height: 40\n")
        + QString::fromStdString(header.substr(mouseBegin, mouseEnd + 14 - mouseBegin))
        + QStringLiteral("\n}\n}"), "production latency toggle instantiates");
    if (!host) return false;
    QQuickWindow window;
    window.resize(400, 300);
    qobject_cast<QQuickItem*>(host.get())->setParentItem(window.contentItem());
    window.show();
    QCoreApplication::processEvents();
    auto visible = [&] {
        QQmlExpression expression(QQmlEngine::contextForObject(host.get()), host.get(),
                                  QStringLiteral("latencyPopup.visible"));
        return expression.evaluate().toBool();
    };
    bool ok = true;
    for (int repeat = 0; repeat < 2; ++repeat) {
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {60, 20});
        ok &= require(QTest::qWaitFor(visible), "latency opens on click");
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {60, 20});
        ok &= require(QTest::qWaitFor([&] { return !visible(); }), "second latency click closes without reopening");
    }
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
            property var crossfaderState: ({
                position: -0.6, sharpness: 1.0, curveMode: "exponential",
                assignA: "T", assignB: "A", assignC: "B", assignD: "T"
            })
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
    ok &= require(std::abs(bar->property("cfPos").toDouble() + 0.6) < 1e-6
                      && bar->property("assignA").toString() == QStringLiteral("T")
                      && bar->property("assignB").toString() == QStringLiteral("A")
                      && bar->property("assignC").toString() == QStringLiteral("B")
                      && bar->property("assignD").toString() == QStringLiteral("T")
                      && bar->property("cfSharpness").toDouble() == 1.0,
                  "opening crossfader controls restores the mixer state instead of resetting it");
    bar->setProperty("cfPos", 0.35);
    ok &= require(std::abs(mixer->property("appliedPosition").toDouble() - 0.35) < 1e-6,
                  "the hosted crossfader forwards position changes to MixerControl");
    const double lastUiPosition = mixer->property("appliedPosition").toDouble();
    mixer->setProperty("crossfaderState", QVariantMap{
        {QStringLiteral("position"), 0.8}, {QStringLiteral("sharpness"), 0.0},
        {QStringLiteral("curveMode"), QStringLiteral("linear")},
        {QStringLiteral("assignA"), QStringLiteral("B")},
        {QStringLiteral("assignB"), QStringLiteral("T")},
        {QStringLiteral("assignC"), QStringLiteral("A")},
        {QStringLiteral("assignD"), QStringLiteral("B")}});
    ok &= require(std::abs(bar->property("cfPos").toDouble() - 0.8) < 1e-6
                      && bar->property("assignA").toString() == QStringLiteral("B")
                      && bar->property("assignB").toString() == QStringLiteral("T")
                      && bar->property("cfCurveMode").toString() == QStringLiteral("linear")
                      && mixer->property("appliedPosition").toDouble() == lastUiPosition,
                  "hardware state updates mirror all controls without sending stale UI state back");
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
        import DJSoftware
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

QQuickItem* findVisualItem(QQuickItem* parent, const QString& name)
{
    if (parent->objectName() == name)
        return parent;
    for (auto* child : parent->childItems()) {
        if (auto* match = findVisualItem(child, name))
            return match;
    }
    return nullptr;
}

bool performancePadsThemeRuntimeTests()
{
    QQmlEngine engine;
    auto deck = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool isPlaying: false
            property bool scratchVisualActive: false
            property real currentBpm: 120
            property real progress: 0
            property real preRollSeconds: 0
            property real keySemitoneOffset: 0
            property var hotCues: [
                { set: true, color: "#f6e05e", positionSec: 30 },
                { set: false }
            ]
            property var savedLoops: [null, null,
                { set: true, color: "#48bb78", inSec: 60, lengthBeats: 4 }]
            property int cueTriggers: 0
            property int cueStores: 0
            property int fxReleases: 0
            property string appliedFx: ""
            property real seekPosition: -1
            function getDuration() { return 120 }
            function getPlayheadPositionAtomic() { return 30 }
            function getVisualPositionQml() { return 30 }
            function triggerCuePad(index) { ++cueTriggers }
            function storeCuePad(index) { ++cueStores }
            function play() { isPlaying = true }
            function pause() { isPlaying = false }
            function setPosition(value) { seekPosition = value }
            function beatJump(beats) { seekPosition = beats }
            function setPadFx(effect, amount) { appliedFx = effect }
            function clearPadFx() { ++fxReleases; appliedFx = "" }
            function startEchoOut() { appliedFx = "EchoOut" }
            function stopEchoOut() { appliedFx = "" }
            signal playingChanged()
            signal tempoChanged()
            signal reverseChanged()
            signal trackLoaded()
        }
    )"), "performance pad theme engine instantiates");
    if (!deck)
        return false;
    auto router = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property int deckAPadMode: 0
            property int deckBPadMode: 0
            property int deckAKeyShiftRange: 1
            property int deckBKeyShiftRange: 1
            function performancePadFxMomentary(deckId) { return -1 }
            function performancePadFxToggle(deckId) { return -1 }
            signal performancePadStateChanged(string changedDeckId)
        }
    )"), "performance pad theme router instantiates");
    if (!router)
        return false;
    engine.rootContext()->setContextProperty(QStringLiteral("midiManager"), router.get());
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromUtf8(BROCKDJ_SOURCE_DIR)
        + QStringLiteral("/src/qml/performance/PerformancePads.qml")));
    std::unique_ptr<QObject> pads(component.create());
    if (!require(pads != nullptr, "the full themed performance pad component instantiates")) {
        std::cerr << component.errorString().toStdString() << '\n';
        return false;
    }
    pads->setProperty("deckId", QStringLiteral("deckC"));
    pads->setProperty("engine", QVariant::fromValue(deck.get()));
    pads->setProperty("compact", true);
    auto* item = qobject_cast<QQuickItem*>(pads.get());
    QQuickWindow window;
    window.resize(420, 100);
    item->setParentItem(window.contentItem());
    item->setSize(QSizeF(420, 100));
    window.show();
    QCoreApplication::processEvents();
    auto* cue = findVisualItem(item, QStringLiteral("performancePad0"));
    auto* empty = findVisualItem(item, QStringLiteral("performancePad1"));
    auto* loop = findVisualItem(item, QStringLiteral("performancePad2"));
    if (!require(cue && empty && loop, "the themed grid exposes individual pads"))
        return false;
    bool ok = require(cue->property("padAccent").value<QColor>() == QColor("#f6e05e")
                          && loop->property("padAccent").value<QColor>() == QColor("#48bb78")
                          && cue->property("color").value<QColor>() != QColor("#f6e05e")
                          && cue->property("color").value<QColor>() != empty->property("color").value<QColor>(),
                      "custom cue and loop colors remain identifiable without full-bright pad fills");
    const QColor idleColor = cue->property("color").value<QColor>();
    const QPoint point = cue->mapToScene(QPointF(cue->width() / 2, cue->height() / 2)).toPoint();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point);
    ok &= require(cue->property("padActive").toBool()
                      && cue->property("color").value<QColor>() != idleColor
                      && deck->property("cueTriggers").toInt() == 1,
                  "a pad press immediately changes its themed state and still triggers its cue");
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point);
    ok &= require(!cue->property("padActive").toBool() && !deck->property("isPlaying").toBool()
                      && std::abs(deck->property("seekPosition").toDouble() - 0.25) < 1e-6,
                  "cue release clears the highlight and preserves stopped-deck preview behavior");
    ok &= clickItem(window, empty, "the empty themed pad remains clickable");
    ok &= require(deck->property("cueStores").toInt() == 1,
                  "an empty themed pad still stores a cue");
    pads->setProperty("activeTab", 1);
    QCoreApplication::processEvents();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point);
    ok &= require(cue->property("padFxLit").toBool()
                      && deck->property("appliedFx").toString() == QStringLiteral("Echo"),
                  "momentary FX retain their action and active visual state");
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point);
    ok &= require(!cue->property("padFxLit").toBool()
                      && deck->property("fxReleases").toInt() == 1,
                  "releasing a momentary FX clears the effect and its visual state");
    auto* toggle = findVisualItem(item, QStringLiteral("performancePad4"));
    ok &= clickItem(window, toggle, "the themed latched FX pad remains clickable");
    ok &= require(toggle && toggle->property("padFxLit").toBool(),
                  "latched FX keep their highlight after release");
    ok &= clickItem(window, toggle, "the latched FX can be switched off");
    ok &= require(toggle && !toggle->property("padFxLit").toBool(),
                  "switching off a latched FX clears its highlight");
    for (const QSizeF size : {QSizeF(420, 100), QSizeF(537, 100), QSizeF(720, 160)}) {
        item->setSize(size);
        window.resize(size.toSize());
        for (int page = 0; page < 4; ++page) {
            pads->setProperty("activeTab", page);
            QCoreApplication::processEvents();
            auto* bar = findVisualItem(item, QStringLiteral("padModeBar"));
            for (int tabIndex = 0; tabIndex < 4; ++tabIndex) {
                auto* tab = findVisualItem(item, QStringLiteral("padTab%1").arg(tabIndex));
                auto* tabLabel = findVisualItem(item, QStringLiteral("padTabLabel%1").arg(tabIndex));
                auto* previous = tabIndex > 0
                    ? findVisualItem(item, QStringLiteral("padTab%1").arg(tabIndex - 1)) : nullptr;
                ok &= require(bar && tab && tabLabel
                                  && tab->x() == std::round(tab->x())
                                  && tab->width() == std::round(tab->width())
                                  && (!previous || tab->x() == previous->x() + previous->width())
                                  && (tabIndex != 3 || tab->x() + tab->width() == bar->width() - 1),
                              "mode tabs share a continuous frame with integer-aligned adjoining cells");
                ok &= require(tabLabel && !tabLabel->property("truncated").toBool(),
                              "mode captions remain readable without truncation");
            }
            for (int index = 0; index < 8; ++index) {
                auto* pad = findVisualItem(item, QStringLiteral("performancePad%1").arg(index));
                auto* label = findVisualItem(item, QStringLiteral("padLabel%1").arg(index));
                auto* number = findVisualItem(item, QStringLiteral("padIndex%1").arg(index));
                ok &= require(pad && label && number && pad->height() > 20
                                  && (label->y() >= number->y() + number->height()
                                      || label->x() >= number->x() + number->width())
                                  && label->y() + label->height() <= pad->height()
                                  && label->width() <= pad->width(),
                              "all pad pages keep labels within their compact and full-size frames");
                ok &= require(label && label->property("color").value<QColor>().lightnessF() > 0.5,
                              "inactive pad labels retain readable contrast");
                ok &= require(label && !label->property("truncated").toBool(),
                              "pad values and effect names remain readable at compact sizes");
            }
        }
    }
    pads->setProperty("deckId", QStringLiteral("deckA"));
    router->setProperty("deckAPadMode", 4);
    QCoreApplication::processEvents();
    ok &= require(pads->property("keyShiftMode").toBool()
                      && cue->property("padActive").toBool()
                      && !empty->property("padActive").toBool(),
                  "controller key-shift mode highlights only the current semitone pad");
    deck->setProperty("keySemitoneOffset", 1);
    QCoreApplication::processEvents();
    ok &= require(!cue->property("padActive").toBool()
                      && empty->property("padActive").toBool(),
                  "key-shift highlights follow engine changes without changing pad routing");
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
            property real stripePeriod: 16
            property real stripeWidth: 8
            property real stripePhase: 0
            property bool impulseMode: false
            property bool horizontalImpulse: false
            Rectangle { anchors.fill: parent; color: "black" }
            Repeater {
                model: parent.impulseMode ? 0 : Math.ceil(800 / parent.stripePeriod) + 1
                Rectangle {
                    required property int index
                    x: index * parent.stripePeriod - parent.stripePhase
                    width: parent.stripeWidth; height: 600
                    color: parent.stripeColor
                }
            }
            Rectangle {
                x: parent.horizontalImpulse ? 0 : 158
                y: parent.horizontalImpulse ? 98 : 0
                width: parent.horizontalImpulse ? parent.width : 4
                height: parent.horizontalImpulse ? 4 : parent.height
                color: "white"; visible: parent.impulseMode
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
    auto* startupTransition = startupItem->findChild<QQuickItem*>("startupTransition");
    ok &= require(startupTransition && QTest::qWaitFor([&] {
                      return !startupTransition->isVisible();
                  }, 1000),
                  "startup fade completes and releases the overlay after runtime readiness");
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
                      && backdrop->property("live").toBool()
                      && backdrop->property("maximumCaptureSize").toSize() == QSize(512, 512),
                  "exit captures only mainLayout into bounded live textures");
    const bool software = window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    if (software)
        ok &= require(!backdrop->property("blurActive").toBool(),
                      "software exit has an opaque readable fallback");
    QQmlExpression activate(QQmlEngine::contextForObject(backdrop), backdrop,
                            QStringLiteral("graphicsAvailable = true; applicationActive = true"));
    activate.evaluate();
    QCoreApplication::processEvents();
    QPointer<QObject> capture(backdrop->findChild<QObject*>("backdropCapture"));
    ok &= require(capture && capture->property("live").toBool()
                      && capture->property("mipmap").toBool()
                      && capture->property("textureSize").toSize()
                          == backdrop->property("sourceTextureSize").toSize()
                      && backdrop->property("sourceTextureSize").toSize().width() <= 1024
                      && backdrop->property("sourceTextureSize").toSize().height() <= 1024
                      && backdrop->property("blurRadius").toDouble() == 28
                      && !capture->property("recursive").toBool(),
                  "exit uses bounded supersampled live capture with prefiltered fullscreen blur");
    if (!software) {
        startupItem->setProperty("welcomeActive", false);
        appWindow->setProperty("uncleanShutdownWarningVisible", false);
        QTest::qWait(80);
        const QImage first = window.grabWindow();
        if (!require(!first.isNull(), "exit blur renders a framebuffer"))
            return false;
        const QPoint probePoint(first.width() / 10, first.height() / 4);
        const QColor firstPixel = first.pixelColor(probePoint);
        ok &= require(std::abs(firstPixel.red()
                              - first.pixelColor(probePoint + QPoint(8, 0)).red()) < 30,
                      "exit really blurs stripes instead of merely dimming them");
        mainLayout->setProperty("impulseMode", true);
        for (const bool horizontal : {false, true}) {
            mainLayout->setProperty("horizontalImpulse", horizontal);
            QTest::qWait(80);
            const QImage impulse = window.grabWindow();
            if (!require(!impulse.isNull(), "exit blur impulse renders a framebuffer"))
                return false;
            const double dpr = window.devicePixelRatio();
            const auto impulseRed = [&](int distance) {
                return impulse.pixelColor(qRound((horizontal ? 80 : 160 + distance) * dpr),
                                          qRound((horizontal ? 100 + distance : 100) * dpr)).red();
            };
            int previousIntensity = impulseRed(0);
            const int backgroundIntensity = impulseRed(60);
            ok &= require(previousIntensity > backgroundIntensity + 3,
                          "fullscreen blur retains the central response of a thin line");
            for (const int distance : {4, 8, 12, 16, 20, 24, 28, 36, 48}) {
                const int intensity = impulseRed(distance);
                ok &= require(intensity <= previousIntensity + 1,
                              "fullscreen blur falls off smoothly without displaced ghost lines");
                previousIntensity = intensity;
            }
        }
        mainLayout->setProperty("impulseMode", false);
        mainLayout->setProperty("stripePeriod", 2);
        mainLayout->setProperty("stripeWidth", 1);
        int lowestMean = 255;
        int highestMean = 0;
        for (const double phase : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            mainLayout->setProperty("stripePhase", phase);
            QTest::qWait(50);
            const QImage moving = window.grabWindow();
            if (!require(!moving.isNull(), "moving fine-line blur renders a framebuffer"))
                return false;
            int sum = 0;
            int lowest = 255;
            int highest = 0;
            for (int x = probePoint.x(); x < probePoint.x() + 80; ++x) {
                const int value = moving.pixelColor(x, probePoint.y()).red();
                sum += value;
                lowest = std::min(lowest, value);
                highest = std::max(highest, value);
            }
            lowestMean = std::min(lowestMean, sum / 80);
            highestMean = std::max(highestMean, sum / 80);
            ok &= require(highest - lowest < 10,
                          "fullscreen blur suppresses fine-line spatial aliasing");
        }
        std::cout << "Fullscreen blur fine-line temporal range="
                  << highestMean - lowestMean << '\n';
        ok &= require(highestMean - lowestMean < 10,
                      "moving fine lines do not pulse behind fullscreen blur");
        mainLayout->setProperty("stripePeriod", 16);
        mainLayout->setProperty("stripeWidth", 8);
        mainLayout->setProperty("stripePhase", 0);
        mainLayout->setProperty("stripeColor", QColor(Qt::red));
        QTest::qWait(50);
        const QImage updated = window.grabWindow();
        ok &= require(!updated.isNull()
                          && updated.pixelColor(probePoint).green() < firstPixel.green()
                          && updated.pixelColor(probePoint).red() > updated.pixelColor(probePoint).green(),
                      "exit blur follows backing updates while the prompt stays open");
        appWindow->setProperty("exitPromptVisible", false);
        QCoreApplication::processEvents();
        appWindow->setProperty("exitPromptVisible", true);
        QTest::qWait(50);
        const QImage reopened = window.grabWindow();
        ok &= require(!reopened.isNull() && reopened.pixelColor(probePoint).green() < firstPixel.green(),
                      "reopening exit restores the current live background");
        mainLayout->setProperty("stripeColor", QColor(Qt::blue));
        qobject_cast<QQuickItem*>(mainLayout.get())->setSize(QSizeF(640, 480));
        qobject_cast<QQuickItem*>(host.get())->setSize(QSizeF(640, 480));
        window.resize(640, 480);
        QTest::qWait(80);
        const QImage resized = window.grabWindow();
        ok &= require(!resized.isNull()
                          && resized.pixelColor(36, 40).blue() > resized.pixelColor(36, 40).red()
                          && backdrop->property("captureRect").toRectF().size() == QSizeF(640, 480),
                      "resizing keeps the live exit background at current geometry and content");
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
    qmlRegisterType<ScrollingWaveformItemStub>("DJSoftware", 1, 0, "OverviewWaveformItem");
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
            property bool quantizeEnabled1: false
            property bool quantizeEnabled2: false
            property real beatDiv1: 0.25
            property real beatDiv2: 0.25
            property real displayBpm1: 120
            property real displayBpm2: 120
            property real primaryParam1: 0.5
            property real primaryParam2: 0.5
            readonly property var reverbSizeSteps: [0.01, 0.10, 0.25, 0.50, 0.75, 0.90, 1.00]
            property bool enabled1: false
            property bool enabled2: false
            property string effectType1: "---"
            property string effectType2: "---"
            property string soundColorMode: "Filter"
            property real soundColorParam: 0.5
            property bool soundColorAdjustEnabled: true
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
            function setQuantizeEnabled(unit, value) {
                if (unit === 1) quantizeEnabled1 = value; else quantizeEnabled2 = value
                if (value) setSyncEnabled(unit, true)
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
        QtObject {
            property var devices: []
            property string selectedDeviceId: ""
            function chooseDevice(id) { selectedDeviceId = id }
            function mountDevice(id) {}
            function ejectDevice(id) {}
        }
    )"), "QML merge device context instantiates");
    auto deckA = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool hasTrack: true
            property bool readOnlyExternalTrack: false
            property bool isPlaying: false
            property string externalSourceId: ""
            property string trackTitle: "After Hours / Extended Mix"
            property string trackArtist: "Northbound"
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
            property string trackTitle: "Night Drive"
            property string trackArtist: "Coastal Lines"
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
            property var uiState: ({})
            function getUiState(key, fallback) {
                return uiState[key] === undefined ? fallback : uiState[key]
            }
            function setUiState(key, value) { uiState[key] = value }
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
    auto* songTitle = quickA ? quickA->findChild<QObject*>(QStringLiteral("deckTrackTitle")) : nullptr;
    auto* songArtist = quickA ? quickA->findChild<QObject*>(QStringLiteral("deckTrackArtist")) : nullptr;
    ok &= require(songTitle && songArtist
                      && songTitle->property("text").toString() == deckA->property("trackTitle").toString()
                      && songArtist->property("text").toString() == deckA->property("trackArtist").toString(),
                  "song sidebar shows title and artist from its assigned deck");
    deckA->setProperty("currentBpm", 129.99);
    QCoreApplication::processEvents();
    QObject* bpmWhole = quickA ? quickA->findChild<QObject*>(QStringLiteral("bpmWhole")) : nullptr;
    QObject* bpmDecimals = quickA ? quickA->findChild<QObject*>(QStringLiteral("bpmDecimals")) : nullptr;
    auto* bpmSeparator = quickA ? quickA->findChild<QQuickItem*>(QStringLiteral("bpmSeparator")) : nullptr;
    ok &= require(bpmWhole && bpmDecimals
                      && bpmWhole->property("text").toString() == QStringLiteral("130")
                      && bpmDecimals->property("text").toString() == QStringLiteral(".0"),
                  "the split BPM display rounds the whole and decimal parts together");
    ok &= require(bpmSeparator && bpmSeparator->height() == 1
                      && std::abs(bpmSeparator->mapToScene(QPointF()).y()
                                  - std::round(bpmSeparator->mapToScene(QPointF()).y())) < 1e-6,
                  "the BPM bottom separator is one pixel tall and aligned to a whole scene pixel");
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
    if (beatFxPanel) {
        fxManager->setProperty("effectType1", QStringLiteral("Reverb"));
        auto* tempoSync = beatFxPanel->findChild<QQuickItem*>("fxTempoSyncRow");
        auto* effectQuantize = beatFxPanel->findChild<QQuickItem*>("fxQuantizeRow");
        ok &= require(tempoSync && tempoSync->isVisible() && tempoSync->isEnabled()
                          && tempoSync->opacity() < 1.0 && effectQuantize
                          && effectQuantize->isVisible() && effectQuantize->isEnabled()
                          && effectQuantize->opacity() < 1.0,
                      "Reverb keeps tempo and quantize controls available but visually subdued");
        QTest::qWait(220); // Finish the side-panel reveal before real pointer input.
        const bool previousSync = fxManager->property("syncEnabled1").toBool();
        ok &= clickItem(performanceWindow, tempoSync, "subdued tempo sync remains clickable for Reverb");
        ok &= require(fxManager->property("syncEnabled1").toBool() != previousSync,
                      "tempo sync can be changed before selecting a timing effect");
        ok &= clickItem(performanceWindow, tempoSync, "subdued tempo sync can restore its state");
        const double beatDivision = fxManager->property("beatDiv1").toDouble();
        const QList<double> percentages {0.01, 0.10, 0.25, 0.50, 0.75, 0.90, 1.0};
        for (int index = 0; index < percentages.size(); ++index) {
            ok &= require(QMetaObject::invokeMethod(beatFxPanel, "setDivisionIndex",
                              Q_ARG(QVariant, QVariant(index))),
                          "Reverb percentage can be selected in the performance panel");
            QCoreApplication::processEvents();
            auto* value = beatFxPanel->findChild<QObject*>(QStringLiteral("fxParameterValue"));
            ok &= require(std::abs(fxManager->property("primaryParam1").toDouble()
                                      - percentages[index]) < 1e-6
                              && fxManager->property("beatDiv1").toDouble() == beatDivision
                              && value && value->property("text").toString()
                                  == QString::number(qRound(percentages[index] * 100)),
                          "Reverb displays and changes percentage, not beat duration");
        }
        fxManager->setProperty("effectType1", QStringLiteral("Echo"));
        QMetaObject::invokeMethod(beatFxPanel, "setDivisionIndex", Q_ARG(QVariant, QVariant(2)));
        ok &= require(fxManager->property("beatDiv1").toDouble() == 0.25,
                      "timed effects retain beat-division selection");
    }
    for (const bool open : {false, true}) {
        performance->setProperty("rightPanelOpen", open);
        std::unique_ptr<QObject> restored(performanceComponent.create(&context));
        ok &= require(restored && restored->property("rightPanelOpen").toBool() == open,
                      "FX panel open and closed state survives component recreation");
    }
    for (auto* panel : {quickA, quickB, gridPanel, beatFxPanel}) {
        auto* visualPanel = qobject_cast<QQuickItem*>(panel);
        auto* scroll = panel ? panel->findChild<QQuickItem*>(
            panel == gridPanel ? QStringLiteral("gridScroll")
                : (panel == beatFxPanel ? QStringLiteral("fxScroll")
                                       : QStringLiteral("deckScroll"))) : nullptr;
        ok &= require(visualPanel && scroll
                          && scroll->x() == 1 && scroll->y() == 1
                          && scroll->width() == visualPanel->width() - 2
                          && scroll->height() == visualPanel->height() - 2,
                      "sidebar content is inset so it cannot paint over its single outer frame");
    }
    auto* mixBar = beatFxPanel ? beatFxPanel->findChild<QQuickItem*>(QStringLiteral("fxMixBar")) : nullptr;
    QQuickItem* mixFill = nullptr;
    if (mixBar) {
        for (auto* child : mixBar->childItems()) {
            if (QByteArray(child->metaObject()->className()).startsWith("QQuickRectangle")) {
                mixFill = child;
                break;
            }
        }
    }
    for (const double mix : {0.0, 0.5, 1.0}) {
        fxManager->setProperty("wetDry1", mix);
        QCoreApplication::processEvents();
        ok &= require(mixBar && mixFill && mixFill->x() == 1 && mixFill->y() == 1
                          && mixFill->width() <= mixBar->width() - 2
                          && mixFill->height() == mixBar->height() - 2,
                      "FX level fill leaves its one-pixel border visible at every mix amount");
    }

    // Resize the existing instances: construction at 900x600 hid shrinking rows.
    const auto checkPanelGeometry = [&](QObject* panel, const char* label) {
        bool valid = true;
        if (!panel)
            return false;
        for (auto* item : panel->findChildren<QQuickItem*>()) {
            const QByteArray type(item->metaObject()->className());
            if (type.contains("ColumnLayout") || type.contains("GridLayout")) {
                for (auto* row : item->childItems()) {
                    if (!row->isVisible() || row->width() <= 0 || row->height() <= 0)
                        continue;
                    valid &= row->y() >= -0.5
                        && row->y() + row->height() <= item->height() + 0.5;
                    for (auto* other : item->childItems()) {
                        if (other == row || !other->isVisible() || other->width() <= 0 || other->height() <= 0)
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
        for (int height : {180, 240, 320, 399, 400, 420, 800, 2160, 240, 600}) {
            performanceWindow.resize(width, height);
            performanceItem->setSize(QSizeF(width, height));
            performance->setProperty("leftPanel", "deck");
            QTest::qWait(220);
            const qreal sidebarWidth = performance->property("panelWidth").toDouble();
            const qreal expectedPanelWidth = width >= 1600 && height >= 700 ? 232 : 208;
            ok &= require(std::abs(sidebarWidth - expectedPanelWidth) < 0.01,
                          "sidebar width follows the bounded display profile rather than stretching with the viewport");
            for (auto* fittedPanel : {quickA, quickB, beatFxPanel}) {
                auto* viewport = fittedPanel->findChild<QObject*>(
                    fittedPanel == beatFxPanel ? "fxScroll" : "deckScroll");
                ok &= require(viewport && !viewport->property("interactive").toBool()
                                  && std::abs(viewport->property("contentHeight").toDouble()
                                              - viewport->property("height").toDouble()) < 0.5
                                  && std::abs(viewport->property("contentY").toDouble()) < 0.5,
                              "song info and FX fit their viewport without scrolling after resizing");
            }
            auto* levelDial = beatFxPanel->findChild<QQuickItem*>("fxLevelDial");
            auto* levelBar = beatFxPanel->findChild<QQuickItem*>("fxMixBar");
            ok &= require(levelDial && levelBar
                              && levelDial->isVisible() == (height >= 400)
                              && levelBar->isVisible() == (height < 400),
                          "FX shows one level control and switches cleanly between compact and expanded layouts");
            auto* railBackground = performance->findChild<QQuickItem*>("leftBackdrop");
            ok &= require(railBackground && std::abs(railBackground->height() - height) < 0.5
                              && std::abs(beatFxPanel->property("height").toDouble() - height) < 0.5,
                          "both sidebars retain the full waveform viewport height");
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
            const QRectF quantizeBounds = quantize->mapRectToItem(qobject_cast<QQuickItem*>(quickB), quantize->boundingRect());
            ok &= require(quantizeBounds.bottom() >= quickB->property("height").toDouble() - 12,
                          "deck controls stay anchored to the bottom of each full-height deck area");
            const bool previousQuantize = deckB->property("quantizeEnabled").toBool();
            ok &= revealAction(quickB, quantize)
                && clickItem(performanceWindow, quantize, "scrolled deck B quantize is clickable");
            ok &= require(deckB->property("quantizeEnabled").toBool() != previousQuantize,
                          "deck B bottom action stays functional after resizing");
            auto* fxQuantize = beatFxPanel->findChild<QQuickItem*>(QStringLiteral("fxQuantizeRow"));
            const bool previousFxQuantize = fxManager->property("quantizeEnabled1").toBool();
            const bool otherFxQuantize = fxManager->property("quantizeEnabled2").toBool();
            ok &= revealAction(beatFxPanel, fxQuantize)
                && clickItem(performanceWindow, fxQuantize, "right-side FX Quantize is reachable and clickable");
            ok &= require(fxManager->property("quantizeEnabled1").toBool() != previousFxQuantize
                              && fxManager->property("syncEnabled1").toBool()
                              && fxManager->property("quantizeEnabled2").toBool() == otherFxQuantize
                              && fxQuantize && fxQuantize->property("active").toBool() != previousFxQuantize,
                          "right-side FX Quantize updates its unit and mirrors the canonical state");
            ok &= clickItem(performanceWindow, fxQuantize, "right-side FX Quantize switches back off");
            ok &= require(fxManager->property("quantizeEnabled1").toBool() == previousFxQuantize,
                          "right-side FX Quantize can restore its previous state");
            auto* engage = beatFxPanel->findChild<QQuickItem*>(QStringLiteral("fxEngageRow"));
            const QRectF engageBounds = engage->mapRectToItem(qobject_cast<QQuickItem*>(beatFxPanel), engage->boundingRect());
            ok &= require(engageBounds.bottom() >= height - 12,
                          "FX engage stays at the bottom while the central level display absorbs extra height");
            const bool previousEnabled = fxManager->property("enabled1").toBool();
            ok &= revealAction(beatFxPanel, engage)
                && clickItem(performanceWindow, engage, "scrolled FX engage is clickable");
            ok &= require(fxManager->property("enabled1").toBool() != previousEnabled,
                          "FX bottom action stays functional after resizing");
            if (width == 1280 && height == 800) {
                auto* dial = beatFxPanel->findChild<QQuickItem*>("fxLevelDial");
                auto* input = dial ? dial->findChild<QQuickItem*>("fxLevelDialInput") : nullptr;
                const bool savedEnabled = fxManager->property("enabled1").toBool();
                const double savedMix = fxManager->property("wetDry1").toDouble();
                fxManager->setProperty("enabled1", false);
                fxManager->setProperty("wetDry1", 0.3);
                QCoreApplication::processEvents();
                ok &= require(dial && dial->isVisible() && input
                                  && std::abs(dial->property("amount").toDouble() - 0.3) < 1e-6
                                  && !fxManager->property("enabled1").toBool(),
                              "the large level dial follows hardware state without engaging an effect");
                if (input) {
                    const QPoint center = input->mapToScene(input->boundingRect().center()).toPoint();
                    QTest::mousePress(&performanceWindow, Qt::LeftButton, Qt::NoModifier, center);
                    QTest::mouseMove(&performanceWindow, center - QPoint(0, 30));
                    QTest::mouseRelease(&performanceWindow, Qt::LeftButton, Qt::NoModifier, center - QPoint(0, 30));
                    QCoreApplication::processEvents();
                    ok &= require(fxManager->property("wetDry1").toDouble() > 0.4
                                      && fxManager->property("enabled1").toBool(),
                                  "dragging the large FX dial changes and engages the same effect unit");
                }
                fxManager->setProperty("wetDry1", savedMix);
                fxManager->setProperty("enabled1", savedEnabled);
            }
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

    for (const QSize viewport : {QSize(1280, 800), QSize(1920, 720), QSize(2560, 1080), QSize(1024, 600)}) {
        performanceWindow.resize(viewport);
        performanceItem->setSize(viewport);
        performance->setProperty("leftPanel", "deck");
        performance->setProperty("rightPanelOpen", true);
        QTest::qWait(220);
        auto* waveformArea = performance->findChild<QQuickItem*>("waveformBacking");
        const bool docked = performance->property("dockPanels").toBool();
        ok &= require(docked == (viewport.width() >= 1600 && viewport.height() >= 700),
                      "wide panels dock and compact panels return to overlay on live resize");
        ok &= require(waveformArea && waveformArea->width() >= (docked ? 900 : viewport.width()) - 1,
                      "docked panels preserve the waveform reading area");
        const QString captureDir = qEnvironmentVariable("BROCKDJ_UI_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            QDir().mkpath(captureDir);
            QTest::qWait(50);
            ok &= require(performanceWindow.grabWindow().save(captureDir + QString("/performance-%1x%2.png")
                .arg(viewport.width()).arg(viewport.height())), "performance reference capture saves");
        }
    }

    performance->setProperty("leftPanel", "deck");
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
        ok &= require(!backdrop->property("frameVisible").toBool(),
                      "sidebar backdrops do not duplicate the foreground panel frame");
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
                          && crop.width() <= performance->property("panelWidth").toDouble() + 0.01 && crop.height() <= backing->height()
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
        }), "full-height sidebar captures settle to the bounded target size");
        const QSize tallTexture = backdrop->property("captureSize").toSize();
        if (!capture || !intermediate) {
            ok &= require(false, "visible backdrop restores bounded capture objects after resizing");
            performanceItem->setSize(QSizeF(900, 600));
            continue;
        }
        ok &= require(tallTexture.width() <= 128 && tallTexture.height() == 256
                          && capture->property("textureSize").toSize() == tallTexture
                          && intermediate->property("textureSize").toSize() == tallTexture,
                      "full-height sidebar render targets never exceed their hard GPU cap");
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

    {
        QQuickWindow infoWindow;
        infoWindow.resize(1280, 200);
        infoWindow.setColor(QColor("#10151d"));
        auto infoEngine = createQmlObject(engine, QStringLiteral(R"(
            import QtQml
            QtObject {
                property bool hasTrack: true
                property bool isPlaying: false
                property bool loopActive: false
                property real visualPosition: 49.123
                property string trackTitle: "After Hours / Extended Mix"
                property string trackArtist: "Northbound"
                property string trackKey: "Am"
                property string trackNumber: "07"
                property real trackDurationSec: 248
                property real tempoRatio: 1.04
                property real tempoPercent: 4
                property real tempoRangePercent: 8
                property real currentBpm: 128.4
                property real progress: 0.2
                property bool syncMaster: true
                property bool quantizeEnabled: true
                property bool readOnlyExternalTrack: false
                property real externalCacheProgress: 0
                property bool externalCacheReady: false
                property bool seekPreviewActive: false
                property real seekPreviewPosition: 0
                property real mainCueSec: 0
                function getVisualPositionQml() { return visualPosition }
                function getPlayheadPositionAtomic() { return 49.123 }
                function beginSeekPreview(value) {}
                function updateSeekPreview(value) {}
                function commitSeekPreview() {}
                function cancelSeekPreview() {}
            }
        )"), "lower deck display fixture instantiates");
        QQmlComponent infoComponent(&engine, QUrl::fromLocalFile(
            qmlDirectory + QStringLiteral("deck/DeckTrackInfoPanel.qml")));
        std::unique_ptr<QObject> infoA(infoComponent.create(&context));
        std::unique_ptr<QObject> infoB(infoComponent.create(&context));
        if (!require(infoA && infoB, "both lower deck information panels instantiate")) {
            std::cerr << infoComponent.errorString().toStdString();
            return false;
        }
        infoA->setProperty("engine", QVariant::fromValue(infoEngine.get()));
        infoB->setProperty("engine", QVariant::fromValue(infoEngine.get()));
        infoB->setProperty("deckName", QStringLiteral("B"));
        const auto fit = [&](int viewportWidth) {
            infoWindow.resize(viewportWidth, 200);
            for (auto* object : {infoA.get(), infoB.get()}) {
                auto* item = qobject_cast<QQuickItem*>(object);
                item->setParentItem(infoWindow.contentItem());
                item->setSize(QSizeF(viewportWidth / 2, 200));
                item->setX(object == infoA.get() ? 0 : viewportWidth / 2);
            }
            QCoreApplication::processEvents();
        };
        fit(1024);
        const double initialMetric = infoA->property("metricScale").toDouble();
        auto* clockDigits = infoA->findChild<QQuickItem*>("deckClockDigits");
        const qreal clockWidth = clockDigits ? clockDigits->width() : 0;
        for (const int viewportWidth : {1024, 1280, 1440, 1920, 2560}) {
            fit(viewportWidth);
            ok &= require(std::abs(infoA->property("metricScale").toDouble() - initialMetric) < 1e-6
                              && infoA->property("titleHeight") == infoB->property("titleHeight")
                              && qobject_cast<QQuickItem*>(infoA.get())->scale() == 1.0
                              && clockDigits && std::abs(clockDigits->width() - clockWidth) < 0.01,
                          "widening the lower deck row preserves control metrics and native item scale");
        }

        infoEngine->setProperty("isPlaying", true);
        infoEngine->setProperty("visualPosition", 240.0);
        QMetaObject::invokeMethod(infoA.get(), "refreshDisplayedPlaybackPosition");
        QCoreApplication::processEvents();
        ok &= require(infoA->property("trackEnding").toBool(), "playing deck warns near its real-time track end");
        infoEngine->setProperty("loopActive", true);
        QCoreApplication::processEvents();
        ok &= require(!infoA->property("trackEnding").toBool(), "an active loop suppresses track-end warnings");
        infoEngine->setProperty("loopActive", false);
        infoEngine->setProperty("isPlaying", false);
        QCoreApplication::processEvents();
        ok &= require(!infoA->property("trackEnding").toBool(), "a paused deck does not show an end warning");
        infoEngine->setProperty("visualPosition", 49.123);
        QMetaObject::invokeMethod(infoA.get(), "refreshDisplayedPlaybackPosition");
        const QString captureDir = qEnvironmentVariable("BROCKDJ_UI_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            QDir().mkpath(captureDir);
            fit(1280);
            infoWindow.show();
            QTest::qWait(50);
            ok &= require(infoWindow.grabWindow().save(captureDir + "/deck-information.png"),
                          "deck information reference capture saves");
        }

    }

    {
        QQuickWindow sourceWindow;
        sourceWindow.resize(1280, 600);
        sourceWindow.setColor(QColor("#10151d"));
        QQmlComponent sourceComponent(&engine, QUrl::fromLocalFile(
            qmlDirectory + QStringLiteral("library/SourcePage.qml")));
        std::unique_ptr<QObject> source(sourceComponent.createWithInitialProperties(
            {{QStringLiteral("appWindow"), QVariant::fromValue(windowContext.get())}}, &context));
        if (!require(source != nullptr, "source selector with shared panel styling instantiates")) {
            std::cerr << sourceComponent.errorString().toStdString();
            return false;
        }
        auto* sourceItem = qobject_cast<QQuickItem*>(source.get());
        sourceItem->setParentItem(sourceWindow.contentItem());
        sourceItem->setSize(QSizeF(1280, 600));
        sourceWindow.show();
        QTest::qWait(40);
        QSignalSpy libraryOpens(source.get(), SIGNAL(libraryRequested()));
        ok &= clickItem(sourceWindow, source->findChild<QQuickItem*>("localSourceRow"),
                        "local source can be selected with the pointer");
        ok &= clickItem(sourceWindow, source->findChild<QQuickItem*>("sourceLibraryRow"),
                        "selected source library is directly reachable");
        ok &= require(libraryOpens.count() == 1, "the source selector opens the requested local library");
        QQmlExpression devices(&context, deviceLibrary.get(), QStringLiteral(
            "devices = [{id:'usb-test', name:'DJ USB', ready:true, trackCount:1248, "
            "canMount:false, canEject:true, operationPending:false, scanning:false, actionLabel:'Eject'}]"));
        devices.evaluate();
        QCoreApplication::processEvents();
        QSignalSpy usbOpens(source.get(), SIGNAL(usbLibraryRequested()));
        // BACK restores the media pane before moving from Local to USB.
        QMetaObject::invokeMethod(source.get(), "goBack");
        QMetaObject::invokeMethod(source.get(), "moveCursor", Q_ARG(QVariant, QVariant(1)));
        QMetaObject::invokeMethod(source.get(), "activateCursor");
        QMetaObject::invokeMethod(source.get(), "activateCursor");
        ok &= require(usbOpens.count() == 1
                          && deviceLibrary->property("selectedDeviceId").toString() == "usb-test",
                      "controller source navigation still opens the selected USB library");

        const QString captureDir = qEnvironmentVariable("BROCKDJ_UI_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            QTest::qWait(50);
            ok &= require(sourceWindow.grabWindow().save(captureDir + "/sources.png"),
                          "source reference capture saves");
        }
        deviceLibrary->setProperty("devices", QVariantList{});
    }

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
    fxManager->setProperty("soundColorMode", QStringLiteral("None"));
    auto* adjustControl = findVisualItem(fxBarItem, QStringLiteral("soundColorAdjustControl"));
    ok &= require(adjustControl && adjustControl->isVisible(), "Sound Color Adjust starts visible");
    fxManager->setProperty("soundColorParam", 0.83);
    fxManager->setProperty("soundColorAdjustEnabled", false);
    QCoreApplication::processEvents();
    ok &= require(adjustControl && !adjustControl->isVisible() && !adjustControl->isEnabled(),
                  "disabling Sound Color Adjust hides and disables its control");
    fxManager->setProperty("soundColorAdjustEnabled", true);
    QCoreApplication::processEvents();
    ok &= require(adjustControl && adjustControl->isVisible()
                      && std::abs(fxManager->property("soundColorParam").toDouble() - 0.83) < 1e-6,
                  "re-enabling Sound Color Adjust preserves its manual setting");
    auto* filterMode = findVisualItem(fxBarItem, QStringLiteral("soundColorMode_Filter"));
    auto* noiseMode = findVisualItem(fxBarItem, QStringLiteral("soundColorMode_Noise"));
    ok &= require(filterMode && !filterMode->property("isActive").toBool()
                      && noiseMode && !noiseMode->property("isActive").toBool(),
                  "Sound Color can have no active selection");
    ok &= clickItem(fxWindow, filterMode, "Sound Color Filter button is clickable");
    ok &= require(fxManager->property("soundColorMode").toString() == QStringLiteral("Filter"),
                  "Sound Color UI selects the requested mode");
    ok &= clickItem(fxWindow, noiseMode, "Sound Color Noise button is clickable");
    ok &= require(fxManager->property("soundColorMode").toString() == QStringLiteral("Noise")
                      && filterMode && !filterMode->property("isActive").toBool(),
                  "Sound Color UI replaces the previous selection");
    ok &= clickItem(fxWindow, noiseMode, "Sound Color active mode can be clicked again");
    ok &= require(fxManager->property("soundColorMode").toString() == QStringLiteral("None"),
                  "Sound Color UI deselects the active mode");
    QObject* unit1 = fxBar->findChild<QObject*>(QStringLiteral("fxUnit1"));
    QObject* unit2 = fxBar->findChild<QObject*>(QStringLiteral("fxUnit2"));
    auto* quantize1 = unit1 ? unit1->findChild<QQuickItem*>("fxQuantizeButton") : nullptr;
    auto* quantize2 = unit2 ? unit2->findChild<QQuickItem*>("fxQuantizeButton") : nullptr;
    auto* effectSelector = unit1 ? unit1->findChild<QObject*>("fxEffectCombo") : nullptr;
    if (effectSelector) effectSelector->setProperty("currentIndex", 1); // Echo
    QCoreApplication::processEvents();
    QTest::qWait(30); // Let the row layout place the newly visible timing controls before clicking.
    ok &= clickItem(fxWindow, quantize1, "FX Quantize is clickable for a rhythmic effect");
    ok &= require(fxManager->property("quantizeEnabled1").toBool()
                      && fxManager->property("syncEnabled1").toBool()
                      && !fxManager->property("quantizeEnabled2").toBool()
                      && quantize1 && quantize1->property("checked").toBool(),
                  "FX Quantize follows manager state and affects only the selected unit");
    ok &= clickItem(fxWindow, quantize1, "FX Quantize can be disabled again");
    ok &= require(!fxManager->property("quantizeEnabled1").toBool()
                      && fxManager->property("syncEnabled1").toBool(),
                  "disabling quantize retains tempo sync");
    if (effectSelector) effectSelector->setProperty("currentIndex", 0);
    QCoreApplication::processEvents();
    ok &= require(quantize1 && quantize1->isVisible() && quantize1->isEnabled()
                      && quantize2 && quantize2->isVisible()
                      && quantize1->opacity() < 1.0,
                  "FX Quantize stays visible and interactive with reduced emphasis when irrelevant");
    ok &= clickItem(fxWindow, quantize1, "irrelevant FX Quantize remains switchable");
    ok &= require(fxManager->property("quantizeEnabled1").toBool(),
                  "FX Quantize can be armed before choosing a timing effect");
    fxManager->setProperty("effectType1", QStringLiteral("Echo"));
    QCoreApplication::processEvents();
    ok &= require(quantize1 && quantize1->opacity() == 1.0,
                  "controller-driven effect changes update timing relevance in the FX strip");
    fxManager->setProperty("effectType1", QStringLiteral("---"));

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

bool designSystemRuntimeTests()
{
    QQmlEngine engine;
    auto host = createQmlObject(engine, QStringLiteral(R"(
        import QtQuick
        import DJSoftware
        QtObject {
            function profile(w, h, factor) { return UiTheme.layoutProfile(w, h, factor) }
            function labelSize(h, factor) { return UiTheme.fontSize(10, h, factor) }
            readonly property color background: UiTheme.surfaceRaised
            readonly property var foregrounds: [UiTheme.textPrimary, UiTheme.textSecondary,
                                                UiTheme.textLabel, UiTheme.textDim]
        }
    )"), "design-system production helpers instantiate");
    if (!host) return false;
    bool ok = true;
    for (const auto size : {QSize(800, 600), QSize(1024, 600), QSize(1280, 720),
             QSize(1280, 800), QSize(1920, 720), QSize(1920, 1080),
             QSize(2560, 1080), QSize(2560, 1440), QSize(3840, 2160)}) {
        for (double factor : {0.8, 1.0, 1.4}) {
            QVariant value;
            QMetaObject::invokeMethod(host.get(), "profile", Q_RETURN_ARG(QVariant, value),
                Q_ARG(QVariant, size.width()), Q_ARG(QVariant, size.height()), Q_ARG(QVariant, factor));
            const QString expected = size.width() / factor < 1100 || size.height() / factor < 700
                ? "compact" : size.width() / factor >= 1600 ? "wide" : "standard";
            ok &= require(value.toString() == expected, "display profiles account for width, height and user scale independently");
            QMetaObject::invokeMethod(host.get(), "labelSize", Q_RETURN_ARG(QVariant, value),
                Q_ARG(QVariant, size.height()), Q_ARG(QVariant, factor));
            ok &= require(value.toDouble() >= 11 * factor,
                          "labels retain their readability floor at every display profile");
        }
    }
    for (const auto& boundary : {QSize(1099, 800), QSize(1100, 800), QSize(1599, 800),
                                QSize(1600, 800), QSize(1920, 699), QSize(1920, 700)}) {
        QVariant value;
        QMetaObject::invokeMethod(host.get(), "profile", Q_RETURN_ARG(QVariant, value),
            Q_ARG(QVariant, boundary.width()), Q_ARG(QVariant, boundary.height()), Q_ARG(QVariant, 1.0));
        const QString expected = boundary.width() < 1100 || boundary.height() < 700
            ? "compact" : boundary.width() >= 1600 ? "wide" : "standard";
        ok &= require(value.toString() == expected, "profile boundaries transition without gaps");
    }
    const auto luminance = [](const QColor& color) {
        const auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
    };
    const double background = luminance(host->property("background").value<QColor>());
    for (const auto& foreground : host->property("foregrounds").toList())
        ok &= require((luminance(foreground.value<QColor>()) + 0.05) / (background + 0.05) >= 4.5,
                      "enabled text tokens meet 4.5 to 1 contrast on the raised working surface");
    return ok;
}

bool squareControlsRuntimeTests()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(220, 100);
    window.setColor(QColor("#ff00ff"));
    const QString directory = QString::fromUtf8(BROCKDJ_SOURCE_DIR)
        + QStringLiteral("/src/qml/components/");
    bool ok = true;
    for (const QString& name : {QStringLiteral("Button.qml"), QStringLiteral("ComboBox.qml")}) {
        QQmlComponent component(&engine, QUrl::fromLocalFile(directory + name));
        std::unique_ptr<QObject> object(component.create());
        if (!require(object != nullptr, "production square control instantiates")) {
            std::cerr << component.errorString().toStdString();
            return false;
        }
        auto* control = qobject_cast<QQuickItem*>(object.get());
        if (!require(control != nullptr, "square control is visual")) return false;
        control->setParentItem(window.contentItem());
        control->setPosition(QPointF(20, 20));
        control->setSize(QSizeF(180, 44));
        if (name == "Button.qml") object->setProperty("text", QStringLiteral("LOAD A"));
        window.show();
        for (bool active : {false, true}) {
            if (name == "Button.qml") object->setProperty("checked", active);
            // Focus remains a distinct square outline; neither state rounds corners.
            control->forceActiveFocus(active ? Qt::TabFocusReason : Qt::OtherFocusReason);
            QTest::qWait(40);
            const QImage frame = window.grabWindow();
            if (!require(!frame.isNull(), "square control renders into a real framebuffer")) return false;
            const qreal dpr = frame.devicePixelRatio();
            const QColor corner = frame.pixelColor(qRound(20 * dpr), qRound(20 * dpr));
            const QColor edge = frame.pixelColor(qRound(100 * dpr), qRound(20 * dpr));
            ok &= require(corner == edge && corner != window.color(),
                          "button and selector frames keep filled square corners in normal and keyboard-focus states");
        }
        if (name == "Button.qml") {
            object->setProperty("checked", false);
            control->setFocus(false);
            QQmlExpression border(QQmlEngine::contextForObject(object.get()), object.get(),
                QStringLiteral("background.border.color === UiTheme.borderSubtle"));
            ok &= require(border.evaluate().toBool() && !border.hasError(),
                          "inactive buttons retain a subtle outline instead of the bright strong border");
        }
    }
    return ok;
}

bool mainNavigationRuntimeTests()
{
    const QString directory = QString::fromUtf8(BROCKDJ_SOURCE_DIR) + "/src/qml/";
    for (const auto& type : {std::pair{"settings/SettingsPanel.qml","SettingsPanel"},
                             std::pair{"settings/MappingEditorWindow.qml","MappingEditorWindow"},
                             std::pair{"shell/TopHeader.qml","TopHeader"}})
        qmlRegisterType(QUrl::fromLocalFile(directory + type.first),"DJSoftware",1,0,type.second);
    QQmlEngine engine;
    for (const char* name : {"deckA","deckB","deckC","deckD","settingsManager","mixerControl","linkManager","parameterStore"})
        engine.rootContext()->setContextProperty(name,static_cast<QObject*>(nullptr));
    auto scale = createQmlObject(engine,"import QtQml; QtObject { property real scale: 1 }","navigation scale fixture instantiates");
    engine.rootContext()->setContextProperty("uiScaleController",scale.get());
    auto host = createQmlObject(engine,QStringLiteral(R"(
        import QtQuick
        import QtQuick.Controls
        import DJSoftware
        ApplicationWindow {
            width: 1280; height: 150; visible: true
            color: UiTheme.panel
            property string activeMainTab: "performance"
            property bool allInOneMode: true
            property bool showLibrary: true
            property real topBarPullProgress: 0
            property bool topBarPullDragging: false
            readonly property bool sourcePageActive: activeMainTab === "source"
            readonly property bool samplerPageActive: activeMainTab === "sampler"
            readonly property bool fxPageActive: activeMainTab === "fx"
            readonly property bool utilityPageActive: samplerPageActive || fxPageActive
            readonly property bool libraryPanelActive: activeMainTab === "library"
            readonly property bool settingsPanelActive: activeMainTab === "settings"
            function closeTopBarPullDown() { topBarPullProgress = 0 }
            function toggleTopBarPullDown() { topBarPullProgress = 1 - topBarPullProgress }
            function openTopBarPullDown() { topBarPullProgress = 1 }
            function openMainPage(tab) { activeMainTab = tab }
            function toggleAllInOneLibrary() { activeMainTab = "library" }
            function toggleAllInOneSettings() { activeMainTab = "settings"; return true }
            function toggleLibrarySourceView() { activeMainTab = "source" }
            TopHeader { objectName: "testTopHeader"; width: parent.width; height: UiTheme.toolbarHeight }
        }
    )"),"complete production header instantiates");
    if (!host) return false;
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    bool ok = true;
    for (const bool aio : {true,false}) for (const int width : {800,1280,1920}) for (const double factor : {1.0,1.4}) {
        host->setProperty("allInOneMode",aio); scale->setProperty("scale",factor); window->resize(width,150);
        QCoreApplication::processEvents();
        for (const char* name : {"performanceNavigation","libraryNavigation","samplerNavigation","effectsNavigation","settingsNavigation"}) {
            auto* button = findVisualItem(window->contentItem(),QString::fromUtf8(name));
            const auto bounds = button ? button->mapRectToScene(button->boundingRect()) : QRectF();
            ok &= require(button && button->isVisible() && bounds.left() >= -1 && bounds.right() <= width + 1 && bounds.width() >= 40,
                "all five top navigation tabs remain visible within desktop/AIO viewports at each UI scale");
        }
        auto* sampler = findVisualItem(window->contentItem(),"samplerNavigation");
        auto* effects = findVisualItem(window->contentItem(),"effectsNavigation");
        auto* library = findVisualItem(window->contentItem(),"libraryNavigation");
        ok &= clickItem(*window,sampler,"sampler has a clickable top-level navigation tab");
        ok &= require(host->property("activeMainTab") == "sampler" && sampler->property("checked").toBool()
            && !library->property("checked").toBool(),"sampler page selection clears the Library tab highlight in both modes");
        ok &= clickItem(*window,effects,"FX has a clickable top-level navigation tab");
        ok &= require(host->property("activeMainTab") == "fx" && effects->property("checked").toBool()
            && !sampler->property("checked").toBool(),"FX and Sampler tabs track their own main-page state");
        ok &= clickItem(*window,findVisualItem(window->contentItem(),"performanceNavigation"),"Live remains reachable from either new page");
        if (aio && width == 1280 && factor == 1 && !qEnvironmentVariableIsEmpty("BROCKDJ_TEST_CAPTURE_DIR")) {
            QTest::qWait(40);
            window->grabWindow().save(QDir(qEnvironmentVariable("BROCKDJ_TEST_CAPTURE_DIR")).filePath("header-navigation.png"));
        }
    }
    return ok;
}

bool effectsPageRuntimeTests(const std::string& main)
{
    QQmlEngine engine;
    auto manager = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property string effectType1: "Echo"
            property string effectType2: "Reverb"
            property bool enabled1: false
            property bool enabled2: true
            property real wetDry1: 0.2
            property real wetDry2: 0.3
            property real primaryParam1: 0.5
            property real primaryParam2: 0.7
            property real beatDiv1: 0.5
            property real beatDiv2: 1
            property real displayBpm1: 128
            property real displayBpm2: 128
            property bool syncEnabled1: false
            property bool syncEnabled2: true
            property bool quantizeEnabled1: false
            property bool quantizeEnabled2: true
            property bool deck1A: true
            property bool deck1B: false
            property bool deck1C: false
            property bool deck1D: false
            property bool deck2A: false
            property bool deck2B: true
            property bool deck2C: false
            property bool deck2D: false
            property string soundColorMode: "Filter"
            property bool soundColorAdjustEnabled: false
            property real soundColorParam: 0.5
            function setWetDry(unit, value) { this["wetDry" + unit] = value }
            function setPrimaryParam(unit, value) { this["primaryParam" + unit] = value }
            function setBeatDivision(unit, value) { this["beatDiv" + unit] = value }
            function setSyncEnabled(unit, value) { this["syncEnabled" + unit] = value }
            function setQuantizeEnabled(unit, value) { this["quantizeEnabled" + unit] = value }
            function setUnitEnabled(unit, value) { this["enabled" + unit] = value }
            function setDeckAssignment(unit, deck, value) { this["deck" + unit + "ABCD"[deck-1]] = value }
            function setEffectType(unit, value) { this["effectType" + unit] = value }
            function setSoundColorMode(value) { soundColorMode = value }
        }
    )"), "FX page control fixture instantiates");
    if (!manager) return false;
    engine.rootContext()->setContextProperty("fxManager",manager.get());
    const QString directory = QString::fromUtf8(BROCKDJ_SOURCE_DIR) + "/src/qml/";
    QQmlComponent component(&engine,QUrl::fromLocalFile(directory + "performance/EffectsPage.qml"));
    std::unique_ptr<QObject> page(component.create());
    if (!require(page != nullptr,"full effects page instantiates")) { std::cerr << component.errorString().toStdString(); return false; }
    auto* item = qobject_cast<QQuickItem*>(page.get());
    QQuickWindow window; window.resize(1280,800); item->setParentItem(window.contentItem()); item->setSize(QSizeF(1280,740));
    window.show(); QCoreApplication::processEvents();
    auto* pad = findVisualItem(item,"touchFxPad");
    bool ok = require(pad && pad->width() > 500 && pad->height() >= 220,"Touch FX provides a real full-page XY surface");
    if (!pad) return false;
    const QPoint point = pad->mapToScene(QPointF(pad->width()*0.7,pad->height()*0.25)).toPoint();
    QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,point);
    ok &= require(page->property("touching").toBool() && manager->property("enabled1").toBool()
        && std::abs(manager->property("wetDry1").toDouble()-0.7) < 0.02 && manager->property("syncEnabled1").toBool(),
        "Touch FX enables the selected unit and maps horizontal movement to wet mix");
    QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,point);
    ok &= require(!page->property("touching").toBool() && !manager->property("enabled1").toBool()
        && std::abs(manager->property("wetDry1").toDouble()-0.2) < 0.001
        && !manager->property("syncEnabled1").toBool() && manager->property("beatDiv1").toDouble() == 0.5,
        "Touch FX release restores mix, beat timing, sync and prior on/off state");
    page->setProperty("selectedUnit",2);
    QMetaObject::invokeMethod(page.get(),"beginTouch",Q_ARG(QVariant,0.8),Q_ARG(QVariant,0.4));
    ok &= require(manager->property("primaryParam2").toDouble() == 0.4,"Reverb Touch FX maps its second axis to room size");
    item->setVisible(false);
    ok &= require(!page->property("touching").toBool() && manager->property("enabled2").toBool()
        && manager->property("primaryParam2").toDouble() == 0.7 && manager->property("wetDry2").toDouble() == 0.3,
        "leaving the FX page restores a pre-existing active unit instead of leaving touch latched");
    item->setVisible(true); page->setProperty("section",1); QCoreApplication::processEvents();
    ok &= clickItem(window,findVisualItem(item,"fxPageDeck2"),"FX settings expose independent C/D routing");
    ok &= require(manager->property("deck2C").toBool() && manager->property("deck2B").toBool(),"FX page preserves existing routing while adding another deck");
    page->setProperty("section",0); manager->setProperty("deck2B",false); manager->setProperty("deck2C",false);
    QMetaObject::invokeMethod(page.get(),"beginTouch",Q_ARG(QVariant,0.8),Q_ARG(QVariant,0.4));
    ok &= require(!page->property("touching").toBool(),"unrouted Touch FX does not silently choose a deck");
    if (!qEnvironmentVariableIsEmpty("BROCKDJ_TEST_CAPTURE_DIR")) {
        manager->setProperty("deck2B",true); QTest::qWait(40);
        window.grabWindow().save(QDir(qEnvironmentVariable("BROCKDJ_TEST_CAPTURE_DIR")).filePath("effects-page.png"));
    }
    QString methods;
    if (!extractFunctions(main,{"openMainPage"},methods)) return false;
    auto navigation = createQmlObject(engine,QStringLiteral("import QtQml; QtObject { property string activeMainTab: 'performance'; property bool libraryExpanded: true; function closeTopBarPullDown() {}\n")
        + methods + "\n}","shared main-page navigation instantiates");
    if (!navigation) return false;
    for (const QString name : {QStringLiteral("sampler"),QStringLiteral("fx"),QStringLiteral("performance")}) {
        QMetaObject::invokeMethod(navigation.get(),"openMainPage",Q_ARG(QVariant,name));
        ok &= require(navigation->property("activeMainTab").toString() == name && !navigation->property("libraryExpanded").toBool(),
            "Sampler and FX navigation use full main pages and clear the library expansion");
    }
    return ok;
}

bool samplerAndRecordingRuntimeTests(const std::string& header)
{
    const QString directory = QString::fromUtf8(BROCKDJ_SOURCE_DIR) + "/src/qml/";
    for (const QString& name : {QStringLiteral("Button"), QStringLiteral("Slider"), QStringLiteral("ComboBox")})
        qmlRegisterType(QUrl::fromLocalFile(directory + "components/" + name + ".qml"), "DJSoftware", 1, 0, name.toUtf8());
    qmlRegisterType(QUrl::fromLocalFile(directory + "performance/SamplerEditor.qml"), "DJSoftware", 1, 0, "SamplerEditor");
    QQmlEngine engine;
    auto recorder = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property bool recording: false
            property bool stopping: false
            property real elapsedSeconds: 0
            property string error: ""
            property int toggles: 0
            function toggleRecording() { toggles++; recording = !recording }
        }
    )"), "recorder UI fixture instantiates");
    if (!recorder) return false;
    engine.rootContext()->setContextProperty("setRecorder", recorder.get());
    const auto begin = header.find("            Rectangle {\n                id: quickRecording");
    const auto end = header.find("            Rectangle {\n                objectName: \"quickSampler\"", begin);
    if (!require(begin != std::string::npos && end != std::string::npos, "recording quick access exists")) return false;
    auto host = createQmlObject(engine, QStringLiteral("import QtQuick; import QtQuick.Controls; import QtQuick.Layouts; import DJSoftware; Item { id: root; width: 250; height: 80; function sp(n) { return n }\n")
        + QString::fromStdString(header.substr(begin, end-begin)) + "\n}", "real recording quick-access field instantiates");
    if (!host) return false;
    QQuickWindow window; window.resize(800, 600);
    auto* item = qobject_cast<QQuickItem*>(host.get()); item->setParentItem(window.contentItem());
    auto* button = host->findChild<QQuickItem*>("quickRecording");
    button->setSize(QSizeF(250,80));
    window.show(); QCoreApplication::processEvents();
    bool ok = clickItem(window, button, "recording starts from the real quick-access field");
    ok &= require(recorder->property("recording").toBool() && recorder->property("toggles").toInt() == 1, "recording quick access uses common toggle");
    recorder->setProperty("elapsedSeconds", 3661.0); QCoreApplication::processEvents();
    auto* duration = host->findChild<QObject*>("quickRecordingDuration");
    ok &= require(duration && duration->property("text") == "01:01:01", "recording field displays hours/minutes/seconds");
    recorder->setProperty("stopping", true); QCoreApplication::processEvents();
    ok &= clickItem(window, button, "saving recording field remains visible");
    ok &= require(recorder->property("toggles").toInt() == 1, "saving state prevents repeated recording toggles");
    item->setVisible(false);

    auto controller = createQmlObject(engine, QStringLiteral(R"(
        import QtQml
        QtObject {
            property int bank: 0
            property int selectedSlot: 0
            property real gain: 0.72
            property bool cue: false
            property int quantize: 1
            property bool sequencerEnabled: false
            property int sequencerStep: 0
            property real sequencerSwing: 0
            property var sequencerPattern: [0,0,0,0,0,0,0,0]
            property string clockSource: "INTERNAL"
            property real bpm: 120
            property string error: ""
            property var fileInfo: ({})
            property int triggers: 0
            property int stops: 0
            property int padStops: 0
            property int editsSaved: 0
            property int editSlot: -1
            property int lastSavedSlot: -1
            property int nextSession: 0
            property bool midiPickupPending: false
            property bool previewAvailable: true
            property bool previewPlaying: false
            property bool previewLoading: false
            property bool editorBusy: false
            property bool failSave: false
            property string editorError: ""
            property var previewPlayback: ({position:0, progress:0})
            property var playback: [{position:0.5,duration:2,progress:0.25,remainingBeats:1.5}]
            property var bankActivity: [{playing:1,queued:0},{playing:0,queued:1},{playing:0,queued:0},{playing:0,queued:0}]
            signal editorChanged()
            signal editorSourceReady(double session, var info)
            signal editorWaveformReady(double session, double start, double end, var peaks)
            signal editorFinished(double session, bool success, string error)
            property var slots: [{name:"Airhorn", color:"#e0a344", loaded:true, state:"READY", duration:2, start:0, end:2, gain:1, waveform:[0.2,0.5,0.8]}, {}, {}, {}, {}, {}, {}, {}]
            function trigger(pad) { triggers++ }
            function setSequencerStep(pad,step,enabled) {
                const next=sequencerPattern.slice()
                next[pad] = enabled ? (next[pad] | (1 << step)) : (next[pad] & ~(1 << step))
                sequencerPattern=next
            }
            function clearSequencerPattern() { sequencerPattern=[0,0,0,0,0,0,0,0] }
            function stopAll() { stops++ }
            function stop(pad) { padStops++ }
            function playFixture() { const next=slots.slice(); next[0]=Object.assign({},slots[0],{state:"PLAYING",transportState:"PLAYING"}); slots=next }
            function queueFixture() { const next=slots.slice(); next[0]=Object.assign({},slots[0],{state:"QUEUED",transportState:"QUEUED"}); slots=next }
            function beginEditor(pad) {
                editSlot = bank*8 + pad; editorError = ""
                return {session:++nextSession, slot:editSlot, source:"stored.wav", duration:2, sampleRate:48000,
                    name:"Airhorn", color:"#e0a344", gain:1, loop:false, sync:false, bpm:120,
                    start:0, end:2, waveform:[0.2,0.5,0.8], loaded:true}
            }
            function cancelEditor(session) { previewPlaying = false }
            function stopEditorPreview() { previewPlaying = false; previewLoading = false }
            function requestEditorWaveform(session, start, end) { const peaks=[]; for(let i=0;i<1024;i++) peaks.push(0.12+0.7*Math.abs(Math.sin(i*0.071))*Math.exp(-(i%128)/100)); editorWaveformReady(session,start,end,peaks) }
            function setEditorFile(session,path) { editorSourceReady(session,{source:path,duration:2,sampleRate:48000,name:"Imported",waveform:[0.2,0.5,0.8]}) }
            function captureEditor(session,deck,beats) { editorSourceReady(session,{source:"deck.wav",duration:2,sampleRate:48000,name:"Capture",waveform:[0.4],start:0,end:1,bpm:120,loop:true,sync:true}) }
            function previewEditor(session,draft) { previewPlaying = true }
            function applyEditor(session,draft) {
                if (failSave) { editorError = "Storage full"; editorFinished(session,false,editorError); return }
                editsSaved++; lastSavedSlot = editSlot; previewPlaying = false; editorFinished(session,true,"")
            }
            function clearEditor(session) { editorFinished(session,true,"") }
            function probeFile(path) {}
            function captureDeck(pad, deck, beats) {}
            function clear(pad) {}
            function edit() {}
            function loadFile() {}
        }
    )"), "sampler UI fixture instantiates");
    if (!controller) return false;
    engine.rootContext()->setContextProperty("sampler", controller.get());
    QQmlComponent component(&engine, QUrl::fromLocalFile(directory + "performance/SamplerPanel.qml"));
    std::unique_ptr<QObject> panel(component.create());
    if (!require(panel != nullptr, "full sampler panel instantiates")) { std::cerr << component.errorString().toStdString(); return false; }
    auto* panelItem = qobject_cast<QQuickItem*>(panel.get()); panelItem->setParentItem(window.contentItem());
    for (const QSize size : {QSize(800,600), QSize(1280,800), QSize(1920,1080)}) {
        window.resize(size); panelItem->setSize(QSizeF(size.width(), 244)); QCoreApplication::processEvents();
        auto* bank = findVisualItem(panelItem, "samplerBank2");
        ok &= clickItem(window, bank, "sampler bank button fits viewport and responds");
        ok &= require(controller->property("bank").toInt() == 2, "sampler bank selector updates common controller");
        auto* pad = findVisualItem(panelItem, "samplerPad0");
        const int previous = controller->property("triggers").toInt();
        // The edit affordance occupies the right edge; play uses the left half.
        const QPoint point = pad->mapToScene(QPointF(pad->width()/3, pad->height()/2)).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
        ok &= require(controller->property("triggers").toInt() == previous + 1, "sampler pad triggers with no deck engine");
        ok &= require(pad->property("radius").toDouble() == 0 && pad->height() >= 44, "sampler pads retain square touch targets");
        if (size.width() == 1280 && !qEnvironmentVariableIsEmpty("BROCKDJ_TEST_CAPTURE_DIR")) {
            QTest::qWait(40);
            window.grabWindow().save(QDir(qEnvironmentVariable("BROCKDJ_TEST_CAPTURE_DIR")).filePath("sampler-panel.png"));
        }
    }
    ok &= clickItem(window, findVisualItem(panelItem, "samplerStopAll"), "sampler stop-all is directly reachable");
    ok &= require(controller->property("stops").toInt() == 1, "sampler stop-all reaches controller");
    panel->setProperty("pageMode",true);
    for (const QSize size : {QSize(800,600),QSize(1280,800),QSize(1920,1080)}) {
        window.resize(size); panelItem->setSize(QSizeF(size.width(),size.height()-48)); QCoreApplication::processEvents();
        auto* pad = findVisualItem(panelItem,"samplerPad0");
        ok &= require(pad && pad->height() >= 100 && pad->mapRectToScene(pad->boundingRect()).right() <= size.width(),
            "dedicated sampler page expands pads and fits its full workspace");
        if (size.width() == 1280 && !qEnvironmentVariableIsEmpty("BROCKDJ_TEST_CAPTURE_DIR")) {
            QTest::qWait(40);
            window.grabWindow().save(QDir(qEnvironmentVariable("BROCKDJ_TEST_CAPTURE_DIR")).filePath("sampler-page.png"));
        }
    }
    ok &= clickItem(window,findVisualItem(panelItem,"samplerSequencerView"),"sampler opens its dedicated step-sequencer view");
    for (const QSize size : {QSize(800,600),QSize(1280,800),QSize(1920,1080)}) {
        window.resize(size); panelItem->setSize(QSizeF(size.width(),size.height()-48)); QCoreApplication::processEvents();
        auto* step = findVisualItem(panelItem,"samplerSequencerStep0_0");
        ok &= require(step && step->height() >= 44 && step->mapRectToScene(step->boundingRect()).right() <= size.width(),
            "sequencer steps retain touch targets and fit the sampler workspace");
        if (step && size.width() == 800) ok &= clickItem(window,step,"sequencer step toggles from the grid");
        ok &= require(controller->property("sequencerPattern").toList().first().toInt() == 1,
            "sequencer step control updates the active sampler pattern");
    }
    ok &= clickItem(window,findVisualItem(panelItem,"samplerSequencerTransport"),"sampler sequencer transport is reachable");
    ok &= require(controller->property("sequencerEnabled").toBool(), "sequencer play button starts the pattern");
    ok &= clickItem(window,findVisualItem(panelItem,"samplerSequencerView"),"sampler returns from sequencer to pads");
    QMetaObject::invokeMethod(controller.get(),"playFixture"); QCoreApplication::processEvents();
    const int beforeStop = controller->property("triggers").toInt();
    ok &= clickItem(window,findVisualItem(panelItem,"samplerStop0"),"a running pad has a dedicated stop action");
    ok &= require(controller->property("padStops").toInt() == 1 && controller->property("triggers").toInt() == beforeStop,
        "pad stop never sends an additional trigger");
    QMetaObject::invokeMethod(controller.get(),"queueFixture"); QCoreApplication::processEvents();
    auto* countdown = findVisualItem(panelItem,"samplerProgressText0");
    ok &= require(countdown && countdown->property("text").toString().contains("START IN 1.5"),"queued pads display the audio-clock countdown");
    for (const QSize size : {QSize(800,600), QSize(1280,800), QSize(1920,1080)}) {
        window.resize(size); panelItem->setSize(QSizeF(size.width(),size.height()-48));
        controller->setProperty("bank",0); QCoreApplication::processEvents();
        const int triggersBeforeEdit = controller->property("triggers").toInt();
        ok &= clickItem(window,findVisualItem(panelItem,"samplerEdit0"),"dedicated pad edit action responds to touch");
        auto* editorObject = panel->findChild<QObject*>("samplerEditor");
        if (!require(editorObject && editorObject->property("visible").toBool(),"sampler editor opens")) return false;
        ok &= require(controller->property("triggers").toInt() == triggersBeforeEdit,"editing a pad never triggers its sample");
        auto* save = findVisualItem(window.contentItem(),"samplerEditorSave");
        auto* cancel = findVisualItem(window.contentItem(),"samplerEditorCancel");
        ok &= require(save && cancel && save->height() >= 44 && cancel->height() >= 44,
            "editor completion actions retain touch targets at every viewport size");
        auto* handle = findVisualItem(window.contentItem(),"samplerTrimHandle0");
        if (handle) {
            const QPoint origin = handle->mapToScene(QPointF(handle->width()/2,handle->height()/2)).toPoint();
            QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,origin);
            QTest::mouseMove(&window,origin+QPoint(35,0),20);
            QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,origin+QPoint(35,0));
            auto* start = panel->findChild<QObject*>("samplerEditorStart");
            ok &= require(start && start->property("text").toString().toDouble() > 0,"trim handle changes the sample-accurate start boundary");
        } else ok &= require(false,"trim start handle exists");
        ok &= clickItem(window,findVisualItem(window.contentItem(),"samplerEditorPreview"),"editor headphone preview action is reachable");
        ok &= require(controller->property("previewPlaying").toBool(),"preview uses its dedicated controller action");
        controller->setProperty("bank",2); QCoreApplication::processEvents();
        ok &= clickItem(window,save,"editor save remains reachable without scrolling");
        ok &= require(controller->property("lastSavedSlot").toInt() == 0,"bank changes never redirect the open editor");
        ok &= require(!editorObject->property("visible").toBool(),"successful save closes the editor");
        controller->setProperty("bank",0);
        ok &= clickItem(window,findVisualItem(panelItem,"samplerEdit0"),"editor reopens for error handling");
        controller->setProperty("failSave",true);
        ok &= clickItem(window,findVisualItem(window.contentItem(),"samplerEditorSave"),"editor save reports a failed write");
        ok &= require(editorObject->property("visible").toBool() && controller->property("editorError") == "Storage full",
            "a failed save retains the draft and keeps the editor open");
        auto* footerError = findVisualItem(window.contentItem(),"samplerEditorError");
        ok &= require(footerError && footerError->isVisible() && footerError->mapRectToScene(footerError->boundingRect()).bottom() <= size.height(),
            "editor errors stay visible beside the completion actions without scrolling");
        controller->setProperty("failSave",false);
        if (size.width() == 1280 && !qEnvironmentVariableIsEmpty("BROCKDJ_TEST_CAPTURE_DIR")) {
            QTest::qWait(140);
            window.grabWindow().save(QDir(qEnvironmentVariable("BROCKDJ_TEST_CAPTURE_DIR")).filePath("sampler-editor.png"));
        }
        ok &= clickItem(window,findVisualItem(window.contentItem(),"samplerEditorCancel"),"cancel remains reachable on the fixed footer");
        ok &= require(!controller->property("previewPlaying").toBool(),"closing an editor stops its headphone preview");
    }
    panelItem->setVisible(false); QCoreApplication::processEvents();
    ok &= require(controller->property("stops").toInt() == 1,"leaving the sampler page never issues a stop-all command");
    return ok;
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    qmlRegisterType(QUrl::fromLocalFile(QString::fromUtf8(BROCKDJ_SOURCE_DIR)
        + QStringLiteral("/src/qml/components/UiIcon.qml")), "DJSoftware", 1, 0, "UiIcon");
    bool ok = sliderCleanupTests();
    ok &= designSystemRuntimeTests();
    ok &= squareControlsRuntimeTests();
    const auto main = read("src/qml/main.qml");
    ok &= viewportHeightScalingTests(main);
    ok &= sourceViewToggleTests(main);
    ok &= require(main.find("Math.min(window.scaledWaveformHeight, window.adaptiveWaveformHeight)")
                      != std::string::npos,
                  "the workspace waveform minimum follows its adaptive preferred height");
    const auto topHeader = read("src/qml/shell/TopHeader.qml");
    const auto appOverlays = read("src/qml/shell/AppOverlays.qml");
    ok &= headerPublicationTests(topHeader);
    ok &= hamburgerToggleTests(topHeader);
    ok &= latencyPopupToggleTests(topHeader);
    ok &= pullDownHandleTests(topHeader);
    const auto deckControl = read("src/qml/deck/DeckControl.qml");
    const auto slider = read("src/qml/components/Slider.qml");
    const auto mixerSection = read("src/qml/mixer/MixerSection.qml");
    const auto workspace = read("src/qml/performance/PerformanceWorkspace.qml");
    ok &= desktopLibraryHeightTests(main, workspace);
    const auto developmentControls = read("src/qml/development/DevelopmentControlsWindow.qml");
    const auto performancePads = read("src/qml/performance/PerformancePads.qml");
    ok &= appOverlaysRuntimeTests(appOverlays);
    ok &= samplerAndRecordingRuntimeTests(topHeader);
    ok &= effectsPageRuntimeTests(main);
    ok &= mainNavigationRuntimeTests();
    ok &= shortcutRuntimeTests(main);
    ok &= deckCueFlatButtonRuntimeTests(deckControl);
    ok &= aioDeckControlTests(workspace);
    ok &= crossfaderInlineRuntimeTests(developmentControls);
    ok &= turntableInlineRuntimeTests(performancePads);
    ok &= performancePadsThemeRuntimeTests();
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
    ok &= require(read("src/qml/waveform/EnlargedWaveform.qml").find("inputExternalWaveformOverlay")
                      != std::string::npos
                      && read("src/qml/performance/PerformanceWorkspace.qml")
                             .find("enabled: !inputExternal") != std::string::npos,
                  "waveforms and AIO deck controls dim and disable on external FLX10 input");
    ok &= require(deckControl.find("inputExternalOverlay") != std::string::npos
                      && deckControl.find("deck.engine.inputExternal") != std::string::npos
                      && settingsPanel.find("flx10HostPortCombo") != std::string::npos
                      && settingsPanel.find("settingsManager.flx10HostUsbPort") != std::string::npos,
                  "deck dims on external FLX10 input and the USB port is configurable");
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
    ok &= libraryControllerPaneTests(library);
    ok &= librarySwipeTests(library);
    ok &= require(library.find("tr.touchMode && tr.rowSourceTab !== \"usb\" ? 148 : 0")
                      != std::string::npos
                      && library.find("miscActionW > 0 && swipeX") != std::string::npos,
                  "USB rows retain deck swipes but do not expose local mutation actions");
    const auto sourcePage = read("src/qml/library/SourcePage.qml");
    ok &= sourceCursorPersistenceTests(sourcePage);
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
    ok &= require(main.find("gradient:") == std::string::npos
                      && deckTrackInfoPanel.find("gradient:") == std::string::npos
                      && deckTrackInfoPanel.find("panelColor: UiTheme.inspectorSurface") != std::string::npos
                      && deckTrackInfoPanel.find("root.engine.trackArtist") != std::string::npos
                      && deckTrackInfoPanel.find("infoHeight: Math.round(84 * metricScale)") != std::string::npos
                      && deckTrackInfoPanel.find("width: Math.round(parent.width * 0.22)") != std::string::npos
                      && deckTrackInfoPanel.find("anchors.margins: 1") != std::string::npos,
                  "deck information uses flat shared surfaces, artist hierarchy and integer-aligned BPM geometry");
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
                      && occurrences(qmlManifest, "src/qml/") == 26
                      && qmlManifest.find("src/qml/performance/PerformanceBackdrop.qml") != std::string::npos
                      && qmlManifest.find("src/qml/performance/SamplerPanel.qml") != std::string::npos
                      && qmlManifest.find("src/qml/performance/SamplerEditor.qml") != std::string::npos
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
                  "central manifest packages 26 QML resources including shared icons and backdrop and excludes merged components");
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
                      && turntableIndicator.find("preventStealing: true") != std::string::npos
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
                  "master VU consumes the master bus snapshot");
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
                      && sourcePage.find("function restoreSourceCursor()") != std::string::npos
                      && workspace.find("active: window.sourcePageActive || keepLoaded") != std::string::npos,
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
    for (int deck = 0; deck < 4; ++deck) {
        const std::string prefix = std::string("paramId=\"deck") + char('A' + deck)
            + "_crossfader_assign_";
        const std::string status = std::string("\" status=\"0x9") + char('0' + deck)
            + "\" control=\"";
        ok &= require(flx10Mapping.find(prefix + "a" + status + "0x16\" type=\"toggle\"")
                          != std::string::npos
                      && flx10Mapping.find(prefix + "thru" + status + "0x1D\" type=\"toggle\"")
                          != std::string::npos
                      && flx10Mapping.find(prefix + "b" + status + "0x18\" type=\"toggle\"")
                          != std::string::npos,
                      "every FLX10 channel maps A/THRU/B as active-note selections, not continuous CCs");
    }
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
                  && waveformScreen.find("readonly property real panelWidth: UiTheme.px(wideLayout ? 232 : 208)")
                      != std::string::npos
                  && waveformScreen.find("readonly property real handleWidth: UiTheme.touchTarget")
                      != std::string::npos,
                  "performance song information is open by default with accessible touch handles");
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
                  && deckQuickPanel.find("text: root.loadedSourceLabel()") != std::string::npos,
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
