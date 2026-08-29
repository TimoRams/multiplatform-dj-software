#include "MidiControllerManager.h"
#include "MidiParameterDispatch.h"
#include "controllers/flx10/Flx10ControllerIdentity.h"
#include "platform/FileManagerLaunch.h"

using namespace midi;

#include "app/SettingsManager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>

QString MidiControllerManager::getMappingsDirectoryPath() const
{
    return SettingsManager::getInstance().getMappingsDirectoryPath();
}

QStringList MidiControllerManager::getAvailableMappingFiles()
{
    QStringList mappings { flx10::kMappingLabel };

    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return mappings;

    mappings.append(dir.entryList({"*.xml", "*.XML"}, QDir::Files, QDir::Name));
    mappings.removeDuplicates();
    return mappings;
}

QString MidiControllerManager::getSettingsDirectoryPath() const
{
    return SettingsManager::getInstance().getConfigDirectoryPath();
}

bool MidiControllerManager::openSettingsDirectory() const
{
    return platform::openDirectoryInFileManager(getSettingsDirectoryPath());
}

bool MidiControllerManager::openMappingsDirectory() const
{
    return platform::openDirectoryInFileManager(getMappingsDirectoryPath());
}

QString MidiControllerManager::normalizeControllerKeyFromXmlBase(const QString& baseName) const
{
    QString key = baseName.trimmed();
    if (key.endsWith(".midi", Qt::CaseInsensitive))
        key.chop(5);
    if (key.endsWith(".brockdj", Qt::CaseInsensitive))
        key.chop(8);
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
    QMap<QString, QString> dedup;
    dedup.insert(normalizeControllerKeyFromXmlBase(flx10::kControllerName), flx10::kControllerName);

    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return dedup.values();

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

    QStringList filtered;
    if (normalizedTarget == normalizeControllerKeyFromXmlBase(flx10::kControllerName))
        filtered.push_back(flx10::kMappingLabel);

    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return filtered;

    QStringList files = dir.entryList({"*.xml", "*.XML"}, QDir::Files, QDir::Name);

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
    cancelBeatJumpSearch();
    m_selectedMappingFile = mappingFileName;
    SettingsManager::getInstance().setSelectedMappingFile(mappingFileName);

    m_midiToParam.clear();
    m_paramToMidi.clear();
    m_momentaryHeldByMsgId.clear();
    m_scratchAbsoluteLastByMsgId.clear();
    resetHighResolutionControlState();

    bool loaded = mappingFileName.isEmpty();
    if (!mappingFileName.isEmpty()) {
        loaded = loadBrockDjXmlMapping(mappingFileName);
        if (!loaded)
            qWarning() << "[MIDI] Failed to load mapping:" << mappingFileName;
    }
    setNativeFlx10ScratchEnabled(
        loaded && flx10::isBuiltInMapping(mappingFileName));

    emit mappingUpdated();
    emit mappingListUpdated();
    startFlx10OutputSession();
    refreshAllDeckLeds();
}

QString MidiControllerManager::getSelectedMapping() const
{
    if (!m_selectedMappingFile.isEmpty())
        return m_selectedMappingFile;
    return SettingsManager::getInstance().getSelectedMappingFile();
}

int MidiControllerManager::parseMappingNumber(const QString& rawValue) const
{
    QString value = rawValue.trimmed();
    bool ok = false;

    if (value.startsWith("0x", Qt::CaseInsensitive)) {
        const int parsed = value.mid(2).toInt(&ok, 16);
        return ok ? parsed : -1;
    }

    const int parsed = value.toInt(&ok, 10);
    return ok ? parsed : -1;
}

int MidiControllerManager::midiMessageIdFromStatusAndControl(int statusNo, int controlNo) const
{
    if (statusNo < 0 || controlNo < 0)
        return -1;

    const int statusHi = statusNo & 0xF0;
    const int midiCh = statusNo & 0x0F;
    int subId = midi::clampMidi7bit(controlNo);

    if (statusHi == 0xB0)
        subId = 1000 + midi::clampMidi7bit(controlNo);
    else if (statusHi == 0xE0)
        subId = 1500;
    else if (statusHi != 0x80 && statusHi != 0x90)
        return -1;

    return 10000 + midiCh * 2000 + subId;
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
    m_momentaryHeldByMsgId.erase(msgId);
    m_scratchAbsoluteLastByMsgId.erase(msgId);

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

    for (const auto& [msgId, entry] : m_midiToParam) {
        xml.writeStartElement("Entry");
        xml.writeAttribute("paramId", entry.paramId);
        xml.writeAttribute("msgId", QString::number(msgId));
        xml.writeAttribute("interactionType", midi::interactionTypeToString(entry.interactionType));
        const auto invIt = m_paramInverted.find(entry.paramId);
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
        const MidiInteractionType interactionType =
            midi::interactionTypeFromString(xml.attributes().value("interactionType").toString(), paramId);
        const bool inverted = xml.attributes().value("inverted").toString() == QStringLiteral("1");
        if (ok && !paramId.isEmpty()) {
            m_midiToParam[msgId] = midi::makeMappingEntry(paramId, interactionType);
            if (interactionType == MidiInteractionType::Momentary)
                m_momentaryHeldByMsgId[msgId] = false;
            else
                m_momentaryHeldByMsgId.erase(msgId);
            m_paramToMidi[paramId] = msgId;
            if (inverted)
                m_paramInverted[paramId] = true;
            ++count;
        }
    }

    qDebug() << "[MIDI] Native mapping loaded:" << path << "entries:" << count;

    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (outputOpen) {
        startFlx10OutputSession();
        if (shouldUseFlx10Feedback())
            refreshAllDeckLeds();
    }
}

bool MidiControllerManager::loadBrockDjXmlMapping(const QString& mappingFileName)
{
    const bool builtInMapping = flx10::isBuiltInMapping(mappingFileName);
    const QString filePath = builtInMapping
        ? flx10::kMappingResource
        : QDir(getMappingsDirectoryPath()).filePath(mappingFileName);
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[MIDI] Could not open mapping file:" << filePath;
        return false;
    }

    QXmlStreamReader xml(&file);

    std::map<int, MidiMappingEntry> nextMidiToParam;
    std::map<QString, int> nextParamToMidi;
    std::map<QString, bool> nextParamInverted;
    MidiFeedbackMapping feedbackMapping;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;

        const QString elementName = xml.name().toString();
        const auto attrs = xml.attributes();

        if (builtInMapping)
            applyMidiFeedbackMappingElement(elementName, attrs, feedbackMapping);

        if (elementName.compare(QStringLiteral("Entry"), Qt::CaseInsensitive) != 0)
            continue;

        const QString paramId = attrs.value("paramId").toString().trimmed();
        if (paramId.isEmpty())
            continue;

        bool ok = false;
        int msgId = attrs.value("msgId").toString().toInt(&ok);
        if (!ok) {
            const QString controlRaw = attrs.hasAttribute(QStringLiteral("control"))
                ? attrs.value("control").toString()
                : attrs.value("midino").toString();
            const int statusNo = parseMappingNumber(attrs.value("status").toString());
            const int controlNo = parseMappingNumber(controlRaw);
            msgId = midiMessageIdFromStatusAndControl(statusNo, controlNo);
        }
        if (msgId < 0)
            continue;

        const QString typeRaw = attrs.hasAttribute(QStringLiteral("type"))
            ? attrs.value("type").toString()
            : attrs.value("interactionType").toString();
        const MidiInteractionType interactionType = midi::interactionTypeFromString(typeRaw, paramId);
        const QString invertedRaw = attrs.value("inverted").toString().trimmed().toLower();
        const bool inverted = invertedRaw == QStringLiteral("1")
            || invertedRaw == QStringLiteral("true")
            || invertedRaw == QStringLiteral("yes");

        nextMidiToParam[msgId] = midi::makeMappingEntry(paramId, interactionType);
        // A high-resolution control has two entries for the same parameter.
        // Keep the first (MSB) as its display/feedback identity while both
        // message IDs remain available to the input dispatcher.
        nextParamToMidi.try_emplace(paramId, msgId);
        if (inverted)
            nextParamInverted[paramId] = true;
    }

    if (xml.hasError()) {
        qWarning() << "[MIDI] XML parse error in" << mappingFileName << ":" << xml.errorString();
        return false;
    }

    m_midiToParam = std::move(nextMidiToParam);
    m_paramToMidi = std::move(nextParamToMidi);
    m_paramInverted = std::move(nextParamInverted);
    m_momentaryHeldByMsgId.clear();
    m_scratchAbsoluteLastByMsgId.clear();
    resetHighResolutionControlState();
    for (const auto& [msgId, entry] : m_midiToParam) {
        if (entry.interactionType == MidiInteractionType::Momentary)
            m_momentaryHeldByMsgId[msgId] = false;
    }

    qDebug() << "[MIDI] BrockDJ mapping loaded:" << mappingFileName
             << (builtInMapping ? "(built-in)" : "(user)")
             << "entries:" << static_cast<int>(m_midiToParam.size());
    if (builtInMapping) {
        m_midiFeedback.setMapping(feedbackMapping);
        setPadModeForDeck(QLatin1Char('A'), MidiPadMode::HotCue);
        setPadModeForDeck(QLatin1Char('B'), MidiPadMode::HotCue);
        m_deckAHotCueHold = {};
        m_deckBHotCueHold = {};
    }

    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (outputOpen) {
        startFlx10OutputSession();
        if (shouldUseFlx10Feedback())
            refreshAllDeckLeds();
    }
    return true;
}

void MidiControllerManager::applyMidiFeedbackMappingElement(const QString& elementName,
                                                             const QXmlStreamAttributes& attrs,
                                                             MidiFeedbackMapping& mapping) const
{
    auto parseByteAttr = [this, &attrs](const QString& name, int fallback) -> uint8_t
    {
        if (!attrs.hasAttribute(name))
            return static_cast<uint8_t>(std::clamp(fallback, 0, 255));
        const int parsed = parseMappingNumber(attrs.value(name).toString());
        return static_cast<uint8_t>(std::clamp(parsed >= 0 ? parsed : fallback, 0, 255));
    };

    if (elementName.compare(QStringLiteral("DeckLed"), Qt::CaseInsensitive) == 0) {
        const QString name = attrs.value(QStringLiteral("name")).toString().trimmed().toLower();
        const uint8_t control = parseByteAttr(QStringLiteral("control"), 0);
        if (name == QStringLiteral("play"))
            mapping.playNote = control;
        else if (name == QStringLiteral("cue"))
            mapping.cueNote = control;
        else if (name == QStringLiteral("headphone_cue"))
            mapping.headphoneCueNote = control;
        else if (name == QStringLiteral("loop_in"))
            mapping.loopInNote = control;
        else if (name == QStringLiteral("loop_out"))
            mapping.loopOutNote = control;
        else if (name == QStringLiteral("loop_4beat"))
            mapping.loop4BeatNote = control;
        else if (name == QStringLiteral("loop_reloop"))
            mapping.loopReloopNote = control;
        else if (name == QStringLiteral("tempo_reset"))
            mapping.tempoResetNote = control;
        else if (name == QStringLiteral("beat_sync"))
            mapping.beatSyncNote = control;
        else if (name == QStringLiteral("key_sync"))
            mapping.keySyncNote = control;
        else if (name == QStringLiteral("quantize"))
            mapping.quantizeNote = control;
        else if (name == QStringLiteral("slip_reverse"))
            mapping.slipReverseNote = control;
        return;
    }

    if (elementName.compare(QStringLiteral("DeckStatus"), Qt::CaseInsensitive) == 0) {
        const int deck = std::clamp(attrs.value(QStringLiteral("deck")).toInt(), 1, 4);
        mapping.deckNoteStatus[static_cast<size_t>(deck - 1)] =
            parseByteAttr(QStringLiteral("status"), mapping.deckNoteStatus[static_cast<size_t>(deck - 1)]);
        return;
    }

    if (elementName.compare(QStringLiteral("HotcuePads"), Qt::CaseInsensitive) == 0) {
        const int deck = std::clamp(attrs.value(QStringLiteral("deck")).toInt(), 1, 4);
        mapping.hotcueStatus[static_cast<size_t>(deck - 1)] =
            parseByteAttr(QStringLiteral("status"), mapping.hotcueStatus[static_cast<size_t>(deck - 1)]);
        mapping.hotcueShiftStatus[static_cast<size_t>(deck - 1)] =
            parseByteAttr(QStringLiteral("shiftStatus"), mapping.hotcueShiftStatus[static_cast<size_t>(deck - 1)]);
        return;
    }

    if (elementName.compare(QStringLiteral("VuMeter"), Qt::CaseInsensitive) == 0) {
        const int deck = std::clamp(attrs.value(QStringLiteral("deck")).toInt(), 1, 4);
        mapping.vuStatus[static_cast<size_t>(deck - 1)] =
            parseByteAttr(QStringLiteral("status"), mapping.vuStatus[static_cast<size_t>(deck - 1)]);
        mapping.vuControl = parseByteAttr(QStringLiteral("control"), mapping.vuControl);
        return;
    }

    if (elementName.compare(QStringLiteral("Color"), Qt::CaseInsensitive) == 0) {
        const QString name = attrs.value(QStringLiteral("name")).toString().trimmed().toLower();
        const uint8_t value = parseByteAttr(QStringLiteral("value"), 0);
        if (name == QStringLiteral("blue"))
            mapping.padBlue = value;
        else if (name == QStringLiteral("cyan"))
            mapping.padCyan = value;
        else if (name == QStringLiteral("green"))
            mapping.padGreen = value;
        else if (name == QStringLiteral("yellow"))
            mapping.padYellow = value;
        else if (name == QStringLiteral("orange"))
            mapping.padOrange = value;
        else if (name == QStringLiteral("red"))
            mapping.padRed = value;
        else if (name == QStringLiteral("pink"))
            mapping.padPink = value;
        else if (name == QStringLiteral("magenta"))
            mapping.padMagenta = value;
        else if (name == QStringLiteral("purple"))
            mapping.padPurple = value;
        else if (name == QStringLiteral("white"))
            mapping.padWhite = value;
    }
}

void MidiControllerManager::startMidiLearn(const QString& parameterId)
{
    bool hasLinuxAlsaMonitor = false;
#if defined(Q_OS_LINUX)
    hasLinuxAlsaMonitor = !m_alsaInputMonitors.empty();
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

void MidiControllerManager::learnMapping(int msgId)
{
    if (m_learnParameterId.isEmpty())
        return;

    // Keep the maps one-to-one. Re-learning a control must not leave an old
    // MIDI event still driving the same parameter, and stealing a MIDI event
    // must clear the previous parameter's reverse lookup.
    const auto oldParamForMidi = m_midiToParam.find(msgId);
    if (oldParamForMidi != m_midiToParam.end()) {
        m_paramToMidi.erase(oldParamForMidi->second.paramId);
        m_momentaryHeldByMsgId.erase(msgId);
    }

    const auto oldMidiForParam = m_paramToMidi.find(m_learnParameterId);
    if (oldMidiForParam != m_paramToMidi.end()) {
        m_midiToParam.erase(oldMidiForParam->second);
        m_momentaryHeldByMsgId.erase(oldMidiForParam->second);
    }

    const QString learnedParamId = m_learnParameterId;
    const MidiMappingEntry learnedEntry = midi::makeMappingEntry(learnedParamId);
    m_midiToParam[msgId] = learnedEntry;
    if (learnedEntry.interactionType == MidiInteractionType::Momentary)
        m_momentaryHeldByMsgId[msgId] = false;
    m_paramToMidi[learnedParamId] = msgId;
    m_learnParameterId.clear();
    m_isLearning = false;

    qDebug() << "[MIDI] Learned" << msgId << "->" << learnedParamId;
    saveNativeMapping();
    emit mappingUpdated();
}
