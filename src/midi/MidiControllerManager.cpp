#include "MidiControllerManager.h"

#include "DjEngine.h"
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
#include <QtGlobal>
#include <QUrl>
#include <QXmlStreamReader>
#include <algorithm>

namespace {
const juce::String kAllMidiInputsIdentifier("__all_midi_inputs__");

QString toQString(const juce::String& value)
{
    return QString::fromStdString(value.toStdString());
}

int clampMidi7bit(int value)
{
    return std::max(0, std::min(127, value));
}

bool containsIdentifier(const std::vector<juce::String>& ids, const juce::String& needle)
{
    return std::ranges::find(ids, needle) != ids.end();
}

int indexOfIdentifier(const std::vector<juce::String>& ids, const juce::String& needle)
{
    const auto it = std::ranges::find(ids, needle);
    if (it == ids.end())
        return -1;
    return static_cast<int>(std::distance(ids.begin(), it));
}

template <typename DeviceList>
void appendMidiDeviceNames(const DeviceList& devices,
                           std::vector<juce::String>& outIdentifiers,
                           QStringList& outNames)
{
    outIdentifiers.reserve(outIdentifiers.size() + devices.size());
    for (const auto& dev : devices) {
        outIdentifiers.push_back(dev.identifier);
        outNames.push_back(toQString(dev.name));
    }
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
        // Avoid reopening stale saved identifiers repeatedly on startup.
        // Users can still select a device explicitly in settings.
    });

    m_selectedController = SettingsManager::getInstance().getSelectedController();
    m_selectedMappingFile = SettingsManager::getInstance().getSelectedMappingFile();

    if (!m_selectedMappingFile.isEmpty())
        loadMixxxXmlMapping(m_selectedMappingFile);

    loadNativeMappingIfExists();
}

MidiControllerManager::~MidiControllerManager()
{
    for (auto& input : m_midiInputs) {
        if (input)
            input->stop();
    }

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

    m_availableInputDeviceIdentifiers.push_back(kAllMidiInputsIdentifier);
    m_availableInputDeviceNames.push_back(QStringLiteral("All MIDI Inputs"));

    appendMidiDeviceNames(juce::MidiInput::getAvailableDevices(),
                          m_availableInputDeviceIdentifiers,
                          m_availableInputDeviceNames);

    m_availableOutputDeviceIdentifiers.clear();
    m_availableOutputDeviceNames.clear();

    appendMidiDeviceNames(juce::MidiOutput::getAvailableDevices(),
                          m_availableOutputDeviceIdentifiers,
                          m_availableOutputDeviceNames);

#if defined(Q_OS_LINUX)
    // JUCE does not reliably expose every ALSA sequencer port for larger DJ
    // controllers. Keep the direct ALSA ports visible too, so devices like the
    // DDJ-FLX10 can be selected by the port that actually emits button events.
    populateFromAlsaFallback();
#endif
}

void MidiControllerManager::populateFromAlsaFallback()
{
#if defined(Q_OS_LINUX)
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
        if (lowerClient == "system"
            || lowerClient == "midi through"
            || lowerClient.startsWith("pipewire")
            || lowerClient.contains("brockdj")
            || lowerClient.contains("aseqdump"))
            continue;

        const juce::String identifier("alsa:" + juce::String(currentClient) + ":" + juce::String(port));
        const QString label = QString("%1:%2 - %3").arg(currentClientName).arg(port).arg(portName);

        m_availableInputDeviceIdentifiers.push_back(identifier);
        m_availableInputDeviceNames.push_back(label);
    }
#endif
}

bool MidiControllerManager::isPseudoAlsaIdentifier(const juce::String& identifier) const
{
#if defined(Q_OS_LINUX)
    return identifier.startsWith("alsa:");
#else
    Q_UNUSED(identifier);
    return false;
#endif
}

void MidiControllerManager::startAlsaInputMonitor(const juce::String& pseudoIdentifier)
{
#if defined(Q_OS_LINUX)
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

                // aseqdump line format: "... Ch, controller/note N, value/velocity V"
                // Numbers from end: [..., channel, controllerOrNote, value]
                auto decodeTriple = [&numbers](int& ch, int& first, int& second) -> bool
                {
                    if (numbers.size() < 3)
                        return false;
                    ch     = numbers.at(numbers.size() - 3);
                    first  = numbers.at(numbers.size() - 2);
                    second = numbers.at(numbers.size() - 1);
                    return true;
                };

                // Helper: prefer channel-aware msgId if already mapped, else use it for new learns
                auto resolveMsgId = [this](int channelAware, int legacy) -> int
                {
                    if (m_midiToParam.count(channelAware)) return channelAware;
                    if (m_midiToParam.count(legacy))       return legacy;
                    return channelAware; // learning: store channel-aware
                };

                int ch = 0, a = 0, b = 0;

                if (line.contains("Control change", Qt::CaseInsensitive) && decodeTriple(ch, a, b)) {
                    const int cc              = clampMidi7bit(a);
                    const int channelAwareMsgId = 10000 + clampMidi7bit(ch) * 2000 + 1000 + cc;
                    const int legacyMsgId       = cc + 1000;
                    const int msgId             = resolveMsgId(channelAwareMsgId, legacyMsgId);
                    const auto it               = m_midiToParam.find(msgId);
                    float value;
                    if (it != m_midiToParam.end() && (it->second.endsWith("_jog_move") || it->second == "library_browse")) {
                        // Two's-complement 7-bit relative CC: 1–63 = forward, 65–127 = backward
                        value = static_cast<float>(b < 64 ? b : b - 128);
                    } else {
                        value = clampMidi7bit(b) / 127.0f;
                    }
                    processDecodedMidiEvent(msgId, value, false);
                } else if (line.contains("Note on", Qt::CaseInsensitive) ||
                           line.contains("Note off", Qt::CaseInsensitive)) {
                    const bool isOff = line.contains("Note off", Qt::CaseInsensitive);

                    // --- Format A: "Note on 0, note 11, velocity 127"  ← common aseqdump ---
                    static const QRegularExpression noteCommonRx(
                        R"((?:Note\s+on|Note\s+off)\s+(\d+),\s*note\s+(\d+),\s*velocity\s+(\d+))",
                        QRegularExpression::CaseInsensitiveOption);

                    // --- Format B: "Chan N ... (statusByte note vel)"  ← newer aseqdump ---
                    // Extract channel from "Chan N" (1-based) and note/vel from the 3-number
                    // parenthetical. The first number in the paren is the MIDI status byte:
                    // either treat "Chan N" for channel OR decode status & 0x0F.
                    static const QRegularExpression noteParenRx(
                        R"((?:Chan|Channel)\s+(\d+).*?\((\d+)\s+(\d+)\s+(\d+)\))",
                        QRegularExpression::CaseInsensitiveOption);

                    // --- Format C: "Chan N, Note on/off NOTE vel VELOCITY"  ← older aseqdump ---
                    static const QRegularExpression noteVerboseRx(
                        R"((?:Chan|Channel)\s+(\d+).*?(?:velocity|vel)[^\d]*(\d+))",
                        QRegularExpression::CaseInsensitiveOption);

                    int ch0  = -1;
                    int note = -1;
                    int vel  = 0;

                    const auto nmCommon = noteCommonRx.match(line);
                    if (nmCommon.hasMatch()) {
                        ch0  = std::max(0, std::min(15, nmCommon.captured(1).toInt()));
                        note = clampMidi7bit(nmCommon.captured(2).toInt());
                        vel  = clampMidi7bit(nmCommon.captured(3).toInt());
                    } else if (decodeTriple(ch, a, b) && !line.contains("Chan", Qt::CaseInsensitive)) {
                        // Fallback for variants with decorated note names:
                        // the last three numbers are channel, note, velocity.
                        ch0  = std::max(0, std::min(15, ch));
                        note = clampMidi7bit(a);
                        vel  = clampMidi7bit(b);
                    } else {
                        const auto nmA = noteParenRx.match(line);
                        if (nmA.hasMatch()) {
                            ch0  = std::max(0, std::min(15, nmA.captured(1).toInt() - 1));
                            note = clampMidi7bit(nmA.captured(3).toInt());
                            vel  = clampMidi7bit(nmA.captured(4).toInt());
                        } else {
                            // Format C: get note from note-name number or last available digit,
                            // velocity from "velocity N" or "vel N" suffix.
                            // Extract channel from Chan/Channel N
                            static const QRegularExpression chanRx(
                                R"((?:Chan|Channel)\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
                            const auto chanM = chanRx.match(line);
                            if (chanM.hasMatch())
                                ch0 = std::max(0, std::min(15, chanM.captured(1).toInt() - 1));

                            const auto nmB = noteVerboseRx.match(line);
                            if (nmB.hasMatch()) {
                                vel = clampMidi7bit(nmB.captured(2).toInt());
                                // Note number: second-to-last digit group before "velocity"
                                // Fall back to decodeTriple last 2 numbers
                                if (ch0 >= 0 && decodeTriple(ch, a, b)) {
                                    note = clampMidi7bit(a); // 'a' is second-from-last number
                                }
                            }
                        }
                    }

                    if (ch0 >= 0 && note >= 0) {
                        const int channelAwareMsgId = 10000 + ch0 * 2000 + note;
                        const int msgId             = resolveMsgId(channelAwareMsgId, note);
                        const bool zeroVelocity     = isOff || (vel == 0);
                        qDebug() << "[MIDI ALSA]" << (isOff ? "NoteOff" : "NoteOn")
                                 << "ch0:" << ch0 << "note:" << note << "vel:" << vel
                                 << "msgId:" << msgId;
                        processDecodedMidiEvent(msgId, zeroVelocity ? 0.0f : vel / 127.0f, zeroVelocity);
                    }
                } else if ((line.contains("Pitchbend", Qt::CaseInsensitive) ||
                            line.contains("Pitch bend", Qt::CaseInsensitive)) &&
                           decodeTriple(ch, a, b)) {
                    // aseqdump: "Pitchbend  <ch>, value <signed>" — b is raw +8192 offset
                    // decodeTriple gives last 3 numbers; for negative aseqdump values the
                    // sign is stripped by the digit regex, so use the channel-only 2-number
                    // form and parse signed value directly.
                    static const QRegularExpression pbRx(R"((?:pitchbend|pitch\s+bend)\s+(\d+),\s*value\s+(-?\d+))",
                                                         QRegularExpression::CaseInsensitiveOption);
                    const auto pbMatch = pbRx.match(line);
                    if (pbMatch.hasMatch()) {
                        const int pbCh  = pbMatch.captured(1).toInt();
                        const int pbRaw = pbMatch.captured(2).toInt(); // -8192..+8191
                        const int channelAwareMsgId = 10000 + std::max(0, std::min(15, pbCh)) * 2000 + 1500;
                        const int msgId = resolveMsgId(channelAwareMsgId, 1500);
                        const float value = static_cast<float>(pbRaw + 8192) / 16383.0f;
                        processDecodedMidiEvent(msgId, value, false);
                    }
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

#else
    Q_UNUSED(pseudoIdentifier);
#endif
}

void MidiControllerManager::stopAlsaInputMonitor()
{
#if defined(Q_OS_LINUX)
    if (!m_alsaInputMonitor)
        return;

    if (m_alsaInputMonitor->state() != QProcess::NotRunning) {
        m_alsaInputMonitor->terminate();
        if (!m_alsaInputMonitor->waitForFinished(400))
            m_alsaInputMonitor->kill();
    }

    m_alsaInputMonitor.reset();
    m_alsaMonitorBuffer.clear();
#endif
}

void MidiControllerManager::openMidiInputByIdentifier(const juce::String& identifier)
{
    stopAlsaInputMonitor();

    for (auto& input : m_midiInputs) {
        if (input)
            input->stop();
    }
    m_midiInputs.clear();

    if (identifier.isEmpty())
        return;

    if (identifier == kAllMidiInputsIdentifier) {
        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& dev : devices) {
            auto input = juce::MidiInput::openDevice(dev.identifier, this);
            if (!input) {
                qWarning() << "[MIDI] Failed to open input:" << toQString(dev.identifier);
                continue;
            }

            input->start();
            qDebug() << "[MIDI] Opened input:" << toQString(dev.name)
                     << "id:" << toQString(dev.identifier);
            m_midiInputs.push_back(std::move(input));
        }

        if (m_midiInputs.empty())
            qWarning() << "[MIDI] All MIDI Inputs selected, but no JUCE inputs could be opened";
        return;
    }

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
    m_midiInputs.push_back(std::move(input));
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
}

void MidiControllerManager::restoreSavedDeviceSelections()
{
    const auto inputId = SettingsManager::getInstance().getMidiInputIdentifier();
    const juce::String savedInput = inputId.isEmpty()
        ? kAllMidiInputsIdentifier
        : juce::String::fromUTF8(inputId.toUtf8().constData());
    if (containsIdentifier(m_availableInputDeviceIdentifiers, savedInput))
        openMidiInputByIdentifier(savedInput);
    else
        openMidiInputByIdentifier(kAllMidiInputsIdentifier);

    const auto outputId = SettingsManager::getInstance().getMidiOutputIdentifier();
    if (!outputId.isEmpty()) {
        const juce::String savedOutput = juce::String::fromUTF8(outputId.toUtf8().constData());
        if (containsIdentifier(m_availableOutputDeviceIdentifiers, savedOutput))
            openMidiOutputByIdentifier(savedOutput);
    }
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

    const int outputIndex = (index > 0) ? index - 1 : -1;
    if (outputIndex >= 0 && outputIndex < static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
        selectMidiOutputDevice(outputIndex);

    emit midiDevicesUpdated();
}

int MidiControllerManager::getSelectedMidiInputIndex() const
{
    const QString selected = SettingsManager::getInstance().getMidiInputIdentifier();
    if (selected.isEmpty())
        return 0;

    const juce::String selectedId = juce::String::fromUTF8(selected.toUtf8().constData());
    return indexOfIdentifier(m_availableInputDeviceIdentifiers, selectedId);
}

int MidiControllerManager::getSelectedMidiOutputIndex() const
{
    const QString selected = SettingsManager::getInstance().getMidiOutputIdentifier();
    if (selected.isEmpty())
        return -1;

    const juce::String selectedId = juce::String::fromUTF8(selected.toUtf8().constData());
    return indexOfIdentifier(m_availableOutputDeviceIdentifiers, selectedId);
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
        if (k == "play" || k == "play_indicator") return "deckA_play";
        if (k == "cue_default" || k == "cue_cdj" || k == "cue_simple") return "deckA_cue";
        if (k == "pfl" || k == "pfl_indicator" || k == "cue_preview") return "deckA_headphone_cue";
        if (k == "volume") return "deckA_vol";
        if (k == "pregain") return "deckA_gain";
        if (k == "filterhi")  return "deckA_eqHigh";
        if (k == "filtermid") return "deckA_eqMid";
        if (k == "filterlow") return "deckA_eqLow";
        if (k == "filter" || k == "superknob") return "deckA_filter";
        if (k == "rate") return "deckA_tempo";
        if (k == "jog") return "deckA_jog_move";
        if (k == "scratch2_enable") return "deckA_jog_touch";
    }

    if (g == "[Channel2]") {
        if (k == "play" || k == "play_indicator") return "deckB_play";
        if (k == "cue_default" || k == "cue_cdj" || k == "cue_simple") return "deckB_cue";
        if (k == "pfl" || k == "pfl_indicator" || k == "cue_preview") return "deckB_headphone_cue";
        if (k == "volume") return "deckB_vol";
        if (k == "pregain") return "deckB_gain";
        if (k == "filterhi")  return "deckB_eqHigh";
        if (k == "filtermid") return "deckB_eqMid";
        if (k == "filterlow") return "deckB_eqLow";
        if (k == "filter" || k == "superknob") return "deckB_filter";
        if (k == "rate") return "deckB_tempo";
        if (k == "jog") return "deckB_jog_move";
        if (k == "scratch2_enable") return "deckB_jog_touch";
    }

    // Mixxx EQ rack groups: [EqualizerRack1_[ChannelX]_Effect1]
    if (g.contains("[Channel1]") && g.contains("EqualizerRack")) {
        if (k == "parameter1") return "deckA_eqLow";
        if (k == "parameter2") return "deckA_eqMid";
        if (k == "parameter3") return "deckA_eqHigh";
    }
    if (g.contains("[Channel2]") && g.contains("EqualizerRack")) {
        if (k == "parameter1") return "deckB_eqLow";
        if (k == "parameter2") return "deckB_eqMid";
        if (k == "parameter3") return "deckB_eqHigh";
    }

    // Mixxx QuickEffect (filter) rack
    if (g.contains("[Channel1]") && g.contains("QuickEffectRack")) {
        if (k == "super1") return "deckA_filter";
    }
    if (g.contains("[Channel2]") && g.contains("QuickEffectRack")) {
        if (k == "super1") return "deckB_filter";
    }

    if (g == "[Master]" && k == "crossfader")
        return "crossfader";
    if (g == "[Master]" && (k == "headmix" || k == "head_mix" || k == "cue_mix"))
        return "headphone_mix";
    if (g == "[Master]" && (k == "headphones" || k == "master_cue" || k == "mastercue"))
        return "master_cue";

    return {};
}

QString MidiControllerManager::nativeMappingFilePath() const
{
    return SettingsManager::getInstance().getConfigDirectoryPath() + "/midi_mapping_native.xml";
}

QString MidiControllerManager::getMappingLabel(const QString& paramId) const
{
    const auto it = m_paramToMidi.find(paramId);
    if (it == m_paramToMidi.end())
        return {};
    const int msgId = it->second;
    // Channel-aware format: 10000 + channel*2000 + (isCc ? 1000+cc : note)
    if (msgId >= 10000) {
        const int remainder = msgId - 10000;
        const int channel   = remainder / 2000;
        const int sub       = remainder % 2000;
        if (sub == 1500)
            return QStringLiteral("Ch%1 Pitch").arg(channel + 1);
        if (sub >= 1000)
            return QStringLiteral("Ch%1 CC %2").arg(channel + 1).arg(sub - 1000);
        return QStringLiteral("Ch%1 Note %2").arg(channel + 1).arg(sub);
    }
    if (msgId == 1500)
        return QStringLiteral("Pitch");
    if (msgId >= 1000)
        return QStringLiteral("CC %1").arg(msgId - 1000);
    return QStringLiteral("Note %1").arg(msgId);
}

void MidiControllerManager::clearLearnedMapping(const QString& paramId)
{
    const auto paramIt = m_paramToMidi.find(paramId);
    if (paramIt == m_paramToMidi.end())
        return;

    const int msgId = paramIt->second;
    m_paramToMidi.erase(paramIt);
    m_midiToParam.erase(msgId);

    saveNativeMapping();
    emit mappingUpdated();
}

bool MidiControllerManager::isMappingInverted(const QString& paramId) const
{
    const auto it = m_paramInverted.find(paramId);
    return it != m_paramInverted.end() && it->second;
}

void MidiControllerManager::setMappingInverted(const QString& paramId, bool inverted)
{
    if (inverted)
        m_paramInverted[paramId] = true;
    else
        m_paramInverted.erase(paramId);
    saveNativeMapping();
    emit mappingInversionUpdated();
}

void MidiControllerManager::saveNativeMapping()
{
    const QString path = nativeMappingFilePath();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[MIDI] Could not write native mapping to" << path;
        return;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("BrockDJ_Mapping");
    xml.writeAttribute("version", "1");

    for (const auto& [msgId, paramId] : m_midiToParam) {
        xml.writeStartElement("Entry");
        xml.writeAttribute("paramId", paramId);
        xml.writeAttribute("msgId", QString::number(msgId));
        const auto invIt = m_paramInverted.find(paramId);
        if (invIt != m_paramInverted.end() && invIt->second)
            xml.writeAttribute("inverted", "1");
        xml.writeEndElement();
    }

    xml.writeEndElement();
    xml.writeEndDocument();

    qDebug() << "[MIDI] Native mapping saved:" << path
             << "entries:" << static_cast<int>(m_midiToParam.size());
}

void MidiControllerManager::loadNativeMappingIfExists()
{
    const QString path = nativeMappingFilePath();
    QFile file(path);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[MIDI] Could not read native mapping from" << path;
        return;
    }

    QXmlStreamReader xml(&file);
    int count = 0;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name().toString() != "Entry")
            continue;

        const QString paramId = xml.attributes().value("paramId").toString();
        bool ok = false;
        const int msgId = xml.attributes().value("msgId").toString().toInt(&ok);
        const bool inverted = xml.attributes().value("inverted").toString() == QStringLiteral("1");
        if (ok && !paramId.isEmpty()) {
            m_midiToParam[msgId] = paramId;
            m_paramToMidi[paramId] = msgId;
            if (inverted)
                m_paramInverted[paramId] = true;
            ++count;
        }
    }

    qDebug() << "[MIDI] Native mapping loaded:" << path << "entries:" << count;
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
        } else if (xml.isEndElement() && xml.name().toString() == "control") {
            inControl = false;

            const QString param = mapMixxxControlToInternalParam(group, key);
            if (param.isEmpty())
                continue;

            const int midiNo = parseMixxxNumber(midino);
            const int statusNo = parseMixxxNumber(status);
            if (midiNo < 0 || statusNo < 0)
                continue;

            const int statusHi = (statusNo & 0xF0);
            const int midiCh   = statusNo & 0x0F; // 0-based channel from status byte
            int subId = clampMidi7bit(midiNo);
            if (statusHi == 0xB0)
                subId = 1000 + clampMidi7bit(midiNo);
            else if (statusHi == 0xE0)
                subId = 1500;
            else if (statusHi != 0x80 && statusHi != 0x90)
                continue;

            const int msgId = 10000 + midiCh * 2000 + subId;

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
    bool hasLinuxAlsaMonitor = false;
#if defined(Q_OS_LINUX)
    hasLinuxAlsaMonitor = (m_alsaInputMonitor != nullptr);
#endif

    if (!hasLinuxAlsaMonitor)
        openMidiInputByIdentifier(kAllMidiInputsIdentifier);

    m_learnParameterId = parameterId;
    m_isLearning = true;
    // Clear live monitor so the UI immediately shows the next incoming event
    m_lastMidiEvent.clear();
    emit lastMidiEventChanged();
    qDebug() << "[MIDI] Learn started for" << parameterId;
    emit learnStarted(parameterId);
}

void MidiControllerManager::processDecodedMidiEvent(int msgId, float value, bool isNoteOff)
{
    // Live MIDI monitor — update for every event (both JUCE and ALSA paths)
    {
        const int sub  = (msgId >= 10000) ? (msgId - 10000) % 2000 : -1;
        const int chNo = (msgId >= 10000) ? (msgId - 10000) / 2000 : 0;
        QString evtLabel;
        if (isNoteOff)        evtLabel = QStringLiteral("Ch%1 NoteOff %2").arg(chNo+1).arg(sub);
        else if (sub == 1500) evtLabel = QStringLiteral("Ch%1 PitchBend").arg(chNo+1);
        else if (sub >= 1000) evtLabel = QStringLiteral("Ch%1 CC %2 = %3").arg(chNo+1).arg(sub-1000).arg(static_cast<int>(value*127));
        else if (sub >= 0)    evtLabel = QStringLiteral("Ch%1 Note %2  vel %3").arg(chNo+1).arg(sub).arg(static_cast<int>(value*127));

        qDebug() << "[MIDI IN]" << evtLabel << (m_isLearning ? "(LEARNING)" : "");
        if (!evtLabel.isEmpty() && evtLabel != m_lastMidiEvent) {
            m_lastMidiEvent = evtLabel;
            emit lastMidiEventChanged();
        }
    }

    if (m_isLearning && !isNoteOff) {
        learnMapping(msgId);
        return; // always skip dispatch while in learn mode
    }

    if (!m_parameterStore)
        return;

    // 14-bit CC handling: accumulate MSB (CC 0-31) and combine with LSB (CC 32-63).
    // The DDJ-FLX10 (and most modern controllers) send every analog control as a
    // MSB+LSB pair for 14-bit resolution instead of 7-bit.
    {
        const int sub = (msgId >= 10000) ? (msgId - 10000) % 2000 : -1;

        if (!isNoteOff && sub >= 1000 && sub < 1032) {
            // MSB CC (CC 0-31): accumulate the 7-bit value for later LSB pairing
            const auto msbIt = m_midiToParam.find(msgId);
            if (msbIt != m_midiToParam.end())
                m_msbAccumulator[msbIt->second] = static_cast<int>(value * 127.0f);
            // fall through to standard 7-bit dispatch below so the control moves
            // immediately, even before the LSB arrives
        } else if (!isNoteOff && sub >= 1032 && sub < 1064) {
            // LSB CC (CC 32-63): combine with stored MSB for 14-bit precision
            const int msbMsgId = msgId - 32; // paired MSB is always 32 less
            const auto msbIt = m_midiToParam.find(msbMsgId);
            if (msbIt != m_midiToParam.end()) {
                const QString& pId = msbIt->second;
                const int lsb = static_cast<int>(value * 127.0f);
                const int msb = m_msbAccumulator.count(pId) ? m_msbAccumulator.at(pId) : 64;
                const float combined = static_cast<float>((msb << 7) | lsb) / 16383.0f;
                QMetaObject::invokeMethod(m_parameterStore, "setParameter",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, pId),
                                          Q_ARG(float, combined));
                return; // handled via 14-bit pairing; don't double-dispatch
            }
            // No paired MSB found → fall through to standard dispatch
        }
    }

    const auto it = m_midiToParam.find(msgId);
    if (it == m_midiToParam.end())
        return;

    const QString paramId = it->second;
    float dispatchValue = value;
    {
        const auto invIt = m_paramInverted.find(paramId);
        if (invIt != m_paramInverted.end() && invIt->second) {
            if (paramId.endsWith(QStringLiteral("_jog_move")) || paramId == QStringLiteral("library_browse"))
                dispatchValue = -dispatchValue;
            else if (!isNoteOff)  // never invert a release — NoteOff is always 0
                dispatchValue = 1.0f - dispatchValue;
        }
    }
    // jog_move sends relative delta ticks: consecutive identical values ARE distinct ticks,
    // so always dispatch. All other params deduplicate to break MIDI feedback echo loops
    // (common on PipeWire/ALSA: the software's own LED feedback is echoed back as input).
    if (paramId.endsWith(QStringLiteral("_jog_move")) || paramId == QStringLiteral("library_browse")) {
        QMetaObject::invokeMethod(m_parameterStore, "setMidiParameter", Qt::QueuedConnection,
                                  Q_ARG(QString, paramId), Q_ARG(float, dispatchValue));
    } else {
        QMetaObject::invokeMethod(m_parameterStore, "setParameter", Qt::QueuedConnection,
                                  Q_ARG(QString, paramId), Q_ARG(float, dispatchValue));
    }
}

void MidiControllerManager::learnMapping(int msgId)
{
    if (m_learnParameterId.isEmpty())
        return;

    // Keep the maps one-to-one. Re-learning a control must not leave an old
    // MIDI event still driving the same parameter, and stealing a MIDI event
    // must clear the previous parameter's reverse lookup.
    const auto oldParamForMidi = m_midiToParam.find(msgId);
    if (oldParamForMidi != m_midiToParam.end())
        m_paramToMidi.erase(oldParamForMidi->second);

    const auto oldMidiForParam = m_paramToMidi.find(m_learnParameterId);
    if (oldMidiForParam != m_paramToMidi.end())
        m_midiToParam.erase(oldMidiForParam->second);

    const QString learnedParamId = m_learnParameterId;
    m_midiToParam[msgId] = learnedParamId;
    m_paramToMidi[learnedParamId] = msgId;
    m_learnParameterId.clear();
    m_isLearning = false;

    qDebug() << "[MIDI] Learned" << msgId << "->" << learnedParamId;
    saveNativeMapping();
    emit mappingUpdated();
}

void MidiControllerManager::handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message)
{
    // Diagnostic: log every incoming MIDI message so we can see if JUCE is even
    // receiving Note On events.  The raw status byte tells us the truth.
    {
        const auto* d = message.getRawData();
        const int   sz = message.getRawDataSize();
        const int   status = sz > 0 ? static_cast<unsigned char>(d[0]) : 0;
        const int   d1     = sz > 1 ? static_cast<unsigned char>(d[1]) : -1;
        const int   d2     = sz > 2 ? static_cast<unsigned char>(d[2]) : -1;
        qDebug() << "[MIDI JUCE]" << "status:" << status
                 << "d1:" << d1 << "d2:" << d2
                 << "isNoteOn:" << message.isNoteOn()
                 << "isNoteOff:" << message.isNoteOff()
                 << "isCC:" << message.isController();
    }

    // Runs on the JUCE MIDI thread — decode raw bytes (cheap, no allocation),
    // then dispatch to the Qt main thread so processDecodedMidiEvent can safely
    // read/write m_isLearning and the maps without a data race.
    const int ch = message.getChannel() - 1; // 0-based

    // rawEnc encoding by type:
    //   CC         → raw controller value 0-127 (int stored as float)
    //   Note On/Off→ velocity 0.0-1.0 float
    //   Pitch Bend → raw 14-bit value 0-16383
    int   msgId   = -1;
    float rawEnc  = 0.0f;
    bool  noteOff = false;

    if (message.isController()) {
        msgId  = 10000 + ch * 2000 + 1000 + message.getControllerNumber();
        rawEnc = static_cast<float>(message.getControllerValue()); // 0-127
    } else if (message.isNoteOn()) {
        msgId  = 10000 + ch * 2000 + message.getNoteNumber();
        rawEnc = message.getFloatVelocity();
    } else if (message.isNoteOff()) {
        msgId  = 10000 + ch * 2000 + message.getNoteNumber();
        noteOff = true;
    } else if (message.isPitchWheel()) {
        // sub-ID 1500 is reserved for pitch bend (per channel, no note/cc number)
        msgId  = 10000 + ch * 2000 + 1500;
        rawEnc = static_cast<float>(message.getPitchWheelValue()); // 0-16383
    } else {
        return;
    }

    // Marshal to Qt main thread. Qt cancels the call automatically if `this`
    // is destroyed before the event loop processes it.
    QMetaObject::invokeMethod(this, [this, msgId, rawEnc, noteOff]() mutable
    {
        int   resolvedId    = msgId;
        float resolvedValue = rawEnc;

        if (msgId >= 10000) {
            const int sub = (msgId - 10000) % 2000;

            if (sub >= 1000 && sub < 1500) {
                // Regular CC: check legacy (channel-stripped) mapping too
                const int cc       = sub - 1000;
                const int legacyId = cc + 1000;
                if (!m_midiToParam.count(msgId) && m_midiToParam.count(legacyId))
                    resolvedId = legacyId;

                const auto it = m_midiToParam.find(resolvedId);
                if (it != m_midiToParam.end() && it->second.endsWith("_jog_move")) {
                    const int raw = static_cast<int>(rawEnc);
                    resolvedValue = static_cast<float>(raw < 64 ? raw : raw - 128);
                } else {
                    resolvedValue = clampMidi7bit(static_cast<int>(rawEnc)) / 127.0f;
                }
            } else if (sub == 1500) {
                const int legacyId = 1500;
                if (!m_midiToParam.count(msgId) && m_midiToParam.count(legacyId))
                    resolvedId = legacyId;

                // Pitch bend: normalise 0-16383 → 0.0-1.0
                resolvedValue = rawEnc / 16383.0f;
            }
            // sub < 1000 → Note On/Off: rawEnc is already a normalised float velocity
        }

        processDecodedMidiEvent(resolvedId, resolvedValue, noteOff);
    }, Qt::QueuedConnection);
}

void MidiControllerManager::onParameterChanged(const QString& id, float value)
{
    if (!m_midiOutput)
        return;

    // Jog params are input-only; no MIDI feedback needed
    if (id.endsWith("_jog_move") || id.endsWith("_jog_touch"))
        return;

    const auto it = m_paramToMidi.find(id);
    if (it == m_paramToMidi.end())
        return;

    const int msgId = it->second;
    juce::MidiMessage msg;

    // Decode channel and sub-id from channel-aware format (≥10000) or legacy format
    int channel = 1; // 1-based for JUCE
    int subId   = msgId;
    if (msgId >= 10000) {
        const int remainder = msgId - 10000;
        channel = (remainder / 2000) + 1; // convert 0-based channel to 1-based
        subId   = remainder % 2000;
    }

    if (subId == 1500) {
        const int pitch = std::max(0, std::min(16383, static_cast<int>(value * 16383.0f)));
        msg = juce::MidiMessage::pitchWheel(channel, pitch);
    } else if (subId >= 1000 && subId < 1500) {
        msg = juce::MidiMessage::controllerEvent(channel, subId - 1000,
                                                  clampMidi7bit(static_cast<int>(value * 127.0f)));
    } else {
        if (value > 0.0f)
            msg = juce::MidiMessage::noteOn(channel, clampMidi7bit(subId), value);
        else
            msg = juce::MidiMessage::noteOff(channel, clampMidi7bit(subId), 0.0f);
    }

    m_midiOutput->sendMessageNow(msg);
}

void MidiControllerManager::connectDecks(DjEngine* deckA, DjEngine* deckB)
{
    m_deckA = deckA;
    m_deckB = deckB;

    if (!m_parameterStore)
        return;

    // Route ParameterStore events to deck actions.
    // Volume and crossfader are handled in QML via parameterStore directly.
    // Button convention: value > 0 = press/on, value == 0 = release/off.
    QObject::connect(m_parameterStore, &ParameterStore::parameterChanged,
        this, [this](const QString& id, float value)
    {
        DjEngine* const a = m_deckA;
        DjEngine* const b = m_deckB;

        if (id == "deckA_play") { if (value > 0.0f && a) a->togglePlay(); }
        else if (id == "deckB_play") { if (value > 0.0f && b) b->togglePlay(); }
        else if (id == "deckA_cue") {
            if (a) { if (value > 0.0f) a->cueButtonPress(); else a->cueButtonRelease(); }
        }
        else if (id == "deckB_cue") {
            if (b) { if (value > 0.0f) b->cueButtonPress(); else b->cueButtonRelease(); }
        }
        else if (id == "deckA_headphone_cue") {
            if (value > 0.0f && a) a->setCueEnabled(!a->cueEnabled());
        }
        else if (id == "deckB_headphone_cue") {
            if (value > 0.0f && b) b->setCueEnabled(!b->cueEnabled());
        }
        else if (id == "master_cue") {
            if (value > 0.0f && a) a->setMasterCueEnabled(!a->masterCueEnabled());
        }
        else if (id == "headphone_mix") {
            if (a) a->setHeadphoneMix(static_cast<double>(value));
        }
        // Trim: MIDI 0-1 → engine 0-2 (center = 1.0 = unity)
        else if (id == "deckA_gain")   { if (a) a->setTrim(static_cast<double>(value) * 2.0); }
        else if (id == "deckB_gain")   { if (b) b->setTrim(static_cast<double>(value) * 2.0); }
        // EQ/filter: MIDI 0-1 → engine -1 to +1 (center = 0.0)
        else if (id == "deckA_eqHigh") { if (a) a->setEqHigh(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckB_eqHigh") { if (b) b->setEqHigh(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckA_eqMid")  { if (a) a->setEqMid(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckB_eqMid")  { if (b) b->setEqMid(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckA_eqLow")  { if (a) a->setEqLow(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckB_eqLow")  { if (b) b->setEqLow(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckA_filter") { if (a) a->setFilter(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckB_filter") { if (b) b->setFilter(static_cast<double>(value) * 2.0 - 1.0); }
        // Tempo fader: MIDI 0-1 → engine -8% to +8%
        else if (id == "deckA_tempo")  { if (a) a->setTempoPercent(static_cast<double>(value) * 16.0 - 8.0); }
        else if (id == "deckB_tempo")  { if (b) b->setTempoPercent(static_cast<double>(value) * 16.0 - 8.0); }
        // Jog touch: value > 0 = finger down (enter scratch), 0 = lift (resume)
        else if (id == "deckA_jog_touch") {
            const bool touched = (value > 0.0f);
            if (touched != m_jogATouched) {
                m_jogATouched = touched;
                if (a) { if (touched) a->pauseForScrub(); else a->resumeAfterScrub(); }
            }
        }
        else if (id == "deckB_jog_touch") {
            const bool touched = (value > 0.0f);
            if (touched != m_jogBTouched) {
                m_jogBTouched = touched;
                if (b) { if (touched) b->pauseForScrub(); else b->resumeAfterScrub(); }
            }
        }
        // Jog move: value = signed tick delta; 128 ticks/rev at 33.33 RPM vinyl.
        // With touch (scrubbing): scratch. Without touch (rim turn): speed nudge.
        else if (id == "deckA_jog_move") {
            const double delta = static_cast<double>(value) * (60.0 / 33.333) / 128.0;
            if (a) { if (a->isScrubbing()) a->scratchBySeconds(delta); else a->applyJogNudge(static_cast<double>(value)); }
        }
        else if (id == "deckB_jog_move") {
            const double delta = static_cast<double>(value) * (60.0 / 33.333) / 128.0;
            if (b) { if (b->isScrubbing()) b->scratchBySeconds(delta); else b->applyJogNudge(static_cast<double>(value)); }
        }
    });
}
