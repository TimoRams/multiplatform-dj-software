#include "MidiControllerManager.h"

#include "ParameterStore.h"
#include "SettingsManager.h"

#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <algorithm>

namespace {
QString toQString(const juce::String& value)
{
    return QString::fromStdString(value.toStdString());
}

int clampMidi7bit(int value)
{
    return std::max(0, std::min(127, value));
}
}

MidiControllerManager::MidiControllerManager(ParameterStore* store, QObject* parent)
    : QObject(parent), m_parameterStore(store)
{
    if (m_parameterStore) {
        connect(m_parameterStore, &ParameterStore::parameterChanged,
                this, &MidiControllerManager::onParameterChanged);
    }

    m_midiDeviceListConnection = juce::MidiDeviceListConnection::make([this]
    {
        QMetaObject::invokeMethod(this, [this]()
        {
            refreshMidiAndMappings();
        }, Qt::QueuedConnection);
    });

    refreshMidiAndMappings();
    restoreSavedDeviceSelections();

    QTimer::singleShot(750, this, [this]()
    {
        refreshMidiAndMappings();
        restoreSavedDeviceSelections();
    });

    m_selectedController = SettingsManager::getInstance().getSelectedController();
    m_selectedMappingFile = SettingsManager::getInstance().getSelectedMappingFile();

    if (!m_selectedMappingFile.isEmpty())
        loadMixxxXmlMapping(m_selectedMappingFile);
}

MidiControllerManager::~MidiControllerManager()
{
    if (m_midiInput)
        m_midiInput->stop();

    stopAlsaInputMonitor();
}

QStringList MidiControllerManager::getAvailableMidiInputDevices()
{
    return m_availableInputDeviceNames;
}

QStringList MidiControllerManager::getAvailableMidiOutputDevices()
{
    return m_availableOutputDeviceNames;
}

QStringList MidiControllerManager::getAvailableMidiDevices()
{
    return getAvailableMidiInputDevices();
}

void MidiControllerManager::refreshMidiDeviceCache()
{
    m_availableInputDeviceIdentifiers.clear();
    m_availableInputDeviceNames.clear();

    const auto inDevices = juce::MidiInput::getAvailableDevices();
    qDebug() << "[MIDI] JUCE input devices:" << static_cast<int>(inDevices.size());
    for (const auto& dev : inDevices) {
        m_availableInputDeviceIdentifiers.push_back(dev.identifier);
        m_availableInputDeviceNames.push_back(toQString(dev.name));
        qDebug() << "[MIDI] Input:" << toQString(dev.name) << "id:" << toQString(dev.identifier);
    }

    m_availableOutputDeviceIdentifiers.clear();
    m_availableOutputDeviceNames.clear();

    const auto outDevices = juce::MidiOutput::getAvailableDevices();
    qDebug() << "[MIDI] JUCE output devices:" << static_cast<int>(outDevices.size());
    for (const auto& dev : outDevices) {
        m_availableOutputDeviceIdentifiers.push_back(dev.identifier);
        m_availableOutputDeviceNames.push_back(toQString(dev.name));
        qDebug() << "[MIDI] Output:" << toQString(dev.name) << "id:" << toQString(dev.identifier);
    }

    if (m_availableInputDeviceNames.isEmpty())
        populateFromAlsaFallback();
}

void MidiControllerManager::populateFromAlsaFallback()
{
    QProcess process;
    process.start("aconnect", {"-l"});
    if (!process.waitForFinished(1500)) {
        qWarning() << "[MIDI] ALSA fallback failed: aconnect timeout";
        return;
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    if (output.trimmed().isEmpty()) {
        qWarning() << "[MIDI] ALSA fallback returned no output";
        return;
    }

    const QRegularExpression clientRegex(R"(^client\s+(\d+):\s+'([^']+)')");
    const QRegularExpression portRegex(R"(^\s*(\d+)\s+'([^']+)')");

    int currentClient = -1;
    QString currentClientName;

    const auto lines = output.split('\n');
    for (const auto& line : lines) {
        const auto c = clientRegex.match(line);
        if (c.hasMatch()) {
            currentClient = c.captured(1).toInt();
            currentClientName = c.captured(2).trimmed();
            continue;
        }

        const auto p = portRegex.match(line);
        if (!p.hasMatch() || currentClient < 0)
            continue;

        const int port = p.captured(1).toInt();
        const QString portName = p.captured(2).trimmed();

        const QString lowerClient = currentClientName.toLower();
        if (lowerClient == "system" || lowerClient == "midi through" || lowerClient.startsWith("pipewire"))
            continue;

        const juce::String identifier("alsa:" + juce::String(currentClient) + ":" + juce::String(port));
        const QString label = QString("%1:%2 - %3").arg(currentClientName).arg(port).arg(portName);

        m_availableInputDeviceIdentifiers.push_back(identifier);
        m_availableInputDeviceNames.push_back(label);
        qDebug() << "[MIDI] ALSA fallback input:" << label << "id:" << toQString(identifier);
    }
}

bool MidiControllerManager::isPseudoAlsaIdentifier(const juce::String& identifier) const
{
    return identifier.startsWith("alsa:");
}

void MidiControllerManager::startAlsaInputMonitor(const juce::String& pseudoIdentifier)
{
    stopAlsaInputMonitor();

    if (!isPseudoAlsaIdentifier(pseudoIdentifier))
        return;

    const QString id = toQString(pseudoIdentifier);
    const QStringList parts = id.split(':');
    if (parts.size() != 3) {
        qWarning() << "[MIDI] Invalid ALSA pseudo identifier:" << id;
        return;
    }

    const QString port = parts.at(1) + ":" + parts.at(2);
    m_alsaInputMonitor = std::make_unique<QProcess>(this);
    m_alsaMonitorBuffer.clear();

    connect(m_alsaInputMonitor.get(), &QProcess::readyReadStandardOutput, this, [this]()
    {
        if (!m_alsaInputMonitor)
            return;

        m_alsaMonitorBuffer.append(QString::fromUtf8(m_alsaInputMonitor->readAllStandardOutput()));

        int newline = m_alsaMonitorBuffer.indexOf('\n');
        while (newline >= 0) {
            const QString line = m_alsaMonitorBuffer.left(newline).trimmed();
            m_alsaMonitorBuffer.remove(0, newline + 1);

            if (!line.isEmpty()) {
                const QRegularExpression numRegex(R"((\d+))");
                const auto matches = numRegex.globalMatch(line);
                QList<int> numbers;
                auto it = matches;
                while (it.hasNext())
                    numbers.push_back(it.next().captured(1).toInt());

                auto decodePair = [&numbers](int& first, int& second) -> bool
                {
                    if (numbers.size() < 2)
                        return false;
                    first = numbers.at(numbers.size() - 2);
                    second = numbers.at(numbers.size() - 1);
                    return true;
                };

                int a = 0;
                int b = 0;

                if (line.contains("Control change", Qt::CaseInsensitive) && decodePair(a, b)) {
                    processDecodedMidiEvent(clampMidi7bit(a) + 1000, clampMidi7bit(b) / 127.0f, false);
                } else if (line.contains("Note on", Qt::CaseInsensitive) && decodePair(a, b)) {
                    processDecodedMidiEvent(clampMidi7bit(a), clampMidi7bit(b) / 127.0f, false);
                } else if (line.contains("Note off", Qt::CaseInsensitive) && decodePair(a, b)) {
                    processDecodedMidiEvent(clampMidi7bit(a), clampMidi7bit(b) / 127.0f, true);
                }
            }

            newline = m_alsaMonitorBuffer.indexOf('\n');
        }
    });

    connect(m_alsaInputMonitor.get(), &QProcess::errorOccurred, this, [port](QProcess::ProcessError error)
    {
        qWarning() << "[MIDI] aseqdump error on" << port << "error:" << static_cast<int>(error);
    });

    connect(m_alsaInputMonitor.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [port](int exitCode, QProcess::ExitStatus status)
    {
        qWarning() << "[MIDI] aseqdump finished on" << port
                   << "exitCode:" << exitCode
                   << "status:" << static_cast<int>(status);
    });

    connect(m_alsaInputMonitor.get(), &QProcess::readyReadStandardError, this, [this]()
    {
        if (!m_alsaInputMonitor)
            return;
        const QString err = QString::fromUtf8(m_alsaInputMonitor->readAllStandardError()).trimmed();
        if (!err.isEmpty())
            qWarning() << "[MIDI] aseqdump stderr:" << err;
    });

    m_alsaInputMonitor->start("aseqdump", {"-p", port});
    if (!m_alsaInputMonitor->waitForStarted(1200)) {
        qWarning() << "[MIDI] Could not start aseqdump for" << port;
        m_alsaInputMonitor.reset();
        return;
    }

    qDebug() << "[MIDI] ALSA monitor started on" << port;
}

void MidiControllerManager::stopAlsaInputMonitor()
{
    if (!m_alsaInputMonitor)
        return;

    if (m_alsaInputMonitor->state() != QProcess::NotRunning) {
        m_alsaInputMonitor->terminate();
        if (!m_alsaInputMonitor->waitForFinished(400))
            m_alsaInputMonitor->kill();
    }

    m_alsaInputMonitor.reset();
    m_alsaMonitorBuffer.clear();
}

void MidiControllerManager::openMidiInputByIdentifier(const juce::String& identifier)
{
    stopAlsaInputMonitor();

    if (m_midiInput) {
        m_midiInput->stop();
        m_midiInput.reset();
    }

    if (identifier.isEmpty())
        return;

    if (isPseudoAlsaIdentifier(identifier)) {
        startAlsaInputMonitor(identifier);
        return;
    }

    auto input = juce::MidiInput::openDevice(identifier, this);
    if (!input) {
        qWarning() << "[MIDI] Failed to open input:" << toQString(identifier);
        return;
    }

    input->start();
    m_midiInput = std::move(input);
    qDebug() << "[MIDI] Input opened:" << toQString(identifier);
}

void MidiControllerManager::openMidiOutputByIdentifier(const juce::String& identifier)
{
    if (m_midiOutput)
        m_midiOutput.reset();

    if (identifier.isEmpty() || isPseudoAlsaIdentifier(identifier))
        return;

    auto output = juce::MidiOutput::openDevice(identifier);
    if (!output) {
        qWarning() << "[MIDI] Failed to open output:" << toQString(identifier);
        return;
    }

    m_midiOutput = std::move(output);
    qDebug() << "[MIDI] Output opened:" << toQString(identifier);
}

void MidiControllerManager::restoreSavedDeviceSelections()
{
    const auto inputId = SettingsManager::getInstance().getMidiInputIdentifier();
    if (!inputId.isEmpty())
        openMidiInputByIdentifier(juce::String::fromUTF8(inputId.toUtf8().constData()));

    const auto outputId = SettingsManager::getInstance().getMidiOutputIdentifier();
    if (!outputId.isEmpty())
        openMidiOutputByIdentifier(juce::String::fromUTF8(outputId.toUtf8().constData()));
}

void MidiControllerManager::selectMidiInputDevice(int index)
{
    if (index < 0 || index >= static_cast<int>(m_availableInputDeviceIdentifiers.size()))
        return;

    const auto identifier = m_availableInputDeviceIdentifiers[static_cast<size_t>(index)];
    openMidiInputByIdentifier(identifier);
    SettingsManager::getInstance().setMidiInputIdentifier(toQString(identifier));
}

void MidiControllerManager::selectMidiOutputDevice(int index)
{
    if (index < 0 || index >= static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
        return;

    const auto identifier = m_availableOutputDeviceIdentifiers[static_cast<size_t>(index)];
    openMidiOutputByIdentifier(identifier);
    SettingsManager::getInstance().setMidiOutputIdentifier(toQString(identifier));
}

void MidiControllerManager::selectMidiDevice(int index)
{
    selectMidiInputDevice(index);

    if (index >= 0 && index < static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
        selectMidiOutputDevice(index);

    emit midiDevicesUpdated();
}

int MidiControllerManager::getSelectedMidiInputIndex() const
{
    const QString selected = SettingsManager::getInstance().getMidiInputIdentifier();
    if (selected.isEmpty())
        return -1;

    const juce::String selectedId = juce::String::fromUTF8(selected.toUtf8().constData());
    for (size_t i = 0; i < m_availableInputDeviceIdentifiers.size(); ++i) {
        if (m_availableInputDeviceIdentifiers[i] == selectedId)
            return static_cast<int>(i);
    }

    return -1;
}

int MidiControllerManager::getSelectedMidiOutputIndex() const
{
    const QString selected = SettingsManager::getInstance().getMidiOutputIdentifier();
    if (selected.isEmpty())
        return -1;

    const juce::String selectedId = juce::String::fromUTF8(selected.toUtf8().constData());
    for (size_t i = 0; i < m_availableOutputDeviceIdentifiers.size(); ++i) {
        if (m_availableOutputDeviceIdentifiers[i] == selectedId)
            return static_cast<int>(i);
    }

    return -1;
}

int MidiControllerManager::getSelectedMidiDeviceIndex() const
{
    return getSelectedMidiInputIndex();
}

QString MidiControllerManager::getMappingsDirectoryPath() const
{
    return SettingsManager::getInstance().getMappingsDirectoryPath();
}

QStringList MidiControllerManager::getAvailableMappingFiles()
{
    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return {};

    return dir.entryList({"*.xml", "*.XML"}, QDir::Files, QDir::Name);
}

QString MidiControllerManager::getSettingsDirectoryPath() const
{
    return SettingsManager::getInstance().getConfigDirectoryPath();
}

bool MidiControllerManager::openSettingsDirectory() const
{
    const QString path = getSettingsDirectoryPath();
    if (path.isEmpty())
        return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool MidiControllerManager::openMappingsDirectory() const
{
    const QString path = getMappingsDirectoryPath();
    if (path.isEmpty())
        return false;

    QDir dir(path);
    if (!dir.exists() && !dir.mkpath("."))
        return false;

    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

QString MidiControllerManager::normalizeControllerKeyFromXmlBase(const QString& baseName) const
{
    QString key = baseName.trimmed();
    if (key.endsWith(".midi", Qt::CaseInsensitive))
        key.chop(5);
    return key.toLower();
}

QString MidiControllerManager::normalizeControllerKeyFromJsBase(const QString& baseName) const
{
    QString key = baseName.trimmed();
    if (key.endsWith(".controller", Qt::CaseInsensitive))
        key.chop(11);
    return key.toLower();
}

QStringList MidiControllerManager::getAvailableControllers()
{
    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return {};

    QMap<QString, QString> dedup;

    const auto xmlFiles = dir.entryList({"*.xml", "*.XML"}, QDir::Files, QDir::Name);
    for (const auto& file : xmlFiles) {
        QString base = QFileInfo(file).completeBaseName();
        const QString normalized = normalizeControllerKeyFromXmlBase(base);
        if (normalized.isEmpty())
            continue;

        if (!dedup.contains(normalized))
            dedup.insert(normalized, base);
    }

    const auto jsFiles = dir.entryList({"*.js", "*.JS"}, QDir::Files, QDir::Name);
    for (const auto& file : jsFiles) {
        QString base = QFileInfo(file).completeBaseName();
        const QString normalized = normalizeControllerKeyFromJsBase(base);
        if (normalized.isEmpty())
            continue;

        if (!dedup.contains(normalized))
            dedup.insert(normalized, base);
    }

    QStringList result = dedup.values();
    result.sort(Qt::CaseInsensitive);
    return result;
}

void MidiControllerManager::selectController(const QString& controllerName)
{
    m_selectedController = controllerName;
    SettingsManager::getInstance().setSelectedController(controllerName);
    emit mappingListUpdated();
}

QString MidiControllerManager::getSelectedController() const
{
    if (!m_selectedController.isEmpty())
        return m_selectedController;
    return SettingsManager::getInstance().getSelectedController();
}

QStringList MidiControllerManager::getAvailableXmlMappingFilesForController(const QString& controllerName) const
{
    const QString normalizedTarget = normalizeControllerKeyFromXmlBase(controllerName);
    if (normalizedTarget.isEmpty())
        return {};

    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return {};

    QStringList files = dir.entryList({"*.xml", "*.XML"}, QDir::Files, QDir::Name);
    QStringList filtered;

    for (const auto& file : files) {
        const QString base = QFileInfo(file).completeBaseName();
        if (normalizeControllerKeyFromXmlBase(base) == normalizedTarget)
            filtered.push_back(file);
    }

    filtered.sort(Qt::CaseInsensitive);
    return filtered;
}

QStringList MidiControllerManager::getAvailableMappingsForSelectedController()
{
    const QString controller = getSelectedController();
    if (controller.isEmpty())
        return getAvailableMappingFiles();
    return getAvailableXmlMappingFilesForController(controller);
}

void MidiControllerManager::selectMapping(const QString& mappingFileName)
{
    m_selectedMappingFile = mappingFileName;
    SettingsManager::getInstance().setSelectedMappingFile(mappingFileName);

    m_midiToParam.clear();
    m_paramToMidi.clear();

    if (!mappingFileName.isEmpty()) {
        if (!loadMixxxXmlMapping(mappingFileName))
            qWarning() << "[MIDI] Failed to load mapping:" << mappingFileName;
    }

    emit mappingUpdated();
    emit mappingListUpdated();
}

QString MidiControllerManager::getSelectedMapping() const
{
    if (!m_selectedMappingFile.isEmpty())
        return m_selectedMappingFile;
    return SettingsManager::getInstance().getSelectedMappingFile();
}

void MidiControllerManager::refreshMidiAndMappings()
{
    refreshMidiDeviceCache();
    emit midiDevicesUpdated();
    emit controllerListUpdated();
    emit mappingListUpdated();
}

int MidiControllerManager::parseMixxxNumber(const QString& rawValue) const
{
    QString value = rawValue.trimmed();
    bool ok = false;

    if (value.startsWith("0x", Qt::CaseInsensitive))
        return value.mid(2).toInt(&ok, 16);

    return value.toInt(&ok, 10);
}

QString MidiControllerManager::mapMixxxControlToInternalParam(const QString& group, const QString& key) const
{
    const QString g = group.trimmed();
    const QString k = key.trimmed().toLower();

    if (g == "[Channel1]") {
        if (k == "play") return "deckA_play";
        if (k == "volume") return "deckA_vol";
        if (k == "pregain") return "deckA_gain";
    }

    if (g == "[Channel2]") {
        if (k == "play") return "deckB_play";
        if (k == "volume") return "deckB_vol";
        if (k == "pregain") return "deckB_gain";
    }

    if (g == "[Master]" && k == "crossfader")
        return "crossfader";

    return {};
}

bool MidiControllerManager::loadMixxxXmlMapping(const QString& mappingFileName)
{
    const QString filePath = QDir(getMappingsDirectoryPath()).filePath(mappingFileName);
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[MIDI] Could not open mapping file:" << filePath;
        return false;
    }

    QXmlStreamReader xml(&file);

    std::map<int, QString> nextMidiToParam;
    std::map<QString, int> nextParamToMidi;

    bool inControl = false;
    QString group;
    QString key;
    QString status;
    QString midino;

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QString name = xml.name().toString();
            if (name == "control") {
                inControl = true;
                group.clear();
                key.clear();
                status.clear();
                midino.clear();
            } else if (inControl && (name == "group" || name == "key" || name == "status" || name == "midino")) {
                const QString text = xml.readElementText().trimmed();
                if (name == "group") group = text;
                if (name == "key") key = text;
                if (name == "status") status = text;
                if (name == "midino") midino = text;
            }
        } else if (xml.isEndElement() && xml.name() == "control") {
            inControl = false;

            const QString param = mapMixxxControlToInternalParam(group, key);
            if (param.isEmpty())
                continue;

            const int midiNo = parseMixxxNumber(midino);
            const int statusNo = parseMixxxNumber(status);
            if (midiNo < 0 || statusNo < 0)
                continue;

            const int statusHi = (statusNo & 0xF0);
            const bool isCc = (statusHi == 0xB0);
            const int msgId = isCc ? (clampMidi7bit(midiNo) + 1000) : clampMidi7bit(midiNo);

            nextMidiToParam[msgId] = param;
            nextParamToMidi[param] = msgId;
        }
    }

    if (xml.hasError()) {
        qWarning() << "[MIDI] XML parse error in" << mappingFileName << ":" << xml.errorString();
        return false;
    }

    m_midiToParam = std::move(nextMidiToParam);
    m_paramToMidi = std::move(nextParamToMidi);

    qDebug() << "[MIDI] Mixxx mapping loaded:" << mappingFileName
             << "entries:" << static_cast<int>(m_midiToParam.size());
    return true;
}

void MidiControllerManager::startMidiLearn(const QString& parameterId)
{
    if (!m_midiInput && !m_alsaInputMonitor) {
        const auto savedInput = SettingsManager::getInstance().getMidiInputIdentifier();
        if (!savedInput.isEmpty()) {
            openMidiInputByIdentifier(juce::String::fromUTF8(savedInput.toUtf8().constData()));
        }
    }

    m_learnParameterId = parameterId;
    m_isLearning = true;
    qDebug() << "[MIDI] Learn started for" << parameterId;
}

void MidiControllerManager::processDecodedMidiEvent(int msgId, float value, bool isNoteOff)
{
    if (m_isLearning && !isNoteOff) {
        m_midiToParam[msgId] = m_learnParameterId;
        m_paramToMidi[m_learnParameterId] = msgId;
        m_isLearning = false;

        qDebug() << "[MIDI] Learned" << msgId << "->" << m_learnParameterId;
        emit mappingUpdated();
        return;
    }

    const auto it = m_midiToParam.find(msgId);
    if (it == m_midiToParam.end())
        return;

    if (!m_parameterStore)
        return;

    const QString paramId = it->second;
    QMetaObject::invokeMethod(m_parameterStore, "setParameter", Qt::QueuedConnection,
                              Q_ARG(QString, paramId),
                              Q_ARG(float, value));
}

void MidiControllerManager::handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message)
{
    int msgId = -1;
    float value = 0.0f;
    bool noteOff = false;

    if (message.isController()) {
        msgId = message.getControllerNumber() + 1000;
        value = clampMidi7bit(message.getControllerValue()) / 127.0f;
    } else if (message.isNoteOn()) {
        msgId = message.getNoteNumber();
        value = message.getFloatVelocity();
    } else if (message.isNoteOff()) {
        msgId = message.getNoteNumber();
        value = 0.0f;
        noteOff = true;
    } else {
        return;
    }

    processDecodedMidiEvent(msgId, value, noteOff);
}

void MidiControllerManager::onParameterChanged(const QString& id, float value)
{
    if (!m_midiOutput)
        return;

    const auto it = m_paramToMidi.find(id);
    if (it == m_paramToMidi.end())
        return;

    const int msgId = it->second;
    juce::MidiMessage msg;

    if (msgId >= 1000) {
        msg = juce::MidiMessage::controllerEvent(1, msgId - 1000, clampMidi7bit(static_cast<int>(value * 127.0f)));
    } else {
        if (value > 0.0f)
            msg = juce::MidiMessage::noteOn(1, clampMidi7bit(msgId), value);
        else
            msg = juce::MidiMessage::noteOff(1, clampMidi7bit(msgId), 0.0f);
    }

    m_midiOutput->sendMessageNow(msg);
}
