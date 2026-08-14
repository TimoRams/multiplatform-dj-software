#include "controllers/midi/MidiControllerManager.h"
#include "controllers/midi/MidiControllerManagerInternal.h"
#include "audio/AudioEngine.h"
#include "fx/FxManager.h"

using namespace midi_internal;

#include "deck/DjEngine.h"
#include "controllers/midi/ParameterStore.h"
#include "app/SettingsManager.h"
#include "controllers/flx10/Flx10JogRouter.h"

#include <QDebug>
#include <QMetaObject>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <exception>

namespace {

const char* jogEventName(flx10::JogEventType type) noexcept
{
    switch (type) {
    case flx10::JogEventType::TouchDown: return "touch-down";
    case flx10::JogEventType::TouchUp: return "touch-up";
    case flx10::JogEventType::Platter: return "platter-0x22";
    case flx10::JogEventType::Rim: return "rim-0x21";
    case flx10::JogEventType::Generic: return "generic";
    }
    return "unknown";
}

const char* jogPhaseName(flx10::JogPhase phase) noexcept
{
    switch (phase) {
    case flx10::JogPhase::Idle: return "idle";
    case flx10::JogPhase::TouchTracking: return "touch-tracking";
    case flx10::JogPhase::ReleaseOwned: return "release-owned";
    case flx10::JogPhase::TailSuppression: return "tail-suppression";
    }
    return "unknown";
}

const char* jogActionName(flx10::JogRouteAction action) noexcept
{
    switch (action) {
    case flx10::JogRouteAction::Ignore: return "ignore";
    case flx10::JogRouteAction::BeginScratch: return "begin-scratch";
    case flx10::JogRouteAction::RequestRelease: return "request-release";
    case flx10::JogRouteAction::ScratchDelta: return "scratch-delta";
    case flx10::JogRouteAction::ReleaseDelta: return "release-delta";
    case flx10::JogRouteAction::Nudge: return "nudge";
    }
    return "unknown";
}

struct CuePadInfo {
    bool set = false;
    double positionSeconds = 0.0;
    QString color;
};

CuePadInfo cuePadInfo(DjEngine* engine, int padIndex)
{
    CuePadInfo info;
    if (!engine || padIndex < 0 || padIndex >= 8)
        return info;

    const QVariantList savedLoops = engine->savedLoops();
    if (padIndex < savedLoops.size()) {
        const QVariantMap loop = savedLoops.at(padIndex).toMap();
        if (loop.value(QStringLiteral("set")).toBool()) {
            info.set = true;
            info.positionSeconds = loop.value(QStringLiteral("inSec")).toDouble();
            info.color = loop.value(QStringLiteral("color")).toString();
            return info;
        }
    }

    const QVariantList hotCues = engine->hotCues();
    if (padIndex < hotCues.size()) {
        const QVariantMap cue = hotCues.at(padIndex).toMap();
        if (cue.value(QStringLiteral("set")).toBool()) {
            info.set = true;
            info.positionSeconds = cue.value(QStringLiteral("positionSec")).toDouble();
            info.color = cue.value(QStringLiteral("color")).toString();
        }
    }
    return info;
}

bool parsePerformanceDeckId(const QString& deckId, QChar& deck)
{
    if (deckId.compare(QStringLiteral("deckA"), Qt::CaseInsensitive) == 0
        || deckId.compare(QStringLiteral("A"), Qt::CaseInsensitive) == 0) {
        deck = QLatin1Char('A');
        return true;
    }
    if (deckId.compare(QStringLiteral("deckB"), Qt::CaseInsensitive) == 0
        || deckId.compare(QStringLiteral("B"), Qt::CaseInsensitive) == 0) {
        deck = QLatin1Char('B');
        return true;
    }
    return false;
}

QString performanceDeckId(QChar deck)
{
    return deck == QLatin1Char('B') ? QStringLiteral("deckB") : QStringLiteral("deckA");
}

void cycleFlx10TempoRange(DjEngine* engine)
{
    if (!engine)
        return;

    constexpr std::array<double, 4> ranges { 6.0, 10.0, 16.0, 20.0 };
    const double current = engine->tempoRangePercent();
    const auto nearest = std::min_element(ranges.begin(), ranges.end(), [current](double lhs, double rhs)
    {
        return std::abs(lhs - current) < std::abs(rhs - current);
    });
    const auto next = std::next(nearest) == ranges.end() ? ranges.begin() : std::next(nearest);
    engine->setTempoRangePercent(*next);
    qInfo() << "[MIDI ACTION] action=TempoRange" << "range:" << *next;
}

} // namespace

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
    refreshFxLeds();
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
    if (midi_internal::isRelativeInteraction(interactionType)
        || midi_internal::isButtonInteraction(interactionType)
        || interactionType == MidiInteractionType::Fader
        || (shouldUseFlx10Feedback()
            && interactionType == MidiInteractionType::EncoderAbsolute))
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
        const int deckNumber = std::clamp(deck.toLatin1() - 'A' + 1, 1, 4);
        m_midiFeedback.refreshDeckLeds(deckNumber);
        return;
    }

    const QString prefix = QStringLiteral("deck%1_").arg(deck.toUpper());

    sendMappedNoteLed(prefix + QStringLiteral("play"), engine->isPlaying());
    sendMappedNoteLed(prefix + QStringLiteral("cue"),
                      engine->mainCueSec() >= -DjEngine::PRE_ROLL_SECONDS);
    sendMappedNoteLed(prefix + QStringLiteral("headphone_cue"), engine->cueEnabled());
    const bool loopOutSet = engine->loopOutPosition() > engine->loopInPosition() + 0.001;
    sendMappedNoteLed(prefix + QStringLiteral("loop_in"), engine->loopInSet());
    sendMappedNoteLed(prefix + QStringLiteral("loop_out"), loopOutSet);
    sendMappedNoteLed(prefix + QStringLiteral("loop_4beat"), engine->loopActive());
    sendMappedNoteLed(prefix + QStringLiteral("loop_reloop"), engine->loopActive());
    sendMappedNoteLed(prefix + QStringLiteral("beat_sync"), engine->syncEnabled());
    sendMappedNoteLed(prefix + QStringLiteral("key_sync"), engine->keylock());
    sendMappedNoteLed(prefix + QStringLiteral("keylock"), engine->keylock());
    sendMappedNoteLed(prefix + QStringLiteral("quantize"), engine->quantizeEnabled());
    sendMappedNoteLed(prefix + QStringLiteral("slip"), engine->slipActive());
    sendMappedNoteLed(prefix + QStringLiteral("slip_reverse"), engine->isReverse());
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
    sendMappedNoteLed(prefix + QStringLiteral("sampler"), mode == MidiPadMode::Sampler);
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
    refreshHotCueLeds(deck, engine);

    const int status = hotCueStatusForDeck(deck);
    static constexpr int kPadFxColors[8] = { 0x11, 0x11, 0x15, 0x15, 0x25, 0x25, 0x29, 0x29 };
    static constexpr int kBeatJumpColors[8] = { 0x01, 0x01, 0x11, 0x11, 0x15, 0x15, 0x1D, 0x1D };
    for (int i = 0; i < 8; ++i) {
        sendMidiNoteLed(status, 0x10 + i, kPadFxColors[i]);
        sendMidiNoteLed(status, 0x20 + i, kBeatJumpColors[i]);

        const CuePadInfo sample = cuePadInfo(engine, i);
        sendMidiNoteLed(status, 0x30 + i,
                        sample.set ? hotCueLedValueForColor(sample.color) : 0);
    }
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
    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshAll();
        refreshFxLeds();
        return;
    }
    refreshDeckLeds(QLatin1Char('A'), m_deckA);
    refreshDeckLeds(QLatin1Char('B'), m_deckB);
    refreshFxLeds();
}

MidiPadMode MidiControllerManager::padModeForDeck(QChar deck) const
{
    return deck == QLatin1Char('A') ? m_deckAPadMode : m_deckBPadMode;
}

void MidiControllerManager::setPadModeForDeck(QChar deck, MidiPadMode mode)
{
    MidiPadMode& current = deck == QLatin1Char('A') ? m_deckAPadMode : m_deckBPadMode;
    const bool changed = current != mode;
    current = mode;
    if (changed && deck == QLatin1Char('A'))
        emit deckAPadModeChanged();
    else if (changed)
        emit deckBPadModeChanged();

    refreshPadModeLeds(deck);
    refreshPerformancePadLeds(deck, deck == QLatin1Char('A') ? m_deckA : m_deckB);
}

void MidiControllerManager::selectPerformancePadMode(const QString& deckId, int mode)
{
    if (mode < static_cast<int>(MidiPadMode::HotCue)
        || mode > static_cast<int>(MidiPadMode::Sampler)) {
        return;
    }

    QChar deck;
    if (!parsePerformanceDeckId(deckId, deck))
        return;

    DjEngine* const engine = deck == QLatin1Char('A') ? m_deckA : m_deckB;
    const MidiPadMode nextMode = static_cast<MidiPadMode>(mode);
    const MidiPadMode currentMode = padModeForDeck(deck);
    if (currentMode == nextMode)
        return;

    if (currentMode == MidiPadMode::PadFx)
        clearPadFxState(deck, engine);
    if (currentMode == MidiPadMode::HotCue || currentMode == MidiPadMode::Sampler)
        releaseHeldHotCue(deck, engine);

    setPadModeForDeck(deck, nextMode);
    emit performancePadStateChanged(performanceDeckId(deck));
}

void MidiControllerManager::setPerformancePadPressed(const QString& deckId,
                                                     int padIndex,
                                                     bool pressed)
{
    QChar deck;
    if (!parsePerformanceDeckId(deckId, deck) || padIndex < 0 || padIndex >= 8)
        return;

    DjEngine* const engine = deck == QLatin1Char('A') ? m_deckA : m_deckB;
    handlePerformancePad(deck, engine, padModeForDeck(deck), padIndex, pressed, false);
}

void MidiControllerManager::clearPerformancePad(const QString& deckId, int padIndex)
{
    QChar deck;
    if (!parsePerformanceDeckId(deckId, deck) || padIndex < 0 || padIndex >= 8)
        return;

    DjEngine* const engine = deck == QLatin1Char('A') ? m_deckA : m_deckB;
    handlePerformancePad(deck, engine, MidiPadMode::HotCue, padIndex, true, true);
}

bool MidiControllerManager::consumePerformancePadPlayLatch(const QString& deckId)
{
    QChar deck;
    if (!parsePerformanceDeckId(deckId, deck))
        return false;

    HotCueHoldState& hold = deck == QLatin1Char('A')
        ? m_deckAHotCueHold
        : m_deckBHotCueHold;
    if (hold.padIndex < 0 || !hold.returnOnRelease)
        return false;
    hold.returnOnRelease = false;
    return true;
}

int MidiControllerManager::performancePadFxMomentary(const QString& deckId) const
{
    QChar deck;
    if (!parsePerformanceDeckId(deckId, deck))
        return -1;
    return deck == QLatin1Char('A') ? m_deckAPadFxMomentary : m_deckBPadFxMomentary;
}

int MidiControllerManager::performancePadFxToggle(const QString& deckId) const
{
    QChar deck;
    if (!parsePerformanceDeckId(deckId, deck))
        return -1;
    return deck == QLatin1Char('A') ? m_deckAPadFxToggle : m_deckBPadFxToggle;
}

void MidiControllerManager::connectFxManager(FxManager* fxManager)
{
    if (m_fxManager)
        QObject::disconnect(m_fxManager.data(), nullptr, this, nullptr);

    m_fxManager = fxManager;
    if (!m_fxManager)
        return;

    auto syncBeatFxState = [this]
    {
        if (!m_fxManager)
            return;
        const bool active = m_fxManager->wetDry1() > 0.001f;
        if (m_beatFxActive != active) {
            m_beatFxActive = active;
            emit beatFxActiveChanged();
        }
        refreshFxLeds();
    };

    QObject::connect(m_fxManager, &FxManager::wetDry1Changed, this, syncBeatFxState);
    QObject::connect(m_fxManager, &FxManager::effectType1Changed, this, [this]
    {
        if (!m_fxManager)
            return;
        const int position = midi_internal::beatFxPositionForName(m_fxManager->effectType1());
        if (position > 0)
            m_beatFxPosition = position;
        refreshFxLeds();
    });
    auto syncBeatFxDeck = [this]
    {
        if (!m_fxManager || m_applyingBeatFxRouting)
            return;
        const std::array<bool, 4> assigned {
            m_fxManager->deck1A(), m_fxManager->deck1B(),
            m_fxManager->deck1C(), m_fxManager->deck1D()
        };
        const int assignedCount = static_cast<int>(std::count(assigned.begin(), assigned.end(), true));
        if (assignedCount == 4) {
            m_beatFxTarget = MidiBeatFxTarget::Master;
        } else if (assignedCount == 1) {
            const auto selected = std::find(assigned.begin(), assigned.end(), true);
            m_beatFxTarget = static_cast<MidiBeatFxTarget>(
                std::distance(assigned.begin(), selected));
        }
        refreshFxLeds();
    };
    QObject::connect(m_fxManager, &FxManager::deck1AChanged, this, syncBeatFxDeck);
    QObject::connect(m_fxManager, &FxManager::deck1BChanged, this, syncBeatFxDeck);
    QObject::connect(m_fxManager, &FxManager::deck1CChanged, this, syncBeatFxDeck);
    QObject::connect(m_fxManager, &FxManager::deck1DChanged, this, syncBeatFxDeck);
    QObject::connect(m_fxManager, &FxManager::soundColorModeChanged,
                     this, &MidiControllerManager::refreshFxLeds);
    syncBeatFxState();
    syncBeatFxDeck();
}

void MidiControllerManager::applyBeatFxState()
{
    const EffectType type = midi_internal::beatFxTypeForPosition(m_beatFxPosition);
    const float wet = m_beatFxActive ? 1.0f : 0.0f;

    if (m_fxManager) {
        const MidiBeatFxTarget target = m_beatFxTarget;
        const int targetDeck = midi_internal::beatFxDeckNumber(target);
        m_fxManager->setEffectType1(midi_internal::beatFxNameForPosition(m_beatFxPosition));
        m_applyingBeatFxRouting = true;
        for (int deck = 1; deck <= 4; ++deck) {
            m_fxManager->setDeckAssignment(1, deck,
                targetDeck == deck);
        }
        m_applyingBeatFxRouting = false;
        m_fxManager->setWetDry1(wet);
        AudioEngine::setMasterFx(target == MidiBeatFxTarget::Master ? type : EffectType::None,
                                 target == MidiBeatFxTarget::Master ? wet : 0.0f);
    } else {
        AudioEngine::setMasterFx(m_beatFxTarget == MidiBeatFxTarget::Master
                                     ? type : EffectType::None,
                                 m_beatFxTarget == MidiBeatFxTarget::Master ? wet : 0.0f);
        if (m_deckA) {
            m_deckA->setFxSlotEffectType(1, type);
            m_deckA->setFxSlotWetDry(1,
                m_beatFxTarget == MidiBeatFxTarget::DeckA ? wet : 0.0f);
        }
        if (m_deckB) {
            m_deckB->setFxSlotEffectType(1, type);
            m_deckB->setFxSlotWetDry(1,
                m_beatFxTarget == MidiBeatFxTarget::DeckB ? wet : 0.0f);
        }
    }

    refreshFxLeds();
    qDebug() << "[MIDI ACTION] action=BeatFx"
             << "position:" << m_beatFxPosition
             << "target:" << midi_internal::beatFxTargetName(m_beatFxTarget)
             << "active:" << m_beatFxActive
             << "dispatch=applyBeatFxSlot1";
}

void MidiControllerManager::refreshFxLeds()
{
    sendMappedNoteLed(QStringLiteral("beat_fx_on"), m_beatFxActive);
    for (const auto& [paramId, target] : midi_internal::kBeatFxChannels)
        sendMappedNoteLed(QString::fromLatin1(paramId), m_beatFxTarget == target);
    for (int position = 1; position <= 14; ++position) {
        sendMappedNoteLed(QStringLiteral("beat_fx_select_%1").arg(position),
                          position == m_beatFxPosition);
    }

    const QString soundColorMode = m_fxManager
        ? m_fxManager->soundColorMode()
        : QStringLiteral("Filter");
    for (const auto& [paramId, mode] : midi_internal::kSoundColorModes) {
        sendMappedNoteLed(QString::fromLatin1(paramId),
                          soundColorMode == QLatin1String(mode));
    }
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

void MidiControllerManager::releaseHeldHotCue(QChar deck, DjEngine* engine)
{
    HotCueHoldState& hold = deck == QLatin1Char('A')
        ? m_deckAHotCueHold
        : m_deckBHotCueHold;
    const bool returnOnRelease = hold.returnOnRelease;
    const double returnPositionSeconds = hold.returnPositionSeconds;
    hold = {};

    if (!engine || !returnOnRelease)
        return;

    engine->pause();
    const double durationSeconds = engine->getDuration();
    if (durationSeconds > 0.0) {
        const double normalized = std::clamp(returnPositionSeconds / durationSeconds, 0.0, 1.0);
        engine->setPosition(static_cast<float>(normalized));
    }
}

void MidiControllerManager::handleCuePadHold(QChar deck,
                                              DjEngine* engine,
                                              int padIndex,
                                              bool pressed,
                                              bool storeIfEmpty)
{
    if (!engine || padIndex < 0 || padIndex >= 8)
        return;

    HotCueHoldState& hold = deck == QLatin1Char('A')
        ? m_deckAHotCueHold
        : m_deckBHotCueHold;

    if (!pressed) {
        if (hold.padIndex == padIndex)
            releaseHeldHotCue(deck, engine);
        return;
    }

    if (hold.padIndex == padIndex)
        return;
    if (hold.padIndex >= 0)
        releaseHeldHotCue(deck, engine);

    const CuePadInfo cue = cuePadInfo(engine, padIndex);
    if (!cue.set) {
        if (storeIfEmpty)
            engine->storeCuePad(padIndex);
        return;
    }

    hold.padIndex = padIndex;
    hold.returnPositionSeconds = cue.positionSeconds;
    hold.returnOnRelease = !engine->isPlaying();
    engine->triggerCuePad(padIndex);
    if (hold.returnOnRelease)
        engine->play();
}

void MidiControllerManager::handlePerformancePad(QChar deck,
                                                 DjEngine* engine,
                                                 MidiPadMode mode,
                                                 int padIndex,
                                                 bool pressed,
                                                 bool clearRequest)
{
    if (!engine || padIndex < 0 || padIndex >= 8)
        return;

    if (mode == MidiPadMode::HotCue) {
        if (clearRequest && pressed) {
            if ((deck == QLatin1Char('A') ? m_deckAHotCueHold : m_deckBHotCueHold).padIndex == padIndex)
                releaseHeldHotCue(deck, engine);
            engine->clearCuePad(padIndex);
            refreshPerformancePadLeds(deck, engine);
        } else if (!clearRequest) {
            handleCuePadHold(deck, engine, padIndex, pressed, true);
            if (pressed)
                refreshPerformancePadLeds(deck, engine);
        }
        return;
    }

    if (mode == MidiPadMode::Sampler) {
        // The current mixer has no independent sampler bus; assigned cue/loop
        // slots form the deck-local sample bank and empty slots stay untouched.
        if (!clearRequest)
            handleCuePadHold(deck, engine, padIndex, pressed, false);
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
        emit performancePadStateChanged(performanceDeckId(deck));
        return;
    }

    if (!pressed)
        return;

    if (toggle == padIndex) {
        stopPadFxToggle(engine, padIndex);
        toggle = -1;
        emit performancePadStateChanged(performanceDeckId(deck));
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
    emit performancePadStateChanged(performanceDeckId(deck));
}

bool MidiControllerManager::dispatchFlx10JogAction(const QString& paramId,
                                                   float value,
                                                   double eventTimestampSeconds)
{
    const bool deckA = paramId.startsWith(QStringLiteral("deckA_"));
    const bool deckB = paramId.startsWith(QStringLiteral("deckB_"));
    if (!deckA && !deckB)
        return false;

    flx10::JogEventType eventType;
    if (paramId.endsWith(QStringLiteral("_jog_touch"))) {
        eventType = value >= 0.5f ? flx10::JogEventType::TouchDown
                                  : flx10::JogEventType::TouchUp;
    } else if (paramId.endsWith(QStringLiteral("_jog_scratch"))) {
        eventType = flx10::JogEventType::Platter;
    } else if (paramId.endsWith(QStringLiteral("_jog_nudge"))) {
        eventType = flx10::JogEventType::Rim;
    } else if (paramId.endsWith(QStringLiteral("_jog_move"))) {
        eventType = flx10::JogEventType::Generic;
    } else {
        return false;
    }

    DjEngine* const engine = deckA ? m_deckA : m_deckB;
    auto& router = deckA ? m_jogARouter : m_jogBRouter;
    bool& touched = deckA ? m_jogATouched : m_jogBTouched;
    const double timestamp = std::isfinite(eventTimestampSeconds)
            && eventTimestampSeconds > 0.0
        ? eventTimestampSeconds
        : juce::Time::getMillisecondCounterHiRes() * 0.001;
    const flx10::JogInput input {
        eventType,
        eventType == flx10::JogEventType::TouchDown
                || eventType == flx10::JogEventType::TouchUp
            ? 0.0
            : static_cast<double>(value),
        timestamp,
        engine != nullptr && engine->isScratchReleaseActive()
    };
    const auto route = router.route(input);

    if (m_midiTraceEnabled) {
        qDebug().nospace()
            << "[FLX10 JOG] deck=" << (deckA ? 'A' : 'B')
            << " stream=" << jogEventName(eventType)
            << " ticks=" << route.ticks
            << " dtMs=" << route.eventIntervalSeconds * 1000.0
            << " rate=" << route.estimatedRate
            << " phase=" << jogPhaseName(route.phase)
            << " action=" << jogActionName(route.action);
    }

    if (!engine)
        return true;

    switch (route.action) {
    case flx10::JogRouteAction::BeginScratch:
        touched = true;
        m_scratchAbsoluteLastByMsgId.clear();
        engine->pauseForScrub();
        break;
    case flx10::JogRouteAction::RequestRelease:
        touched = false;
        m_scratchAbsoluteLastByMsgId.clear();
        engine->requestScratchRelease(route.estimatedRate, true);
        break;
    case flx10::JogRouteAction::ScratchDelta:
    {
        double trackingInterval = route.eventIntervalSeconds;
        if (!(trackingInterval > 0.0)
            && std::isfinite(route.estimatedRate)
            && std::abs(route.estimatedRate) > 1.0e-9) {
            // The first wheel tick has no previous wheel event, but its rate is
            // measured from touch-down. Recover that interval instead of making
            // a fast first tick look like the 16 ms compatibility fallback.
            trackingInterval = std::abs(route.deltaSeconds / route.estimatedRate);
        }
        engine->scratchBySecondsTimed(route.deltaSeconds, trackingInterval);
        break;
    }
    case flx10::JogRouteAction::ReleaseDelta:
        engine->submitScratchReleaseSpeed(route.estimatedRate);
        break;
    case flx10::JogRouteAction::Nudge:
        engine->applyJogNudge(route.ticks);
        break;
    case flx10::JogRouteAction::Ignore:
        break;
    }

    return true;
}

void MidiControllerManager::connectDecks(DjEngine* deckA, DjEngine* deckB,
                                         DjEngine* deckC, DjEngine* deckD)
{
    if (m_deckA)
        QObject::disconnect(m_deckA, nullptr, this, nullptr);
    if (m_deckB && m_deckB != m_deckA)
        QObject::disconnect(m_deckB, nullptr, this, nullptr);
    if (m_deckC && m_deckC != m_deckA && m_deckC != m_deckB)
        QObject::disconnect(m_deckC, nullptr, this, nullptr);
    if (m_deckD && m_deckD != m_deckA && m_deckD != m_deckB && m_deckD != m_deckC)
        QObject::disconnect(m_deckD, nullptr, this, nullptr);

    m_deckA = deckA;
    m_deckB = deckB;
    m_deckC = deckC;
    m_deckD = deckD;
    m_cueAHeld = false;
    m_cueBHeld = false;
    m_jogATouched = false;
    m_jogBTouched = false;
    m_jogARouter.reset();
    m_jogBRouter.reset();
    m_deckAShiftHeld = false;
    m_deckBShiftHeld = false;
    m_tempoRawInputSeen = { false, false };
    m_tempoInputSeen = { false, false };
    m_deckASlipReverseHeld = false;
    m_deckBSlipReverseHeld = false;
    m_deckAReverseBeforeSlip = false;
    m_deckBReverseBeforeSlip = false;
    m_deckASlipBeforeReverse = false;
    m_deckBSlipBeforeReverse = false;
    const bool deckAPadModeWasChanged = m_deckAPadMode != MidiPadMode::HotCue;
    const bool deckBPadModeWasChanged = m_deckBPadMode != MidiPadMode::HotCue;
    m_deckAPadMode = MidiPadMode::HotCue;
    m_deckBPadMode = MidiPadMode::HotCue;
    m_deckAPadFxMomentary = -1;
    m_deckBPadFxMomentary = -1;
    m_deckAPadFxToggle = -1;
    m_deckBPadFxToggle = -1;
    m_deckAHotCueHold = {};
    m_deckBHotCueHold = {};
    m_deckAFxSlotsEnabled = { false, false, false };
    m_deckBFxSlotsEnabled = { false, false, false };
    m_beatFxActive = false;
    m_beatFxPosition = 1;
    m_beatFxTarget = MidiBeatFxTarget::DeckA;
    m_midiFeedback.setDecks(m_deckA, m_deckB, m_deckC, m_deckD);
    if (deckAPadModeWasChanged)
        emit deckAPadModeChanged();
    if (deckBPadModeWasChanged)
        emit deckBPadModeChanged();

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
        QObject::connect(engine, &DjEngine::mainCueChanged,
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
        QObject::connect(engine, &DjEngine::reverseChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::hotCuesChanged,
                         this, [this, deck, engine] { refreshPerformancePadLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::savedLoopsChanged,
                         this, [this, deck, engine] { refreshPerformancePadLeds(deck, engine); });
    };
    wireDeckLeds(QLatin1Char('A'), m_deckA);
    wireDeckLeds(QLatin1Char('B'), m_deckB);
    // Channels 3/4 currently expose their mixer strip rather than full deck
    // performance controls. Their PFL state still owns a real FLX10 CUE LED.
    auto wireMixerCueLed = [this](int deck, DjEngine* engine)
    {
        if (!engine)
            return;
        QObject::connect(engine, &DjEngine::cueEnabledChanged,
                         this, [this, deck] { m_midiFeedback.refreshDeckLeds(deck); });
    };
    wireMixerCueLed(3, m_deckC);
    wireMixerCueLed(4, m_deckD);
    refreshAllDeckLeds();

    if (!m_parameterStore)
        return;

    if (m_deckActionsConnection)
        QObject::disconnect(m_deckActionsConnection);

    // Route ParameterStore events to deck actions.
    // Volume/crossfader/mixer EQ: MixerParameterBridge + MixerControl (C++).
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

        if (id == QStringLiteral("library_view_toggle")) {
            if (value >= 0.5f)
                emit libraryViewToggleRequested();
            return;
        }

        if (id == QStringLiteral("deckA_sound_color")
            || id == QStringLiteral("deckB_sound_color")) {
            if (m_fxManager) {
                const float bipolar = std::clamp(value * 2.0f - 1.0f, -1.0f, 1.0f);
                m_fxManager->setSoundColorChannel(
                    id.startsWith(QStringLiteral("deckA_"))
                        ? QStringLiteral("deckA")
                        : QStringLiteral("deckB"),
                    std::abs(bipolar) < 0.01f ? 0.0f : bipolar);
            }
            return;
        }

        for (const auto& [paramId, mode] : midi_internal::kSoundColorModes) {
            if (id == QLatin1String(paramId)) {
                if (value >= 0.5f && m_fxManager)
                    m_fxManager->setSoundColorMode(QString::fromLatin1(mode));
                return;
            }
        }

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

        auto handleFourBeatExit = [](DjEngine* engine, bool shiftHeld)
        {
            if (!engine)
                return;
            if (shiftHeld) {
                engine->reactivateLoop();
            } else if (engine->loopActive()) {
                engine->deactivateLoop();
            } else {
                engine->setLoop4Beats();
            }
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
                if ((padModeForDeck(deck) == MidiPadMode::HotCue
                     || padModeForDeck(deck) == MidiPadMode::Sampler)
                    && padModeForDeck(deck) != mode) {
                    releaseHeldHotCue(deck, deckEngine);
                }
                setPadModeForDeck(deck, mode);
                qDebug() << "[MIDI ACTION] action=PadMode"
                         << "deck:" << deck
                         << "mode:" << id
                         << "dispatch=setPadMode";
                return;
            }

            if (midi_internal::parsePerformancePadParam(id, deck, padIndex, clearPad)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                if (value >= 0.5f && padModeForDeck(deck) != MidiPadMode::HotCue) {
                    if (padModeForDeck(deck) == MidiPadMode::PadFx)
                        clearPadFxState(deck, deckEngine);
                    releaseHeldHotCue(deck, deckEngine);
                    setPadModeForDeck(deck, MidiPadMode::HotCue);
                }
                handlePerformancePad(deck, deckEngine, MidiPadMode::HotCue,
                                     padIndex, value >= 0.5f, clearPad);
                return;
            }

            if (midi_internal::parseDirectPadParam(id, QStringLiteral("padfx_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                if (value >= 0.5f && padModeForDeck(deck) != MidiPadMode::PadFx) {
                    releaseHeldHotCue(deck, deckEngine);
                    setPadModeForDeck(deck, MidiPadMode::PadFx);
                }
                handlePerformancePad(deck, deckEngine, MidiPadMode::PadFx,
                                     padIndex, value >= 0.5f, false);
                return;
            }

            if (midi_internal::parseDirectPadParam(id, QStringLiteral("beatjump_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                if (value >= 0.5f && padModeForDeck(deck) != MidiPadMode::BeatJump) {
                    if (padModeForDeck(deck) == MidiPadMode::PadFx)
                        clearPadFxState(deck, deckEngine);
                    releaseHeldHotCue(deck, deckEngine);
                    setPadModeForDeck(deck, MidiPadMode::BeatJump);
                }
                handlePerformancePad(deck, deckEngine, MidiPadMode::BeatJump,
                                     padIndex, value >= 0.5f, false);
                return;
            }

            if (midi_internal::parseDirectPadParam(id, QStringLiteral("sampler_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                if (value >= 0.5f && padModeForDeck(deck) != MidiPadMode::Sampler) {
                    if (padModeForDeck(deck) == MidiPadMode::PadFx)
                        clearPadFxState(deck, deckEngine);
                    releaseHeldHotCue(deck, deckEngine);
                    setPadModeForDeck(deck, MidiPadMode::Sampler);
                }
                handlePerformancePad(deck, deckEngine, MidiPadMode::Sampler,
                                     padIndex, value >= 0.5f, false);
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
                applyBeatFxState();
            }
            return;
        }
        for (const auto& [paramId, target] : midi_internal::kBeatFxChannels) {
            if (id == QLatin1String(paramId)) {
                if (value >= 0.5f) {
                    m_beatFxTarget = target;
                    applyBeatFxState();
                }
                return;
            }
        }
        if (id == QStringLiteral("beat_fx_on")) {
            const bool active = value >= 0.5f;
            if (m_beatFxActive != active) {
                m_beatFxActive = active;
                emit beatFxActiveChanged();
            }
            applyBeatFxState();
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
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("quantize"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setQuantizeEnabled(!engine->quantizeEnabled());
            }
            return;
        }
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("slip_reverse"), directDeck)) {
            DjEngine* const engine = engineForDeck(directDeck);
            bool& held = directDeck == QLatin1Char('A')
                ? m_deckASlipReverseHeld
                : m_deckBSlipReverseHeld;
            bool& previousReverse = directDeck == QLatin1Char('A')
                ? m_deckAReverseBeforeSlip
                : m_deckBReverseBeforeSlip;
            bool& previousSlip = directDeck == QLatin1Char('A')
                ? m_deckASlipBeforeReverse
                : m_deckBSlipBeforeReverse;
            const bool pressed = value >= 0.5f;
            if (pressed && !held) {
                previousReverse = engine && engine->isReverse();
                previousSlip = engine && engine->slipActive();
                held = true;
                if (engine) {
                    if (!previousSlip)
                        engine->setSlip(true);
                    engine->setReverse(true);
                }
            } else if (!pressed && held) {
                held = false;
                if (engine) {
                    engine->setReverse(previousReverse);
                    if (!previousSlip)
                        engine->setSlip(false);
                }
            }
            return;
        }
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("tempo_range_cycle"), directDeck)) {
            if (value >= 0.5f)
                cycleFlx10TempoRange(engineForDeck(directDeck));
            return;
        }
        if (midi_internal::parseDeckButtonParam(id, QStringLiteral("tempo_reset"), directDeck)
            || midi_internal::parseDeckButtonParam(id, QStringLiteral("rate_reset"), directDeck)) {
            if (value >= 0.5f) {
                DjEngine* const engine = engineForDeck(directDeck);
                const bool shiftHeld = directDeck == QLatin1Char('A')
                    ? m_deckAShiftHeld
                    : m_deckBShiftHeld;
                if (shiftHeld)
                    cycleFlx10TempoRange(engine);
                else if (engine)
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
            if (value >= 0.5f)
                handleFourBeatExit(a, m_deckAShiftHeld);
        }
        else if (id == "deckB_loop_reloop") {
            if (value >= 0.5f)
                handleFourBeatExit(b, m_deckBShiftHeld);
        }
        else if (id == "deckA_loop_4beat") {
            if (value >= 0.5f)
                handleFourBeatExit(a, m_deckAShiftHeld);
        }
        else if (id == "deckB_loop_4beat") {
            if (value >= 0.5f)
                handleFourBeatExit(b, m_deckBShiftHeld);
        }
        // Trim/EQ/filter: MixerParameterBridge applies these for all four decks.
        // Tempo fader: MIDI 0-1 -> current deck tempo range.
        else if (id == "deckA_tempo")  {
            if (a) {
                const double range = std::max(1.0, a->tempoRangePercent());
                const double percent = static_cast<double>(value) * 2.0 * range - range;
                if (!m_tempoInputSeen[0]) {
                    m_tempoInputSeen[0] = true;
                    qInfo() << "[MIDI IN] FLX10 tempo fader deck A detected"
                            << "normalized:" << value << "tempo:" << percent;
                }
                a->setTempoPercent(percent);
            }
        }
        else if (id == "deckB_tempo")  {
            if (b) {
                const double range = std::max(1.0, b->tempoRangePercent());
                const double percent = static_cast<double>(value) * 2.0 * range - range;
                if (!m_tempoInputSeen[1]) {
                    m_tempoInputSeen[1] = true;
                    qInfo() << "[MIDI IN] FLX10 tempo fader deck B detected"
                            << "normalized:" << value << "tempo:" << percent;
                }
                b->setTempoPercent(percent);
            }
        }
    });
}
