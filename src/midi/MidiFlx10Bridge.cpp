#include "MidiControllerManager.h"
#include "MidiControllerManagerInternal.h"

using namespace midi_internal;

#include "DjEngine.h"
#include "ParameterStore.h"
#include "SettingsManager.h"
#include "controllers/flx10/Flx10Constants.h"

#include <QDebug>
#include <QMetaObject>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <exception>

void MidiControllerManager::sendFlx10HotcuePaletteTest()
{
    bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        outputOpen = autoOpenFlx10MidiOutputIfNeeded();

    if (!outputOpen)
        return;

    m_midiFeedback.sendPaletteTest();
}

void MidiControllerManager::testFlx10LedOutput()
{
    bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        outputOpen = autoOpenFlx10MidiOutputIfNeeded();

    if (!outputOpen) {
        qWarning() << "[MIDI OUT] FLX10 raw LED test requested but no MIDI output is open";
        return;
    }

    m_midiFeedback.testRawLedOutput();
}
bool MidiControllerManager::shouldUseFlx10Feedback() const
{
    if (normalizeControllerKeyFromXmlBase(getSelectedController())
            == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
        || midi_internal::isBuiltInFlx10Mapping(getSelectedMapping())) {
        return true;
    }

    const QString selectedOutput = SettingsManager::getInstance().getMidiOutputIdentifier();
    const juce::String selectedId = juce::String::fromUTF8(selectedOutput.toUtf8().constData());
    const int index = midi_internal::indexOfIdentifier(m_availableOutputDeviceIdentifiers, selectedId);
    if (index >= 0 && index < m_availableOutputDeviceNames.size())
        return midi_internal::looksLikeFlx10Name(m_availableOutputDeviceNames.at(index));

    return midi_internal::looksLikeFlx10Name(selectedOutput);
}

void MidiControllerManager::startFlx10OutputSession()
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen) {
        m_midiFeedback.setEnabled(false);
        return;
    }

    if (!shouldUseFlx10Feedback()) {
        if (m_midiFeedback.isEnabled())
            m_midiFeedback.clearAll();
        m_midiFeedback.setEnabled(false);
        return;
    }

    m_midiFeedback.setEnabled(true);
    if (!m_flx10RawLedTestRun && qEnvironmentVariableIntValue("BROCKDJ_FLX10_LED_TEST") != 0) {
        m_flx10RawLedTestRun = true;
        m_midiFeedback.testRawLedOutput();
    }
}

void MidiControllerManager::stopFlx10OutputSession()
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (outputOpen && m_midiFeedback.isEnabled())
        m_midiFeedback.clearAll();
    m_midiFeedback.setEnabled(false);
}
void MidiControllerManager::onParameterChanged(const QString& id, float value)
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        return;

    // Jog and button actions are input-only here. Echoing them as
    // LED feedback can come back through ALSA/PipeWire as a fresh input event
    // and toggle Play/Cue twice.
    const MidiInteractionType interactionType = midi_internal::defaultInteractionTypeForParam(id);
    if (midi_internal::isRelativeInteraction(interactionType) || midi_internal::isButtonInteraction(interactionType))
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
                                                  midi_internal::clampMidi7bit(static_cast<int>(value * 127.0f)));
    } else {
        if (value > 0.0f)
            msg = juce::MidiMessage::noteOn(channel, midi_internal::clampMidi7bit(subId), value);
        else
            msg = juce::MidiMessage::noteOff(channel, midi_internal::clampMidi7bit(subId), 0.0f);
    }

    sendMidiMessageWithDebug(msg, QStringLiteral("mapped-feedback"));
}

int MidiControllerManager::hotCueStatusForDeck(int deck) const
{
    switch (std::clamp(deck, 1, 4)) {
    case 1: return 0x97;
    case 2: return 0x99;
    case 3: return 0x9B;
    case 4: return 0x9D;
    default: return 0x97;
    }
}

int MidiControllerManager::hotCueStatusForDeck(QChar deck) const
{
    return hotCueStatusForDeck(deck == QLatin1Char('A') ? 1 : 2);
}

bool MidiControllerManager::sendMidiShort(int statusNo, int controlNo, int value, const QString& messageType)
{
    const int status = statusNo & 0xff;
    const int control = midi_internal::clampMidi7bit(controlNo);
    const int dataValue = midi_internal::clampMidi7bit(value);
    const bool cacheable = messageType != QStringLiteral("raw-test")
        && messageType != QStringLiteral("pad-palette-test");
    const int cacheKey = (status << 8) | control;
    if (cacheable) {
        const auto it = m_lastMidiShortValues.find(cacheKey);
        if (it != m_lastMidiShortValues.end() && it->second == dataValue)
            return true;
    }

    const unsigned char bytes[3] = {
        static_cast<unsigned char>(status),
        static_cast<unsigned char>(control),
        static_cast<unsigned char>(dataValue)
    };
    const juce::MidiMessage msg(bytes, 3);
    const bool success = sendMidiMessageWithDebug(msg, messageType);
    if (success && cacheable)
        m_lastMidiShortValues[cacheKey] = dataValue;
    return success;
}

bool MidiControllerManager::sendMidiMessageWithDebug(const juce::MidiMessage& message, const QString& messageType)
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        return false;

    const auto* raw = message.getRawData();
    const int size = message.getRawDataSize();
    const int status = size > 0 ? static_cast<unsigned char>(raw[0]) : 0;
    const int data1 = size > 1 ? static_cast<unsigned char>(raw[1]) : 0;
    const int data2 = size > 2 ? static_cast<unsigned char>(raw[2]) : 0;
    const int channel = (status & 0x0f) + 1;

    QString type = messageType;
    if (type.isEmpty()) {
        const int high = status & 0xf0;
        if (high == 0x80 || high == 0x90)
            type = QStringLiteral("note");
        else if (high == 0xB0)
            type = QStringLiteral("cc");
        else if (high == 0xE0)
            type = QStringLiteral("pitch");
        else if (status == 0xF0)
            type = QStringLiteral("sysex");
        else
            type = QStringLiteral("raw");
    }

    const QString rawBytes = size == 3
        ? QStringLiteral("%1 %2 %3").arg(midi_internal::hexByte(status), midi_internal::hexByte(data1), midi_internal::hexByte(data2))
        : QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(raw), size).toHex(' ')).toUpper();
    const bool verboseMidiOut = qEnvironmentVariableIntValue("BROCKDJ_MIDI_OUT_LOG") > 0;
    const bool noisySuccess = type == QStringLiteral("vu-meter") || type == QStringLiteral("pad-led");
    const bool logSuccess = verboseMidiOut || !noisySuccess;

    try {
#if defined(Q_OS_LINUX)
        if (m_alsaMidiOutput && m_alsaMidiOutput->isOpen()) {
            if (size != 3) {
                qWarning().noquote()
                    << QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=error error=\"ALSA short output only supports 3-byte MIDI messages\"")
                           .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                           .arg(m_selectedMidiOutputIndex)
                           .arg(type)
                           .arg(channel)
                           .arg(rawBytes);
                return false;
            }

            QString errorMessage;
            const bool success = m_alsaMidiOutput->sendShort(
                static_cast<uint8_t>(status),
                static_cast<uint8_t>(data1),
                static_cast<uint8_t>(data2),
                &errorMessage);

            const QString logLine = QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=%6%7")
                .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                .arg(m_selectedMidiOutputIndex)
                .arg(type)
                .arg(channel)
                .arg(rawBytes)
                .arg(success ? QStringLiteral("success") : QStringLiteral("error"))
                .arg(success ? QString() : QStringLiteral(" error=\"%1\"").arg(errorMessage));
            if (success) {
                if (logSuccess)
                    qInfo().noquote() << logLine;
            } else {
                qWarning().noquote() << logLine;
            }
            return success;
        }
#endif

        m_midiOutput->sendMessageNow(message);
        if (logSuccess) {
            qInfo().noquote()
                << QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=success")
                       .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                       .arg(m_selectedMidiOutputIndex)
                       .arg(type)
                       .arg(channel)
                       .arg(rawBytes);
        }
        return true;
    } catch (const std::exception& e) {
        qWarning().noquote()
            << QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=error error=\"%6\"")
                   .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                   .arg(m_selectedMidiOutputIndex)
                   .arg(type)
                   .arg(channel)
                   .arg(rawBytes)
                   .arg(QString::fromUtf8(e.what()));
    } catch (...) {
        qWarning().noquote()
            << QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=error error=\"unknown\"")
                   .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                   .arg(m_selectedMidiOutputIndex)
                   .arg(type)
                   .arg(channel)
                   .arg(rawBytes);
    }

    return false;
}

void MidiControllerManager::sendMidiNoteLed(int statusNo, int noteNo, int value)
{
    sendMidiShort(statusNo, noteNo, value, QStringLiteral("note-led"));
}

void MidiControllerManager::sendMappedNoteLed(const QString& paramId, bool on, int onValue)
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        return;

    const auto it = m_paramToMidi.find(paramId);
    if (it == m_paramToMidi.end())
        return;

    int channel = 1;
    int subId = it->second;
    if (subId >= 10000) {
        const int remainder = subId - 10000;
        channel = (remainder / 2000) + 1;
        subId = remainder % 2000;
    }

    if (subId >= 1000)
        return;

    sendMidiShort(0x90 + (channel - 1), subId, on ? onValue : 0);
}

int MidiControllerManager::hotCueLedValueForColor(const QString& color) const
{
    struct NamedPaletteValue { const char* name; int value; double hue; };
    static constexpr NamedPaletteValue kFlx10PadPalette[] = {
        { "red", 0x29, 0.0 },
        { "orange", 0x25, 30.0 },
        { "yellow", 0x1D, 55.0 },
        { "green", 0x15, 120.0 },
        { "cyan", 0x11, 180.0 },
        { "blue", 0x01, 235.0 },
        { "purple", 0x3D, 280.0 },
        { "pink", 0x35, 325.0 },
    };

    QString hex = color.trimmed();
    if (hex.startsWith(QLatin1Char('#')))
        hex.remove(0, 1);

    if (hex.size() == 3) {
        QString expanded;
        expanded.reserve(6);
        for (const QChar c : hex) {
            expanded.append(c);
            expanded.append(c);
        }
        hex = expanded;
    }

    bool ok = false;
    const int rgb = hex.toInt(&ok, 16);
    if (!ok || hex.size() != 6)
        return kFlx10PadPalette[0].value;

    const double r = static_cast<double>((rgb >> 16) & 0xff) / 255.0;
    const double g = static_cast<double>((rgb >> 8) & 0xff) / 255.0;
    const double b = static_cast<double>(rgb & 0xff) / 255.0;
    const double maxC = std::max({ r, g, b });
    const double minC = std::min({ r, g, b });
    const double delta = maxC - minC;
    if (maxC <= 0.0 || delta <= 0.0001)
        return 0x7F;

    const double saturation = delta / maxC;
    if (saturation < 0.18)
        return 0x7F;

    double hue = 0.0;
    if (maxC == r)
        hue = 60.0 * std::fmod(((g - b) / delta), 6.0);
    else if (maxC == g)
        hue = 60.0 * (((b - r) / delta) + 2.0);
    else
        hue = 60.0 * (((r - g) / delta) + 4.0);
    if (hue < 0.0)
        hue += 360.0;

    int bestValue = kFlx10PadPalette[0].value;
    double bestDistance = 361.0;
    for (const auto& entry : kFlx10PadPalette) {
        double distance = std::abs(hue - entry.hue);
        distance = std::min(distance, 360.0 - distance);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestValue = entry.value;
        }
    }
    return bestValue;
}

void MidiControllerManager::refreshTransportAndLoopLeds(QChar deck, DjEngine* engine)
{
    if (!engine)
        return;

    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshDeckLeds(deck == QLatin1Char('A') ? 1 : 2);
        return;
    }

    const QString prefix = deck == QLatin1Char('A')
        ? QStringLiteral("deckA_")
        : QStringLiteral("deckB_");

    sendMappedNoteLed(prefix + QStringLiteral("play"), engine->isPlaying());
    sendMappedNoteLed(prefix + QStringLiteral("cue"), !engine->isPlaying());
    sendMappedNoteLed(prefix + QStringLiteral("headphone_cue"), engine->cueEnabled());
    const bool loopOutSet = engine->loopOutPosition() > engine->loopInPosition() + 0.001;
    const bool isFourBeatLoop = engine->loopActive() && std::abs(engine->loopLengthBeats() - 4.0) < 0.1;
    sendMappedNoteLed(prefix + QStringLiteral("loop_in"), engine->loopInSet());
    sendMappedNoteLed(prefix + QStringLiteral("loop_out"), loopOutSet);
    sendMappedNoteLed(prefix + QStringLiteral("loop_4beat"), isFourBeatLoop);
    sendMappedNoteLed(prefix + QStringLiteral("loop_reloop"), engine->loopActive());
    sendMappedNoteLed(prefix + QStringLiteral("beat_sync"), engine->syncEnabled());
    sendMappedNoteLed(prefix + QStringLiteral("key_sync"), engine->keylock());
    sendMappedNoteLed(prefix + QStringLiteral("keylock"), engine->keylock());
    sendMappedNoteLed(prefix + QStringLiteral("quantize"), engine->quantizeEnabled());
    sendMappedNoteLed(prefix + QStringLiteral("slip"), engine->slipActive());
}

void MidiControllerManager::refreshPadModeLeds(QChar deck)
{
    const QString prefix = deck == QLatin1Char('A')
        ? QStringLiteral("deckA_pad_mode_")
        : QStringLiteral("deckB_pad_mode_");
    const MidiPadMode mode = padModeForDeck(deck);
    sendMappedNoteLed(prefix + QStringLiteral("hotcue"), mode == MidiPadMode::HotCue);
    sendMappedNoteLed(prefix + QStringLiteral("padfx"), mode == MidiPadMode::PadFx);
    sendMappedNoteLed(prefix + QStringLiteral("beatjump"), mode == MidiPadMode::BeatJump);
}

void MidiControllerManager::refreshHotCueLeds(QChar deck, DjEngine* engine)
{
    if (!engine)
        return;

    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshHotcuePads(deck == QLatin1Char('A') ? 1 : 2);
        return;
    }

    const QVariantList hotCues = engine->hotCues();
    const QVariantList savedLoops = engine->savedLoops();
    const int status = hotCueStatusForDeck(deck);
    for (int i = 0; i < 8; ++i) {
        bool isSet = false;
        QString color;
        if (i < savedLoops.size()) {
            const QVariantMap loopCue = savedLoops.at(i).toMap();
            if (loopCue.value(QStringLiteral("set")).toBool()) {
                isSet = true;
                color = loopCue.value(QStringLiteral("color")).toString();
            }
        }
        if (!isSet && i < hotCues.size()) {
            const QVariantMap cue = hotCues.at(i).toMap();
            isSet = cue.value(QStringLiteral("set")).toBool();
            color = cue.value(QStringLiteral("color")).toString();
        }
        sendMidiNoteLed(status, i, isSet ? hotCueLedValueForColor(color) : 0);
    }
}

void MidiControllerManager::refreshPerformancePadLeds(QChar deck, DjEngine* engine)
{
    const MidiPadMode mode = padModeForDeck(deck);
    if (mode == MidiPadMode::HotCue) {
        refreshHotCueLeds(deck, engine);
        return;
    }

    const int status = hotCueStatusForDeck(deck);
    static constexpr int kPadFxColors[8] = { 0x11, 0x11, 0x15, 0x15, 0x25, 0x25, 0x29, 0x29 };
    static constexpr int kBeatJumpColors[8] = { 0x01, 0x01, 0x11, 0x11, 0x15, 0x15, 0x1D, 0x1D };
    const int* colors = mode == MidiPadMode::PadFx ? kPadFxColors : kBeatJumpColors;

    for (int i = 0; i < 8; ++i)
        sendMidiNoteLed(status, i, colors[i]);
}

void MidiControllerManager::refreshDeckLeds(QChar deck, DjEngine* engine)
{
    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshDeckLeds(deck == QLatin1Char('A') ? 1 : 2);
        refreshPadModeLeds(deck);
        refreshPerformancePadLeds(deck, engine);
        return;
    }

    refreshTransportAndLoopLeds(deck, engine);
    refreshPadModeLeds(deck);
    refreshPerformancePadLeds(deck, engine);
}

void MidiControllerManager::refreshAllDeckLeds()
{
    refreshDeckLeds(QLatin1Char('A'), m_deckA);
    refreshDeckLeds(QLatin1Char('B'), m_deckB);
}

MidiPadMode MidiControllerManager::padModeForDeck(QChar deck) const
{
    return deck == QLatin1Char('A') ? m_deckAPadMode : m_deckBPadMode;
}

void MidiControllerManager::setPadModeForDeck(QChar deck, MidiPadMode mode)
{
    if (deck == QLatin1Char('A'))
        m_deckAPadMode = mode;
    else
        m_deckBPadMode = mode;
    refreshPadModeLeds(deck);
    refreshPerformancePadLeds(deck, deck == QLatin1Char('A') ? m_deckA : m_deckB);
}

void MidiControllerManager::stopPadFxToggle(DjEngine* engine, int padIndex)
{
    if (!engine)
        return;

    switch (padIndex) {
    case 4: engine->stopEchoOut(); break;
    case 5: engine->stopBackspin(); break;
    case 6: engine->stopVinylBrake(); break;
    case 7: engine->stopRollOut(); break;
    default: break;
    }
}

void MidiControllerManager::clearPadFxState(QChar deck, DjEngine* engine)
{
    if (!engine)
        return;

    int& momentary = deck == QLatin1Char('A') ? m_deckAPadFxMomentary : m_deckBPadFxMomentary;
    int& toggle = deck == QLatin1Char('A') ? m_deckAPadFxToggle : m_deckBPadFxToggle;

    if (momentary >= 0) {
        engine->clearPadFx();
        momentary = -1;
    }

    if (toggle >= 0) {
        stopPadFxToggle(engine, toggle);
        toggle = -1;
    }
}

void MidiControllerManager::handlePerformancePad(QChar deck,
                                                 DjEngine* engine,
                                                 int padIndex,
                                                 bool pressed,
                                                 bool clearRequest)
{
    if (!engine || padIndex < 0 || padIndex >= 8)
        return;

    const MidiPadMode mode = padModeForDeck(deck);

    if (mode == MidiPadMode::HotCue) {
        if (!pressed)
            return;
        if (clearRequest)
            engine->clearCuePad(padIndex);
        else
            engine->triggerCuePad(padIndex);
        refreshHotCueLeds(deck, engine);
        return;
    }

    if (clearRequest)
        return;

    if (mode == MidiPadMode::BeatJump) {
        if (!pressed)
            return;
        static constexpr double kBeatJumpPads[8] = { -16.0, -8.0, -4.0, -2.0, 2.0, 4.0, 8.0, 16.0 };
        engine->beatJump(kBeatJumpPads[padIndex]);
        return;
    }

    int& momentary = deck == QLatin1Char('A') ? m_deckAPadFxMomentary : m_deckBPadFxMomentary;
    int& toggle = deck == QLatin1Char('A') ? m_deckAPadFxToggle : m_deckBPadFxToggle;

    if (padIndex < 4) {
        static const QString kMomentaryFx[4] = {
            QStringLiteral("Echo"),
            QStringLiteral("Flanger"),
            QStringLiteral("Reverb"),
            QStringLiteral("Roll")
        };

        if (pressed) {
            momentary = padIndex;
            engine->setPadFx(kMomentaryFx[padIndex], 1.0f);
        } else if (momentary == padIndex) {
            engine->clearPadFx();
            momentary = -1;
        }
        return;
    }

    if (!pressed)
        return;

    if (toggle == padIndex) {
        stopPadFxToggle(engine, padIndex);
        toggle = -1;
        return;
    }

    if (toggle >= 0)
        stopPadFxToggle(engine, toggle);

    switch (padIndex) {
    case 4: engine->startEchoOut(); break;
    case 5: engine->startBackspin(); break;
    case 6: engine->startVinylBrake(); break;
    case 7: engine->startRollOut(); break;
    default: break;
    }
    toggle = padIndex;
}

void MidiControllerManager::connectDecks(DjEngine* deckA, DjEngine* deckB)
{
    if (m_deckA)
        QObject::disconnect(m_deckA, nullptr, this, nullptr);
    if (m_deckB && m_deckB != m_deckA)
        QObject::disconnect(m_deckB, nullptr, this, nullptr);

    m_deckA = deckA;
    m_deckB = deckB;
    m_cueAHeld = false;
    m_cueBHeld = false;
    m_jogATouched = false;
    m_jogBTouched = false;
    m_jogAReleaseTimer.stop();
    m_jogBReleaseTimer.stop();
    m_jogAReleasedRecently = false;
    m_jogBReleasedRecently = false;
    m_deckAShiftHeld = false;
    m_deckBShiftHeld = false;
    m_deckAPadMode = MidiPadMode::HotCue;
    m_deckBPadMode = MidiPadMode::HotCue;
    m_deckAPadFxMomentary = -1;
    m_deckBPadFxMomentary = -1;
    m_deckAPadFxToggle = -1;
    m_deckBPadFxToggle = -1;
    m_deckAFxSlotsEnabled = { false, false, false };
    m_deckBFxSlotsEnabled = { false, false, false };
    m_beatFxActive = false;
    m_beatFxPosition = 1;
    m_beatFxTargetDeck = QLatin1Char('A');
    m_midiFeedback.setDecks(m_deckA, m_deckB);

    auto wireDeckLeds = [this](QChar deck, DjEngine* engine)
    {
        if (!engine)
            return;

        QObject::connect(engine, &DjEngine::trackLoaded,
                         this, [this] { refreshAllDeckLeds(); });
        QObject::connect(engine, &DjEngine::trackMetadataChanged,
                         this, [this] { refreshAllDeckLeds(); });
        QObject::connect(engine, &DjEngine::playingChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::loopChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::cueEnabledChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::tempoChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::syncChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::syncMasterChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::keylockChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::quantizeEnabledChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::slipChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::hotCuesChanged,
                         this, [this, deck, engine] { refreshHotCueLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::savedLoopsChanged,
                         this, [this, deck, engine] { refreshHotCueLeds(deck, engine); });
    };
    wireDeckLeds(QLatin1Char('A'), m_deckA);
    wireDeckLeds(QLatin1Char('B'), m_deckB);
    refreshAllDeckLeds();

    if (!m_parameterStore)
        return;

    if (m_deckActionsConnection)
        QObject::disconnect(m_deckActionsConnection);

    // Route ParameterStore events to deck actions.
    // Volume and crossfader are handled in QML via parameterStore directly.
    // Button convention: 127/1.0 = press/on, 0 = release/off.
    m_deckActionsConnection = QObject::connect(m_parameterStore, &ParameterStore::parameterChanged,
        this, [this](const QString& id, float value)
    {
        DjEngine* const a = m_deckA;
        DjEngine* const b = m_deckB;

        auto engineForDeck = [a, b](QChar deck) -> DjEngine*
        {
            return deck == QLatin1Char('A') ? a : b;
        };

        auto toggleFxSlot = [this, &engineForDeck](QChar deck, int slot)
        {
            DjEngine* const engine = engineForDeck(deck);
            if (!engine || slot < 1 || slot > 3)
                return;

            std::array<bool, 3>& fxSlotStates = deck == QLatin1Char('A')
                ? m_deckAFxSlotsEnabled
                : m_deckBFxSlotsEnabled;
            const int index = slot - 1;
            fxSlotStates[static_cast<size_t>(index)] = !fxSlotStates[static_cast<size_t>(index)];

            static constexpr EffectType kSlotTypes[3] = {
                EffectType::Echo,
                EffectType::Flanger,
                EffectType::Reverb
            };
            const bool enabled = fxSlotStates[static_cast<size_t>(index)];
            engine->setFxSlotEffectType(slot, enabled ? kSlotTypes[index] : EffectType::None);
            engine->setFxSlotWetDry(slot, enabled ? 1.0f : 0.0f);

            qDebug() << "[MIDI ACTION] action=FxSlot"
                     << "deck:" << deck
                     << "slot:" << slot
                     << "enabled:" << enabled
                     << "dispatch=toggleFxSlot";
        };

        auto handleBeatJumpParam = [&engineForDeck](const QString& paramId) -> bool
        {
            QChar deck;
            double beats = 0.0;
            if (midi_internal::parseDeckButtonParam(paramId, QStringLiteral("beatjump_backward"), deck)
                || midi_internal::parseDeckButtonParam(paramId, QStringLiteral("beatjump_4_backward"), deck)) {
                beats = -4.0;
            } else if (midi_internal::parseDeckButtonParam(paramId, QStringLiteral("beatjump_forward"), deck)
                       || midi_internal::parseDeckButtonParam(paramId, QStringLiteral("beatjump_4_forward"), deck)) {
                beats = 4.0;
            } else if (midi_internal::parseDeckButtonParam(paramId, QStringLiteral("beatjump_16_backward"), deck)) {
                beats = -16.0;
            } else if (midi_internal::parseDeckButtonParam(paramId, QStringLiteral("beatjump_16_forward"), deck)) {
                beats = 16.0;
            } else {
                return false;
            }

            if (DjEngine* const engine = engineForDeck(deck))
                engine->beatJump(beats);
            qDebug() << "[MIDI ACTION] action=BeatJump"
                     << "deck:" << deck
                     << "beats:" << beats
                     << "dispatch=beatJump";
            return true;
        };

        auto applyBeatFx = [this, a, b]()
        {
            const EffectType type = midi_internal::beatFxTypeForPosition(m_beatFxPosition);
            const float wet = m_beatFxActive ? 1.0f : 0.0f;

            if (a) {
                a->setFxSlotEffectType(1, type);
                a->setFxSlotWetDry(1, m_beatFxTargetDeck == QLatin1Char('A') ? wet : 0.0f);
            }
            if (b) {
                b->setFxSlotEffectType(1, type);
                b->setFxSlotWetDry(1, m_beatFxTargetDeck == QLatin1Char('B') ? wet : 0.0f);
            }

            qDebug() << "[MIDI ACTION] action=BeatFx"
                     << "position:" << m_beatFxPosition
                     << "targetDeck:" << m_beatFxTargetDeck
                     << "active:" << m_beatFxActive
                     << "dispatch=applyBeatFxSlot1";
        };

        if (id == "deckA_play") {
            if (value >= 0.5f && a)
                a->togglePlay();
        }
        else if (id == "deckB_play") {
            if (value >= 0.5f && b)
                b->togglePlay();
        }
        else if (id == "deckA_cue") {
            const bool currentHeld = value >= 0.5f;
            const bool previousHeld = m_cueAHeld;
            if (currentHeld) {
                if (!previousHeld && a) {
                    m_cueAHeld = true;
                    qDebug() << "[MIDI ACTION] action=TransportCue deck=A"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=cueButtonPress";
                    a->cueButtonPress();
                } else {
                    qDebug() << "[MIDI ACTION] action=TransportCue deck=A"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=ignored-repeat-press";
                }
            } else {
                m_cueAHeld = false;
                qDebug() << "[MIDI ACTION] action=TransportCue deck=A"
                         << "previous:" << previousHeld
                         << "current:" << currentHeld
                         << "dispatch=cueButtonRelease";
                if (a)
                    a->cueButtonRelease();
            }
        }
        else if (id == "deckB_cue") {
            const bool currentHeld = value >= 0.5f;
            const bool previousHeld = m_cueBHeld;
            if (currentHeld) {
                if (!previousHeld && b) {
                    m_cueBHeld = true;
                    qDebug() << "[MIDI ACTION] action=TransportCue deck=B"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=cueButtonPress";
                    b->cueButtonPress();
                } else {
                    qDebug() << "[MIDI ACTION] action=TransportCue deck=B"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=ignored-repeat-press";
                }
            } else {
                m_cueBHeld = false;
                qDebug() << "[MIDI ACTION] action=TransportCue deck=B"
                         << "previous:" << previousHeld
                         << "current:" << currentHeld
                         << "dispatch=cueButtonRelease";
                if (b)
                    b->cueButtonRelease();
            }
        }
        else if (midi_internal::isHotCueParam(id)) {
            if (value < 0.5f)
                return;

            QChar deck;
            int hotCueIndex = -1;
            bool clear = false;
            if (!midi_internal::parseHotCueParam(id, deck, hotCueIndex, clear))
                return;

            DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
            if (!deckEngine)
                return;

            if (clear)
                deckEngine->clearCuePad(hotCueIndex);
            else
                deckEngine->triggerCuePad(hotCueIndex);
        }
        else {
            QChar deck;
            MidiPadMode mode = MidiPadMode::HotCue;
            int padIndex = -1;
            bool clearPad = false;

            if (midi_internal::parsePadModeParam(id, deck, mode)) {
                if (value < 0.5f)
                    return;

                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                if (padModeForDeck(deck) == MidiPadMode::PadFx && mode != MidiPadMode::PadFx)
                    clearPadFxState(deck, deckEngine);
                setPadModeForDeck(deck, mode);
                qDebug() << "[MIDI ACTION] action=PadMode"
                         << "deck:" << deck
                         << "mode:" << id
                         << "dispatch=setPadMode";
                return;
            }

            if (midi_internal::parsePerformancePadParam(id, deck, padIndex, clearPad)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                handlePerformancePad(deck, deckEngine, padIndex, value >= 0.5f, clearPad);
                return;
            }

            if (midi_internal::parseDirectPadParam(id, QStringLiteral("padfx_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                handlePerformancePad(deck, deckEngine, padIndex, value >= 0.5f, false);
                return;
            }

            if (midi_internal::parseDirectPadParam(id, QStringLiteral("beatjump_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                handlePerformancePad(deck, deckEngine, padIndex, value >= 0.5f, false);
                return;
            }

            int fxSlot = -1;
            if (midi_internal::parseDeckFxSlotParam(id, deck, fxSlot)) {
                if (value >= 0.5f)
                    toggleFxSlot(deck, fxSlot);
                return;
            }
        }
        QChar directDeck;
        if (value >= 0.5f && handleBeatJumpParam(id))
            return;
        int beatFxPosition = -1;
        if (midi_internal::parseBeatFxSelectParam(id, beatFxPosition)) {
            if (value >= 0.5f) {
                m_beatFxPosition = beatFxPosition;
                applyBeatFx();
            }
            return;
        }
        if (id == QStringLiteral("beat_fx_channel_deck_a")) {
            if (value >= 0.5f) {
                m_beatFxTargetDeck = QLatin1Char('A');
                applyBeatFx();
            }
            return;
        }
        if (id == QStringLiteral("beat_fx_channel_deck_b")) {
            if (value >= 0.5f) {
                m_beatFxTargetDeck = QLatin1Char('B');
                applyBeatFx();
            }
            return;
        }
        if (id == QStringLiteral("beat_fx_on")) {
            if (value >= 0.5f) {
                m_beatFxActive = !m_beatFxActive;
                applyBeatFx();
            }
            return;
        }
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("beat_sync"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setSyncEnabled(!engine->syncEnabled());
            }
            return;
        }
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("beatsync"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck)) {
                    if (engine->syncEnabled())
                        engine->reSync();
                    else
                        engine->setSyncEnabled(true);
                }
            }
            return;
        }
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("key_sync"), directDeck)
            || midi_internal::parseDeckButtonParam(id, QStringLiteral("keylock"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setKeylock(!engine->keylock());
            }
            return;
        }
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("slip"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setSlip(!engine->slipActive());
            }
            return;
        }
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("tempo_reset"), directDeck)
            || midi_internal::parseDeckButtonParam(id, QStringLiteral("rate_reset"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setTempoPercent(0.0);
            }
            return;
        }
        if (id == "deckA_shift") {
            m_deckAShiftHeld = value >= 0.5f;
        }
        else if (id == "deckB_shift") {
            m_deckBShiftHeld = value >= 0.5f;
        }
        else if (id == "deckA_loop_in") {
            if (value >= 0.5f && a) {
                if (a->loopActive()) a->halveLoopLength();
                else a->setLoopIn();
            }
        }
        else if (id == "deckB_loop_in") {
            if (value >= 0.5f && b) {
                if (b->loopActive()) b->halveLoopLength();
                else b->setLoopIn();
            }
        }
        else if (id == "deckA_loop_out") {
            if (value >= 0.5f && a) {
                if (a->loopActive()) a->doubleLoopLength();
                else a->setLoopOut();
            }
        }
        else if (id == "deckB_loop_out") {
            if (value >= 0.5f && b) {
                if (b->loopActive()) b->doubleLoopLength();
                else b->setLoopOut();
            }
        }
        else if (id == "deckA_loop_in_adjust") {
            if (value >= 0.5f && a)
                a->halveLoopLength();
        }
        else if (id == "deckB_loop_in_adjust") {
            if (value >= 0.5f && b)
                b->halveLoopLength();
        }
        else if (id == "deckA_loop_out_adjust") {
            if (value >= 0.5f && a)
                a->doubleLoopLength();
        }
        else if (id == "deckB_loop_out_adjust") {
            if (value >= 0.5f && b)
                b->doubleLoopLength();
        }
        else if (id == "deckA_loop_reloop") {
            if (value >= 0.5f && a) {
                if (a->loopActive())
                    a->deactivateLoop();
                else if (m_deckAShiftHeld)
                    a->reactivateLoop();
            }
        }
        else if (id == "deckB_loop_reloop") {
            if (value >= 0.5f && b) {
                if (b->loopActive())
                    b->deactivateLoop();
                else if (m_deckBShiftHeld)
                    b->reactivateLoop();
            }
        }
        else if (id == "deckA_loop_4beat") {
            if (value >= 0.5f && a) {
                if (m_deckAShiftHeld) {
                    if (a->loopActive())
                        a->deactivateLoop();
                    else
                        a->reactivateLoop();
                } else {
                    a->setLoop4Beats();
                }
            }
        }
        else if (id == "deckB_loop_4beat") {
            if (value >= 0.5f && b) {
                if (m_deckBShiftHeld) {
                    if (b->loopActive())
                        b->deactivateLoop();
                    else
                        b->reactivateLoop();
                } else {
                    b->setLoop4Beats();
                }
            }
        }
        else if (id == "deckA_headphone_cue") {
            if (value >= 0.5f && a) a->setCueEnabled(!a->cueEnabled());
        }
        else if (id == "deckB_headphone_cue") {
            if (value >= 0.5f && b) b->setCueEnabled(!b->cueEnabled());
        }
        else if (id == "master_cue") {
            if (value >= 0.5f && a) a->setMasterCueEnabled(!a->masterCueEnabled());
        }
        else if (id == "headphone_mix") {
            if (a) a->setHeadphoneMix(static_cast<double>(value));
        }
        // Trim/EQ/filter: MixerParameterBridge applies these for all four decks.
        // Tempo fader: MIDI 0-1 -> current deck tempo range.
        else if (id == "deckA_tempo")  {
            if (a) {
                const double range = std::max(1.0, a->tempoRangePercent());
                a->setTempoPercent(static_cast<double>(value) * 2.0 * range - range);
            }
        }
        else if (id == "deckB_tempo")  {
            if (b) {
                const double range = std::max(1.0, b->tempoRangePercent());
                b->setTempoPercent(static_cast<double>(value) * 2.0 * range - range);
            }
        }
        // Jog touch: 127 = finger down (enter scratch), 0 = lift (resume)
        else if (id == "deckA_jog_touch") {
            const bool currentHeld = value >= 0.5f;
            const bool previousHeld = m_jogATouched;
            if (currentHeld) {
                if (!previousHeld && a) {
                    m_jogATouched = true;
                    m_jogAReleasedRecently = false;
                    m_jogAReleaseTimer.stop();
                    m_scratchAbsoluteLastByMsgId.clear();
                    qDebug() << "[MIDI ACTION] action=JogTouch deck=A"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=pauseForScrub";
                    a->pauseForScrub();
                } else {
                    qDebug() << "[MIDI ACTION] action=JogTouch deck=A"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=ignored-repeat-press";
                }
            } else {
                m_jogATouched = false;
                m_scratchAbsoluteLastByMsgId.clear();
                qDebug() << "[MIDI ACTION] action=JogTouch deck=A"
                         << "previous:" << previousHeld
                         << "current:" << currentHeld
                         << "dispatch=resume-scrub-open-real-tick-window";
                if (a && a->isScrubbing())
                    a->resumeAfterScrub();
                m_jogAReleasedRecently = true;
                m_jogAReleaseTimer.start();
            }
        }
        else if (id == "deckB_jog_touch") {
            const bool currentHeld = value >= 0.5f;
            const bool previousHeld = m_jogBTouched;
            if (currentHeld) {
                if (!previousHeld && b) {
                    m_jogBTouched = true;
                    m_jogBReleasedRecently = false;
                    m_jogBReleaseTimer.stop();
                    m_scratchAbsoluteLastByMsgId.clear();
                    qDebug() << "[MIDI ACTION] action=JogTouch deck=B"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=pauseForScrub";
                    b->pauseForScrub();
                } else {
                    qDebug() << "[MIDI ACTION] action=JogTouch deck=B"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=ignored-repeat-press";
                }
            } else {
                m_jogBTouched = false;
                m_scratchAbsoluteLastByMsgId.clear();
                qDebug() << "[MIDI ACTION] action=JogTouch deck=B"
                         << "previous:" << previousHeld
                         << "current:" << currentHeld
                         << "dispatch=resume-scrub-open-real-tick-window";
                if (b && b->isScrubbing())
                    b->resumeAfterScrub();
                m_jogBReleasedRecently = true;
                m_jogBReleaseTimer.start();
            }
        }
        // Jog wheels mirror the Mixxx FLX10 mapping: CC 0x22 is the touched
        // top-platter stream; CC 0x21 is rim / post-release free spin. While the
        // finger is down only CC 0x22 is used for scratch — routing both would
        // double the travel and spin the UI handle too fast.
        else if (id == "deckA_jog_nudge") {
            if (a) {
                if (m_jogATouched)
                    return;
                if (a->isScratchReleaseActive()) {
                    if (std::abs(value) > 0.0f) {
                        m_jogAReleaseTimer.start();
                        a->applyScratchReleaseJog(midi_internal::flx10ScratchDeltaSec(static_cast<double>(value)));
                    }
                } else if (m_jogAReleasedRecently || a->isScrubbing()) {
                    if (!a->isScrubbing() && std::abs(value) <= 0.0f)
                        return;
                    if (!a->isScrubbing())
                        a->pauseForScrub();
                    if (std::abs(value) > 0.0f)
                        m_jogAReleaseTimer.start();
                    a->scratchBySeconds(midi_internal::flx10ScratchDeltaSec(static_cast<double>(value)), true);
                } else {
                    a->applyJogNudge(static_cast<double>(value));
                }
            }
        }
        else if (id == "deckB_jog_nudge") {
            if (b) {
                if (m_jogBTouched)
                    return;
                if (b->isScratchReleaseActive()) {
                    if (std::abs(value) > 0.0f) {
                        m_jogBReleaseTimer.start();
                        b->applyScratchReleaseJog(midi_internal::flx10ScratchDeltaSec(static_cast<double>(value)));
                    }
                } else if (m_jogBReleasedRecently || b->isScrubbing()) {
                    if (!b->isScrubbing() && std::abs(value) <= 0.0f)
                        return;
                    if (!b->isScrubbing())
                        b->pauseForScrub();
                    if (std::abs(value) > 0.0f)
                        m_jogBReleaseTimer.start();
                    b->scratchBySeconds(midi_internal::flx10ScratchDeltaSec(static_cast<double>(value)), true);
                } else {
                    b->applyJogNudge(static_cast<double>(value));
                }
            }
        }
        else if (id == "deckA_jog_scratch") {
            if (a && std::abs(value) > 0.0f) {
                // CC 0x22 is the vinyl-on platter stream; engage scratch even if the
                // touch Note arrived a tick later than the first relative CC.
                if (!m_jogATouched) {
                    m_jogATouched = true;
                    m_jogAReleasedRecently = false;
                    m_jogAReleaseTimer.stop();
                    m_scratchAbsoluteLastByMsgId.clear();
                }
                if (!a->isScrubbing())
                    a->pauseForScrub();
                a->scratchBySeconds(midi_internal::flx10ScratchDeltaSec(static_cast<double>(value)), true);
            }
        }
        else if (id == "deckB_jog_scratch") {
            if (b && std::abs(value) > 0.0f) {
                if (!m_jogBTouched) {
                    m_jogBTouched = true;
                    m_jogBReleasedRecently = false;
                    m_jogBReleaseTimer.stop();
                    m_scratchAbsoluteLastByMsgId.clear();
                }
                if (!b->isScrubbing())
                    b->pauseForScrub();
                b->scratchBySeconds(midi_internal::flx10ScratchDeltaSec(static_cast<double>(value)), true);
            }
        }
        else if (id == "deckA_jog_move") {
            const double delta = midi_internal::flx10ScratchDeltaSec(static_cast<double>(value));
            if (a) {
                if (m_jogATouched || a->isScrubbing()) {
                    if (!a->isScrubbing())
                        a->pauseForScrub();
                    qDebug() << "[MIDI ACTION] action=JogMove deck=A"
                             << "ticks:" << value
                             << "deltaSec:" << delta
                             << "touched:" << m_jogATouched
                             << "scrubbing:" << a->isScrubbing()
                             << "dispatch=scratchBySeconds";
                    a->scratchBySeconds(delta, true);
                } else {
                    qDebug() << "[MIDI ACTION] action=JogMove deck=A"
                             << "ticks:" << value
                             << "touched:" << m_jogATouched
                             << "scrubbing:" << a->isScrubbing()
                             << "dispatch=applyJogNudge";
                    a->applyJogNudge(static_cast<double>(value));
                }
            }
        }
        else if (id == "deckB_jog_move") {
            const double delta = midi_internal::flx10ScratchDeltaSec(static_cast<double>(value));
            if (b) {
                if (m_jogBTouched || b->isScrubbing()) {
                    if (!b->isScrubbing())
                        b->pauseForScrub();
                    qDebug() << "[MIDI ACTION] action=JogMove deck=B"
                             << "ticks:" << value
                             << "deltaSec:" << delta
                             << "touched:" << m_jogBTouched
                             << "scrubbing:" << b->isScrubbing()
                             << "dispatch=scratchBySeconds";
                    b->scratchBySeconds(delta, true);
                } else {
                    qDebug() << "[MIDI ACTION] action=JogMove deck=B"
                             << "ticks:" << value
                             << "touched:" << m_jogBTouched
                             << "scrubbing:" << b->isScrubbing()
                             << "dispatch=applyJogNudge";
                    b->applyJogNudge(static_cast<double>(value));
                }
            }
        }
    });
}
