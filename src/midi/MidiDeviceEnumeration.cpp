#include "MidiControllerManager.h"
#include "MidiControllerManagerInternal.h"

using namespace midi_internal;

#include "SettingsManager.h"

#include <QDebug>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include <alsa/asoundlib.h>
#endif

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
    return m_availableControllerDeviceNames;
}

void MidiControllerManager::refreshMidiDeviceCache()
{
    m_availableInputDeviceIdentifiers.clear();
    m_availableInputDeviceNames.clear();

    m_availableInputDeviceIdentifiers.push_back(kAllMidiInputsIdentifier);
    m_availableInputDeviceNames.push_back(QStringLiteral("All MIDI Inputs"));

    midi_internal::appendMidiDeviceNames(juce::MidiInput::getAvailableDevices(),
                          m_availableInputDeviceIdentifiers,
                          m_availableInputDeviceNames);

    m_availableOutputDeviceIdentifiers.clear();
    m_availableOutputDeviceNames.clear();

    midi_internal::appendMidiDeviceNames(juce::MidiOutput::getAvailableDevices(),
                          m_availableOutputDeviceIdentifiers,
                          m_availableOutputDeviceNames);

#if defined(Q_OS_LINUX)
    // JUCE does not reliably expose every ALSA sequencer port for larger DJ
    // controllers. Keep the direct ALSA ports visible too, so devices like the
    // DDJ-FLX10 can be selected by the port that actually emits button events.
    populateFromAlsaFallback();
#endif

    rebuildControllerDeviceCache();

    logAvailableMidiPorts();
}

void MidiControllerManager::rebuildControllerDeviceCache()
{
    m_availableControllerDeviceIdentifiers.clear();
    m_availableControllerInputIndexes.clear();
    m_availableControllerDeviceNames.clear();

    bool flx10Added = false;
    for (int i = 0; i < static_cast<int>(m_availableInputDeviceIdentifiers.size()); ++i) {
        const juce::String& identifier = m_availableInputDeviceIdentifiers[static_cast<size_t>(i)];
        if (identifier == kAllMidiInputsIdentifier)
            continue;

        const QString name = m_availableInputDeviceNames.value(i);
        if (midi_internal::looksLikeFlx10Name(name)) {
            if (flx10Added)
                continue;

            int preferredInput = i;
            for (int candidate = 0; candidate < static_cast<int>(m_availableInputDeviceIdentifiers.size()); ++candidate) {
                if (isPseudoAlsaIdentifier(m_availableInputDeviceIdentifiers[static_cast<size_t>(candidate)])
                        && midi_internal::looksLikeFlx10Name(m_availableInputDeviceNames.value(candidate))) {
                    preferredInput = candidate;
                    break;
                }
            }

            m_availableControllerDeviceIdentifiers.push_back(m_availableInputDeviceIdentifiers[static_cast<size_t>(preferredInput)]);
            m_availableControllerInputIndexes.push_back(preferredInput);
            m_availableControllerDeviceNames.push_back(QStringLiteral("DDJ-FLX10 (auto MIDI I/O)"));
            flx10Added = true;
            continue;
        }

        m_availableControllerDeviceIdentifiers.push_back(identifier);
        m_availableControllerInputIndexes.push_back(i);
        m_availableControllerDeviceNames.push_back(name);
    }
}

void MidiControllerManager::populateFromAlsaFallback()
{
#if defined(Q_OS_LINUX)
    snd_seq_t* seq = nullptr;
    const int openResult = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (openResult < 0) {
        qWarning() << "[MIDI] ALSA fallback failed:" << QString::fromUtf8(snd_strerror(openResult));
        return;
    }

    snd_seq_client_info_t* clientInfo = nullptr;
    snd_seq_port_info_t* portInfo = nullptr;
    snd_seq_client_info_alloca(&clientInfo);
    snd_seq_port_info_alloca(&portInfo);

    snd_seq_client_info_set_client(clientInfo, -1);
    while (snd_seq_query_next_client(seq, clientInfo) >= 0) {
        const int client = snd_seq_client_info_get_client(clientInfo);
        const QString clientName = QString::fromUtf8(snd_seq_client_info_get_name(clientInfo)).trimmed();
        const QString lowerClient = clientName.toLower();

        if (lowerClient == "system"
            || lowerClient == "midi through"
            || lowerClient.startsWith("pipewire")
            || lowerClient.contains("brockdj")
            || lowerClient.contains("aseqdump"))
            continue;

        snd_seq_port_info_set_client(portInfo, client);
        snd_seq_port_info_set_port(portInfo, -1);
        while (snd_seq_query_next_port(seq, portInfo) >= 0) {
            const int port = snd_seq_port_info_get_port(portInfo);
            const QString portName = QString::fromUtf8(snd_seq_port_info_get_name(portInfo)).trimmed();
            const unsigned int caps = snd_seq_port_info_get_capability(portInfo);
            const QString label = QString("%1:%2 - %3 (%4)")
                                      .arg(client)
                                      .arg(port)
                                      .arg(portName.isEmpty() ? clientName : portName)
                                      .arg(clientName);

            const bool readable = (caps & SND_SEQ_PORT_CAP_READ) != 0
                && (caps & SND_SEQ_PORT_CAP_SUBS_READ) != 0;
            const bool writable = (caps & SND_SEQ_PORT_CAP_WRITE) != 0
                && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE) != 0;

            if (readable) {
                const juce::String inputIdentifier("alsa:" + juce::String(client) + ":" + juce::String(port));
                m_availableInputDeviceIdentifiers.push_back(inputIdentifier);
                m_availableInputDeviceNames.push_back(label);
            }

            if (writable) {
                const juce::String outputIdentifier("alsa-out:" + juce::String(client) + ":" + juce::String(port));
                m_availableOutputDeviceIdentifiers.push_back(outputIdentifier);
                m_availableOutputDeviceNames.push_back(label + QStringLiteral(" (ALSA)"));
            }
        }
    }

    snd_seq_close(seq);
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

bool MidiControllerManager::isPseudoAlsaOutputIdentifier(const juce::String& identifier) const
{
#if defined(Q_OS_LINUX)
    return identifier.startsWith("alsa-out:");
#else
    Q_UNUSED(identifier);
    return false;
#endif
}

void MidiControllerManager::startAlsaInputMonitor(const juce::String& pseudoIdentifier)
{
#if defined(Q_OS_LINUX)
    if (!isPseudoAlsaIdentifier(pseudoIdentifier))
        return;

    const QString id = midi_internal::toQString(pseudoIdentifier);
    const QStringList parts = id.split(':');
    if (parts.size() != 3) {
        qWarning() << "[MIDI] Invalid ALSA pseudo identifier:" << id;
        return;
    }

    const QString port = parts.at(1) + ":" + parts.at(2);
    auto monitor = std::make_unique<QProcess>(this);
    QProcess* process = monitor.get();
    m_alsaMonitorBuffers[process].clear();

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]()
    {
        if (!process)
            return;

        QString& buffer = m_alsaMonitorBuffers[process];
        buffer.append(QString::fromUtf8(process->readAllStandardOutput()));

        int newline = buffer.indexOf('\n');
        while (newline >= 0) {
            const QString line = buffer.left(newline).trimmed();
            buffer.remove(0, newline + 1);

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

                // aseqdump formats are not consistent across ALSA/PipeWire paths:
                // some report MIDI channels as 0-based, others as 1-based. Prefer
                // an exact mapped ID, then the 1-based-adjusted candidate where safe.
                auto resolveMsgId = [this](int channelAware, int oneBasedAdjusted, int legacy) -> int
                {
                    if (m_midiToParam.count(channelAware))
                        return channelAware;
                    if (oneBasedAdjusted >= 0 && m_midiToParam.count(oneBasedAdjusted))
                        return oneBasedAdjusted;
                    if (m_midiToParam.count(legacy))
                        return legacy;
                    return channelAware; // learning: store channel-aware
                };

                int ch = 0, a = 0, b = 0;

                if (line.contains("Control change", Qt::CaseInsensitive) && decodeTriple(ch, a, b)) {
                    const int cc              = midi_internal::clampMidi7bit(a);
                    const int chClamped       = std::max(0, std::min(15, ch));
                    const int channelAwareMsgId = 10000 + chClamped * 2000 + 1000 + cc;
                    const int oneBasedMsgId     = ch > 0 ? 10000 + std::min(15, ch - 1) * 2000 + 1000 + cc : -1;
                    const int legacyMsgId       = cc + 1000;
                    const int msgId             = resolveMsgId(channelAwareMsgId, oneBasedMsgId, legacyMsgId);
                    const auto it               = m_midiToParam.find(msgId);
                    float value;
                    if (it != m_midiToParam.end() && midi_internal::isRelativeInteraction(it->second.interactionType)) {
                        value = midi_internal::decodeRelativeCcValue(b, it->second.paramId);
                    } else {
                        value = midi_internal::clampMidi7bit(b) / 127.0f;
                    }
                    processDecodedMidiEvent(
                        msgId, value, false,
                        juce::Time::getMillisecondCounterHiRes() * 0.001);
                } else if (line.contains("Note on", Qt::CaseInsensitive) ||
                           line.contains("Note off", Qt::CaseInsensitive)) {
                    const bool isOff = line.contains("Note off", Qt::CaseInsensitive);

                    // --- Format A: "Note on 0, note 11, velocity 127"  ← common aseqdump ---
                    // Some ALSA drivers omit the velocity on NoteOff. Extract channel/note
                    // from the named fields first; the source port prefix also contains
                    // numbers, so a generic "last three numbers" fallback can swap note/vel.
                    static const QRegularExpression noteTextRx(
                        R"((?:Note\s+on|Note\s+off)\s+(\d+)\s*,\s*note\s+(\d+)(?:\s*,\s*velocity\s+(\d+))?)",
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
                        R"((?:Chan|Channel)\s+(\d+).*?Note\s+(?:on|off)[^\d]*(\d+)(?:.*?(?:velocity|vel)[^\d]*(\d+))?)",
                        QRegularExpression::CaseInsensitiveOption);

                    int ch0  = -1;
                    int note = -1;
                    int vel  = 0;

                    const auto nmText = noteTextRx.match(line);
                    if (nmText.hasMatch()) {
                        ch0  = std::max(0, std::min(15, nmText.captured(1).toInt()));
                        note = midi_internal::clampMidi7bit(nmText.captured(2).toInt());
                        vel  = nmText.captured(3).isEmpty() ? 0 : midi_internal::clampMidi7bit(nmText.captured(3).toInt());
                    } else {
                        const auto nmA = noteParenRx.match(line);
                        if (nmA.hasMatch()) {
                            ch0  = std::max(0, std::min(15, nmA.captured(1).toInt() - 1));
                            note = midi_internal::clampMidi7bit(nmA.captured(3).toInt());
                            vel  = midi_internal::clampMidi7bit(nmA.captured(4).toInt());
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
                                ch0 = std::max(0, std::min(15, nmB.captured(1).toInt() - 1));
                                note = midi_internal::clampMidi7bit(nmB.captured(2).toInt());
                                vel = nmB.captured(3).isEmpty() ? 0 : midi_internal::clampMidi7bit(nmB.captured(3).toInt());
                            } else if (decodeTriple(ch, a, b)) {
                                // Last resort only. It can be wrong for some aseqdump NoteOff
                                // lines because the source port prefix contributes numbers.
                                ch0  = std::max(0, std::min(15, ch));
                                note = midi_internal::clampMidi7bit(a);
                                vel  = midi_internal::clampMidi7bit(b);
                            }
                        }
                    }

                    if (ch0 >= 0 && note >= 0) {
                        const int channelAwareMsgId = 10000 + ch0 * 2000 + note;
                        // Note channels are controller-action dense on the FLX10. A one-based
                        // fallback can turn unmapped pad notes into Ch7 library-load buttons,
                        // so notes must be exact or legacy only.
                        const int msgId             = resolveMsgId(channelAwareMsgId, -1, note);
                        const bool zeroVelocity     = isOff || (vel == 0);
                        qDebug() << "[MIDI ALSA]" << (isOff ? "NoteOff" : "NoteOn")
                                 << "ch0:" << ch0 << "note:" << note << "vel:" << vel
                                 << "msgId:" << msgId
                                 << "raw:" << line;
                        processDecodedMidiEvent(
                            msgId, zeroVelocity ? 0.0f : vel / 127.0f, zeroVelocity,
                            juce::Time::getMillisecondCounterHiRes() * 0.001);
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
                        const int msgId = resolveMsgId(channelAwareMsgId, -1, 1500);
                        const float value = static_cast<float>(pbRaw + 8192) / 16383.0f;
                        processDecodedMidiEvent(
                            msgId, value, false,
                            juce::Time::getMillisecondCounterHiRes() * 0.001);
                    }
                }
            }

            newline = buffer.indexOf('\n');
        }
    });

    connect(process, &QProcess::errorOccurred, this, [port](QProcess::ProcessError error)
    {
        qWarning() << "[MIDI] aseqdump error on" << port << "error:" << static_cast<int>(error);
    });

    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, port](int exitCode, QProcess::ExitStatus status)
    {
        qWarning() << "[MIDI] aseqdump finished on" << port
                   << "exitCode:" << exitCode
                   << "status:" << static_cast<int>(status);
        m_alsaMonitorBuffers.erase(process);
    });

    connect(process, &QProcess::readyReadStandardError, this, [process]()
    {
        if (!process)
            return;
        const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (!err.isEmpty())
            qWarning() << "[MIDI] aseqdump stderr:" << err;
    });

    process->start("aseqdump", {"-p", port});
    if (!process->waitForStarted(1200)) {
        qWarning() << "[MIDI] Could not start aseqdump for" << port;
        m_alsaMonitorBuffers.erase(process);
        return;
    }

    qInfo() << "[MIDI] Started ALSA input monitor on" << port;
    m_alsaInputMonitors.push_back(std::move(monitor));

#else
    Q_UNUSED(pseudoIdentifier);
#endif
}

void MidiControllerManager::stopAlsaInputMonitor()
{
#if defined(Q_OS_LINUX)
    if (m_alsaInputMonitors.empty())
        return;

    for (auto& monitor : m_alsaInputMonitors) {
        if (!monitor)
            continue;
        if (monitor->state() != QProcess::NotRunning) {
            monitor->terminate();
            if (!monitor->waitForFinished(400))
                monitor->kill();
        }
    }

    m_alsaInputMonitors.clear();
    m_alsaMonitorBuffers.clear();
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
                qWarning() << "[MIDI] Failed to open input:" << midi_internal::toQString(dev.identifier);
                continue;
            }

            input->start();
            qDebug() << "[MIDI] Opened input:" << midi_internal::toQString(dev.name)
                     << "id:" << midi_internal::toQString(dev.identifier);
            m_midiInputs.push_back(std::move(input));
        }

        if (m_midiInputs.empty())
            qWarning() << "[MIDI] All MIDI Inputs selected, but no JUCE inputs could be opened";
        return;
    }

    if (isPseudoAlsaIdentifier(identifier)) {
        const int selectedIndex = midi_internal::indexOfIdentifier(m_availableInputDeviceIdentifiers, identifier);
        const QString selectedName = (selectedIndex >= 0 && selectedIndex < m_availableInputDeviceNames.size())
            ? m_availableInputDeviceNames.at(selectedIndex)
            : QString();
        const bool flx10Context = midi_internal::looksLikeFlx10Name(selectedName)
            || normalizeControllerKeyFromXmlBase(getSelectedController()) == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
            || midi_internal::isBuiltInFlx10Mapping(getSelectedMapping());

        if (flx10Context) {
            int started = 0;
            for (int i = 0; i < static_cast<int>(m_availableInputDeviceIdentifiers.size()); ++i) {
                if (!isPseudoAlsaIdentifier(m_availableInputDeviceIdentifiers[static_cast<size_t>(i)]))
                    continue;
                if (!midi_internal::looksLikeFlx10Name(m_availableInputDeviceNames.at(i)))
                    continue;

                startAlsaInputMonitor(m_availableInputDeviceIdentifiers[static_cast<size_t>(i)]);
                ++started;
            }

            if (started > 0) {
                qInfo() << "[MIDI] FLX10 ALSA input monitors active:" << started;
                return;
            }
        }

        startAlsaInputMonitor(identifier);
        return;
    }

    auto input = juce::MidiInput::openDevice(identifier, this);
    if (!input) {
        qWarning() << "[MIDI] Failed to open input:" << midi_internal::toQString(identifier);
        return;
    }

    input->start();
    m_midiInputs.push_back(std::move(input));
}

void MidiControllerManager::openMidiOutputByIdentifier(const juce::String& identifier)
{
    stopFlx10OutputSession();

    if (m_midiOutput)
        m_midiOutput.reset();
#if defined(Q_OS_LINUX)
    if (m_alsaMidiOutput)
        m_alsaMidiOutput.reset();
#endif

    m_selectedMidiOutputIdentifier = {};
    m_selectedMidiOutputName.clear();
    m_selectedMidiOutputIndex = -1;
    m_flx10RawLedTestRun = false;
    m_lastMidiShortValues.clear();

    if (identifier.isEmpty() || isPseudoAlsaIdentifier(identifier))
        return;

    const int outputIndex = midi_internal::indexOfIdentifier(m_availableOutputDeviceIdentifiers, identifier);
    const QString outputName = (outputIndex >= 0 && outputIndex < m_availableOutputDeviceNames.size())
        ? m_availableOutputDeviceNames.at(outputIndex)
        : midi_internal::toQString(identifier);

#if defined(Q_OS_LINUX)
    if (isPseudoAlsaOutputIdentifier(identifier)) {
        auto output = std::make_unique<AlsaMidiOutput>();
        QString errorMessage;
        if (!output->open(midi_internal::toQString(identifier), &errorMessage)) {
            qWarning() << "[MIDI OUT] Failed to open ALSA output:" << outputName
                       << "id:" << midi_internal::toQString(identifier)
                       << "index:" << outputIndex
                       << "error:" << errorMessage;
            return;
        }

        m_selectedMidiOutputIdentifier = identifier;
        m_selectedMidiOutputName = outputName;
        m_selectedMidiOutputIndex = outputIndex;
        m_alsaMidiOutput = std::move(output);
        qInfo().noquote() << QString("FLX10 MIDI OUT port: \"%1\", index=%2")
                                 .arg(m_selectedMidiOutputName)
                                 .arg(m_selectedMidiOutputIndex);
        startFlx10OutputSession();
        refreshAllDeckLeds();
        return;
    }
#endif

    auto output = juce::MidiOutput::openDevice(identifier);
    if (!output) {
        qWarning() << "[MIDI OUT] Failed to open output:" << outputName
                   << "id:" << midi_internal::toQString(identifier)
                   << "index:" << outputIndex;
        return;
    }

    m_selectedMidiOutputIdentifier = identifier;
    m_selectedMidiOutputName = outputName;
    m_selectedMidiOutputIndex = outputIndex;
    m_midiOutput = std::move(output);
    qInfo().noquote() << QString("FLX10 MIDI OUT port: \"%1\", index=%2")
                             .arg(m_selectedMidiOutputName)
                             .arg(m_selectedMidiOutputIndex);
    startFlx10OutputSession();
    refreshAllDeckLeds();
}

int MidiControllerManager::findMatchingMidiOutputIndexForInput(int inputIndex) const
{
    if (m_availableOutputDeviceIdentifiers.empty())
        return -1;

    const bool flx10Context = normalizeControllerKeyFromXmlBase(getSelectedController())
            == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
        || midi_internal::isBuiltInFlx10Mapping(getSelectedMapping());
    const QString inputName = (inputIndex >= 0 && inputIndex < m_availableInputDeviceNames.size())
        ? m_availableInputDeviceNames.at(inputIndex)
        : QString();
    const QString inputKey = midi_internal::midiMatchKey(inputName);

    if (flx10Context || midi_internal::looksLikeFlx10Name(inputName)) {
        for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
            if (midi_internal::looksLikeFlx10Name(m_availableOutputDeviceNames.at(i))
                    && isPseudoAlsaOutputIdentifier(m_availableOutputDeviceIdentifiers[static_cast<size_t>(i)]))
                return i;
        }

        for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
            if (midi_internal::looksLikeFlx10Name(m_availableOutputDeviceNames.at(i)))
                return i;
        }
    }

    if (!inputKey.isEmpty()) {
        for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
            const QString outputKey = midi_internal::midiMatchKey(m_availableOutputDeviceNames.at(i));
            if (!outputKey.isEmpty()
                    && (outputKey.contains(inputKey) || inputKey.contains(outputKey)))
                return i;
        }
    }

    const int ordinalOutputIndex = (inputIndex > 0) ? inputIndex - 1 : -1;
    if (ordinalOutputIndex >= 0
            && ordinalOutputIndex < static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
        return ordinalOutputIndex;

    return -1;
}

int MidiControllerManager::findMidiOutputIndexByName(const QString& nameOrIdentifier) const
{
    const QString needle = nameOrIdentifier.trimmed();
    if (needle.isEmpty())
        return -1;

    for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
        if (m_availableOutputDeviceNames.at(i).compare(needle, Qt::CaseInsensitive) == 0)
            return i;
    }

    for (int i = 0; i < static_cast<int>(m_availableOutputDeviceIdentifiers.size()); ++i) {
        if (midi_internal::toQString(m_availableOutputDeviceIdentifiers[static_cast<size_t>(i)]).compare(needle, Qt::CaseInsensitive) == 0)
            return i;
    }

    const QString needleKey = midi_internal::midiMatchKey(needle);
    if (needleKey.isEmpty())
        return -1;

    for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
        const QString outputKey = midi_internal::midiMatchKey(m_availableOutputDeviceNames.at(i));
        if (outputKey.contains(needleKey) || needleKey.contains(outputKey))
            return i;
    }

    return -1;
}

void MidiControllerManager::logAvailableMidiPorts() const
{
    qInfo() << "Available MIDI INPUT ports:";
    for (int i = 0; i < m_availableInputDeviceNames.size(); ++i) {
        const QString id = (i >= 0 && i < static_cast<int>(m_availableInputDeviceIdentifiers.size()))
            ? midi_internal::toQString(m_availableInputDeviceIdentifiers[static_cast<size_t>(i)])
            : QString();
        qInfo().noquote() << QString("[%1] %2  id=\"%3\"")
                                 .arg(i)
                                 .arg(m_availableInputDeviceNames.at(i))
                                 .arg(id);
    }

    qInfo() << "Available MIDI OUTPUT ports:";
    for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
        const QString id = (i >= 0 && i < static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
            ? midi_internal::toQString(m_availableOutputDeviceIdentifiers[static_cast<size_t>(i)])
            : QString();
        qInfo().noquote() << QString("[%1] %2  id=\"%3\"")
                                 .arg(i)
                                 .arg(m_availableOutputDeviceNames.at(i))
                                 .arg(id);
    }
}

void MidiControllerManager::restoreSavedDeviceSelections()
{
    const bool flx10Context = normalizeControllerKeyFromXmlBase(getSelectedController())
            == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
        || midi_internal::isBuiltInFlx10Mapping(getSelectedMapping());
    const auto inputId = SettingsManager::getInstance().getMidiInputIdentifier();
    juce::String savedInput = inputId.isEmpty()
        ? kAllMidiInputsIdentifier
        : juce::String::fromUTF8(inputId.toUtf8().constData());

    if (flx10Context && (inputId.isEmpty() || savedInput == kAllMidiInputsIdentifier)) {
        for (int i = 0; i < static_cast<int>(m_availableInputDeviceIdentifiers.size()); ++i) {
            const auto& identifier = m_availableInputDeviceIdentifiers[static_cast<size_t>(i)];
            if (isPseudoAlsaIdentifier(identifier) && midi_internal::looksLikeFlx10Name(m_availableInputDeviceNames.at(i))) {
                savedInput = identifier;
                SettingsManager::getInstance().setMidiInputIdentifier(midi_internal::toQString(identifier));
                qInfo() << "[MIDI] Auto-selected FLX10 ALSA input"
                        << m_availableInputDeviceNames.at(i)
                        << "index:" << i;
                break;
            }
        }
    }

    if (midi_internal::containsIdentifier(m_availableInputDeviceIdentifiers, savedInput))
        openMidiInputByIdentifier(savedInput);
    else
        openMidiInputByIdentifier(kAllMidiInputsIdentifier);

    const auto outputId = SettingsManager::getInstance().getMidiOutputIdentifier();
    const QByteArray envOutputName = qgetenv("BROCKDJ_MIDI_OUT");
    if (!envOutputName.isEmpty()) {
        const QString requestedOutput = QString::fromUtf8(envOutputName).trimmed();
        const int outputIndex = findMidiOutputIndexByName(requestedOutput);
        if (outputIndex >= 0 && outputIndex < static_cast<int>(m_availableOutputDeviceIdentifiers.size())) {
            const juce::String identifier = m_availableOutputDeviceIdentifiers[static_cast<size_t>(outputIndex)];
            SettingsManager::getInstance().setMidiOutputIdentifier(midi_internal::toQString(identifier));
            qInfo() << "[MIDI OUT] BROCKDJ_MIDI_OUT selected output" << requestedOutput
                    << "->" << m_availableOutputDeviceNames.at(outputIndex)
                    << "index:" << outputIndex;
            openMidiOutputByIdentifier(identifier);
            return;
        }

        qWarning() << "[MIDI OUT] BROCKDJ_MIDI_OUT did not match any output port:" << requestedOutput;
    }

    if (!outputId.isEmpty()) {
        const juce::String savedOutput = juce::String::fromUTF8(outputId.toUtf8().constData());
        if (midi_internal::containsIdentifier(m_availableOutputDeviceIdentifiers, savedOutput))
            openMidiOutputByIdentifier(savedOutput);
    }

    autoOpenFlx10MidiOutputIfNeeded();
}

bool MidiControllerManager::autoOpenFlx10MidiOutputIfNeeded()
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (outputOpen || m_availableOutputDeviceIdentifiers.empty())
        return false;

    const bool flx10Context = normalizeControllerKeyFromXmlBase(getSelectedController())
            == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
        || midi_internal::isBuiltInFlx10Mapping(getSelectedMapping());

    int outputIndex = -1;
    if (flx10Context) {
        for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
            if (midi_internal::looksLikeFlx10Name(m_availableOutputDeviceNames.at(i))
                    && isPseudoAlsaOutputIdentifier(m_availableOutputDeviceIdentifiers[static_cast<size_t>(i)])) {
                outputIndex = i;
                break;
            }
        }

        if (outputIndex < 0) {
            for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
                if (midi_internal::looksLikeFlx10Name(m_availableOutputDeviceNames.at(i))) {
                    outputIndex = i;
                    break;
                }
            }
        }
    }

    if (outputIndex < 0)
        return false;

    const juce::String identifier = m_availableOutputDeviceIdentifiers[static_cast<size_t>(outputIndex)];
    SettingsManager::getInstance().setMidiOutputIdentifier(midi_internal::toQString(identifier));
    qInfo() << "[MIDI OUT] Auto-selected FLX10 output"
            << m_availableOutputDeviceNames.at(outputIndex)
            << "index:" << outputIndex;
    openMidiOutputByIdentifier(identifier);
    return true;
}

void MidiControllerManager::selectMidiInputDevice(int index)
{
    if (index < 0 || index >= static_cast<int>(m_availableInputDeviceIdentifiers.size()))
        return;

    const auto identifier = m_availableInputDeviceIdentifiers[static_cast<size_t>(index)];
    openMidiInputByIdentifier(identifier);
    SettingsManager::getInstance().setMidiInputIdentifier(midi_internal::toQString(identifier));
}

void MidiControllerManager::selectMidiOutputDevice(int index)
{
    if (index < 0 || index >= static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
        return;

    const auto identifier = m_availableOutputDeviceIdentifiers[static_cast<size_t>(index)];
    SettingsManager::getInstance().setMidiOutputIdentifier(midi_internal::toQString(identifier));
    openMidiOutputByIdentifier(identifier);
}

void MidiControllerManager::selectMidiDevice(int index)
{
    if (index < 0 || index >= static_cast<int>(m_availableControllerInputIndexes.size()))
        return;

    const int inputIndex = m_availableControllerInputIndexes[static_cast<size_t>(index)];
    selectMidiInputDevice(inputIndex);

    const int outputIndex = findMatchingMidiOutputIndexForInput(inputIndex);
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
    return midi_internal::indexOfIdentifier(m_availableInputDeviceIdentifiers, selectedId);
}

int MidiControllerManager::getSelectedMidiOutputIndex() const
{
    const QString selected = SettingsManager::getInstance().getMidiOutputIdentifier();
    if (selected.isEmpty())
        return -1;

    const juce::String selectedId = juce::String::fromUTF8(selected.toUtf8().constData());
    return midi_internal::indexOfIdentifier(m_availableOutputDeviceIdentifiers, selectedId);
}

int MidiControllerManager::getSelectedMidiDeviceIndex() const
{
    const int selectedInputIndex = getSelectedMidiInputIndex();
    if (selectedInputIndex < 0)
        return -1;

    for (int i = 0; i < static_cast<int>(m_availableControllerInputIndexes.size()); ++i) {
        if (m_availableControllerInputIndexes[static_cast<size_t>(i)] == selectedInputIndex)
            return i;
    }

    const QString selectedInputName = m_availableInputDeviceNames.value(selectedInputIndex);
    if (midi_internal::looksLikeFlx10Name(selectedInputName)) {
        for (int i = 0; i < m_availableControllerDeviceNames.size(); ++i) {
            if (midi_internal::looksLikeFlx10Name(m_availableControllerDeviceNames.at(i)))
                return i;
        }
    }

    return -1;
}
