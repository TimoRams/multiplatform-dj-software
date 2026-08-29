#include "controllers/midi/MidiControllerManager.h"
#include "controllers/midi/MidiParameterDispatch.h"
#include "controllers/flx10/Flx10ControllerIdentity.h"
#include "audio/AudioEngine.h"
#include "fx/FxManager.h"
#include "fx/FxTypes.h"

using namespace midi;

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
#include <array>
#include <ranges>

namespace {

// Hardware-panel tables and the parameter-id shapes this controller emits.
// Only this bridge speaks them, so they stay private to it.

struct SoundColorModeMapping {
    const char* paramId;
    const char* mode;
};
constexpr std::array<SoundColorModeMapping, 6> kSoundColorModes {{
    { "sound_color_fx_space", "Space" },
    { "sound_color_fx_dub_echo", "D.Echo" },
    { "sound_color_fx_crush", "Crush" },
    { "sound_color_fx_pitch", "Pitch" },
    { "sound_color_fx_noise", "Noise" },
    { "sound_color_fx_filter", "Filter" }
}};

struct BeatFxChannelMapping {
    const char* paramId;
    MidiBeatFxTarget target;
};
constexpr std::array<BeatFxChannelMapping, 7> kBeatFxChannels {{
    { "beat_fx_channel_deck_a", MidiBeatFxTarget::DeckA },
    { "beat_fx_channel_deck_b", MidiBeatFxTarget::DeckB },
    { "beat_fx_channel_deck_c", MidiBeatFxTarget::DeckC },
    { "beat_fx_channel_deck_d", MidiBeatFxTarget::DeckD },
    { "beat_fx_channel_master", MidiBeatFxTarget::Master },
    { "beat_fx_channel_mic", MidiBeatFxTarget::Mic },
    { "beat_fx_channel_sampler", MidiBeatFxTarget::Sampler }
}};

constexpr int beatFxDeckNumber(MidiBeatFxTarget target) noexcept
{
    switch (target) {
    case MidiBeatFxTarget::DeckA: return 1;
    case MidiBeatFxTarget::DeckB: return 2;
    case MidiBeatFxTarget::DeckC: return 3;
    case MidiBeatFxTarget::DeckD: return 4;
    default: return 0;
    }
}

// Order of the BEAT FX selector on the hardware panel, top to bottom. The two
// tables are indexed together, and the names must match exactly what
// FxManager::effectTypeFromString() understands.
constexpr std::array<EffectType, 14> kBeatFxTypes {{
    EffectType::LowCutEcho, EffectType::Echo,      EffectType::MtDelay,
    EffectType::Spiral,     EffectType::Reverb,    EffectType::Trans,
    EffectType::EnigmaJet,  EffectType::Flanger,   EffectType::Phaser,
    EffectType::Stretch,    EffectType::SlipRoll,  EffectType::Roll,
    EffectType::MobiusSaw,  EffectType::MobiusTri
}};

const std::array<QString, 14>& beatFxNames()
{
    static const std::array<QString, 14> names = {
        QStringLiteral("Low Cut Echo"), QStringLiteral("Echo"),
        QStringLiteral("MT Delay"),     QStringLiteral("Spiral"),
        QStringLiteral("Reverb"),       QStringLiteral("Trans"),
        QStringLiteral("Enigma Jet"),   QStringLiteral("Flanger"),
        QStringLiteral("Phaser"),       QStringLiteral("Stretch"),
        QStringLiteral("Slip Roll"),    QStringLiteral("Roll"),
        QStringLiteral("Mobius Saw"),   QStringLiteral("Mobius Tri")
    };
    return names;
}

constexpr size_t beatFxIndex(int position) noexcept
{
    return static_cast<size_t>(std::clamp(position, 1, 14) - 1);
}

constexpr EffectType beatFxTypeForPosition(int position) noexcept
{
    return kBeatFxTypes[beatFxIndex(position)];
}

QString beatFxNameForPosition(int position)
{
    return beatFxNames()[beatFxIndex(position)];
}

int beatFxPositionForName(const QString& name)
{
    const auto& names = beatFxNames();
    const auto it = std::ranges::find(names, name);
    return it == names.end() ? -1 : static_cast<int>(std::distance(names.begin(), it)) + 1;
}

QString hexByte(int value)
{
    return QStringLiteral("%1").arg(value & 0xff, 2, 16, QLatin1Char('0')).toUpper();
}

// Splits "deck<A|B|C|D><stem><n>" style parameter ids. Returns false unless the
// deck prefix matches and the trailing number is a valid 1-based pad index.
bool splitDeckIndexParam(const QString& paramId, const QString& stem,
                         QChar& deck, QString& suffix)
{
    for (const QChar candidate : { QLatin1Char('A'), QLatin1Char('B'),
                                   QLatin1Char('C'), QLatin1Char('D') }) {
        const QString prefix = QStringLiteral("deck%1_").arg(candidate) + stem;
        if (paramId.startsWith(prefix)) {
            deck = candidate;
            suffix = paramId.mid(prefix.size());
            return true;
        }
    }
    return false;
}

bool parsePadIndexParam(const QString& paramId, const QString& stem,
                        QChar& deck, int& index, bool* clear)
{
    QString suffix;
    if (!splitDeckIndexParam(paramId, stem, deck, suffix))
        return false;

    if (clear) {
        *clear = suffix.endsWith(QStringLiteral("_clear"));
        if (*clear)
            suffix.chop(QStringLiteral("_clear").size());
    }

    bool ok = false;
    index = suffix.toInt(&ok) - 1;
    return ok && index >= 0 && index < 8;
}

bool parsePerformancePadParam(const QString& paramId, QChar& deck, int& index, bool& clear)
{
    return parsePadIndexParam(paramId, QStringLiteral("pad"), deck, index, &clear);
}

bool parseHotCueParam(const QString& paramId, QChar& deck, int& index, bool& clear)
{
    return parsePadIndexParam(paramId, QStringLiteral("hotcue"), deck, index, &clear);
}

bool parseDirectPadParam(const QString& paramId, const QString& stem, QChar& deck, int& index)
{
    return parsePadIndexParam(paramId, stem, deck, index, nullptr);
}

bool parseKeyShiftPadParam(const QString& paramId, QChar& deck, int& index, bool& shifted)
{
    QString suffix;
    if (!splitDeckIndexParam(paramId, QStringLiteral("keyshift_pad"), deck, suffix))
        return false;

    shifted = suffix.endsWith(QStringLiteral("_shift"));
    if (shifted)
        suffix.chop(QStringLiteral("_shift").size());
    if (suffix.endsWith(QStringLiteral("_page2")))
        suffix.chop(QStringLiteral("_page2").size());

    bool ok = false;
    index = suffix.toInt(&ok) - 1;
    return ok && index >= 0 && index < 8;
}

// Key Shift's three range pages, one fixed absolute semitone offset per pad.
// A pad press sets an absolute offset (never adds to the current one); the
// pad reading 0 is the "root" pad for that range and moves with it.
static constexpr int kFlx10KeyShiftTable[3][8] = {
    { -3, -2, -1,  0, -7, -6, -5, -4 }, // Down
    {  0,  1,  2,  3, -4, -3, -2, -1 }, // Middle
    {  4,  5,  6,  7,  0,  1,  2,  3 }, // Up
};

bool parsePadModeParam(const QString& paramId, QChar& deck, MidiPadMode& mode)
{
    QString suffix;
    if (!splitDeckIndexParam(paramId, QStringLiteral("pad_mode_"), deck, suffix))
        return false;

    if (suffix == QStringLiteral("hotcue"))   { mode = MidiPadMode::HotCue;   return true; }
    if (suffix == QStringLiteral("padfx"))    { mode = MidiPadMode::PadFx;    return true; }
    if (suffix == QStringLiteral("beatjump")) { mode = MidiPadMode::BeatJump; return true; }
    if (suffix == QStringLiteral("sampler"))  { mode = MidiPadMode::Sampler;  return true; }
    if (suffix == QStringLiteral("keyshift")) { mode = MidiPadMode::KeyShift; return true; }
    return false;
}

bool parseDeckButtonParam(const QString& paramId, const QString& suffix, QChar& deck)
{
    for (const QChar candidate : { QLatin1Char('A'), QLatin1Char('B'),
                                   QLatin1Char('C'), QLatin1Char('D') }) {
        if (paramId == QStringLiteral("deck%1_").arg(candidate) + suffix) {
            deck = candidate;
            return true;
        }
    }
    return false;
}

bool parseDeckFxSlotParam(const QString& paramId, QChar& deck, int& slot)
{
    QString suffix;
    if (!splitDeckIndexParam(paramId, QStringLiteral("fx_slot"), deck, suffix))
        return false;

    bool ok = false;
    slot = suffix.toInt(&ok);
    return ok && slot >= 1 && slot <= 3;
}

bool parseBeatFxSelectParam(const QString& paramId, int& position)
{
    static const QString prefix = QStringLiteral("beat_fx_select_");
    if (!paramId.startsWith(prefix))
        return false;

    bool ok = false;
    position = paramId.mid(prefix.size()).toInt(&ok);
    return ok && position >= 1 && position <= 14;
}


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
            == normalizeControllerKeyFromXmlBase(flx10::kControllerName)
        || flx10::isBuiltInMapping(getSelectedMapping())) {
        return true;
    }

    const QString selectedOutput = SettingsManager::getInstance().getMidiOutputIdentifier();
    const juce::String selectedId = juce::String::fromUTF8(selectedOutput.toUtf8().constData());
    const int index = indexOfIdentifier(m_availableOutputDeviceIdentifiers, selectedId);
    if (index >= 0 && index < m_availableOutputDeviceNames.size())
        return flx10::looksLikeControllerName(m_availableOutputDeviceNames.at(index));

    return flx10::looksLikeControllerName(selectedOutput);
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
    const MidiInteractionType interactionType = midi::defaultInteractionTypeForParam(id);
    if (midi::isRelativeInteraction(interactionType)
        || midi::isButtonInteraction(interactionType)
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
                                                  midi::clampMidi7bit(static_cast<int>(value * 127.0f)));
    } else {
        if (value > 0.0f)
            msg = juce::MidiMessage::noteOn(channel, midi::clampMidi7bit(subId), value);
        else
            msg = juce::MidiMessage::noteOff(channel, midi::clampMidi7bit(subId), 0.0f);
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
    return hotCueStatusForDeck(std::clamp(deck.toUpper().toLatin1() - 'A' + 1, 1, 4));
}

bool MidiControllerManager::sendMidiShort(int statusNo, int controlNo, int value, const QString& messageType)
{
    const int status = statusNo & 0xff;
    const int control = midi::clampMidi7bit(controlNo);
    const int dataValue = midi::clampMidi7bit(value);
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

    // Remember every lamp write, so the input side can recognise it if it comes
    // back in on one of the ports this application also listens on. Note-ons
    // only: a returning note-off is harmless, a returning note-on is a phantom
    // press.
    if (size == 3 && (status & 0xf0) == 0x90 && data2 > 0) {
        m_outputEchoGuard.noteSent(10000 + (status & 0x0f) * 2000 + data1, data2,
                                   juce::Time::getMillisecondCounterHiRes() * 0.001);
    }

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
        ? QStringLiteral("%1 %2 %3").arg(hexByte(status), hexByte(data1), hexByte(data2))
        : QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(raw), size).toHex(' ')).toUpper();
    // Controller feedback can produce thousands of successful packets per
    // second. Keep failures visible and make success traces explicitly opt-in.
    const bool logSuccess = m_midiTraceEnabled
        || qEnvironmentVariableIntValue("BROCKDJ_MIDI_OUT_LOG") > 0;

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
    sendMappedNoteLed(prefix + QStringLiteral("beat_sync"),
                      engine->syncEnabled() || engine->isSyncMaster());
    sendMappedNoteLed(prefix + QStringLiteral("key_sync"), engine->keylock());
    sendMappedNoteLed(prefix + QStringLiteral("keylock"), engine->keylock());
    sendMappedNoteLed(prefix + QStringLiteral("quantize"), engine->quantizeEnabled());
    sendMappedNoteLed(prefix + QStringLiteral("slip"), engine->slipActive());
    sendMappedNoteLed(prefix + QStringLiteral("slip_reverse"), engine->isReverse());
}

void MidiControllerManager::refreshPadModeLeds(QChar deck)
{
    const QString prefix = QStringLiteral("deck%1_pad_mode_").arg(deck.toUpper());
    const MidiPadMode mode = padModeForDeck(deck);
    sendMappedNoteLed(prefix + QStringLiteral("hotcue"), mode == MidiPadMode::HotCue);
    sendMappedNoteLed(prefix + QStringLiteral("padfx"), mode == MidiPadMode::PadFx);
    sendMappedNoteLed(prefix + QStringLiteral("beatjump"), mode == MidiPadMode::BeatJump);
    // 0x22 and 0x6f are independent FLX10 mode commands even though they share
    // one physical button. Sending 0x22 here would put the controller back into
    // its normal Sampler bank, so only the dedicated 0x6f Key Shift command may
    // be active in the shifted mode.
    sendMappedNoteLed(prefix + QStringLiteral("sampler"), mode == MidiPadMode::Sampler);
    sendMappedNoteLed(prefix + QStringLiteral("keyshift"),
                      mode == MidiPadMode::KeyShift && m_keyShiftModeBlinkOn);
}

void MidiControllerManager::updateKeyShiftModeBlink(double monotonicSeconds)
{
    const bool anyKeyShift = padModeForDeck(QLatin1Char('A')) == MidiPadMode::KeyShift
        || padModeForDeck(QLatin1Char('B')) == MidiPadMode::KeyShift
        || padModeForDeck(QLatin1Char('C')) == MidiPadMode::KeyShift
        || padModeForDeck(QLatin1Char('D')) == MidiPadMode::KeyShift;
    if (!anyKeyShift) {
        m_keyShiftModeBlinkOn = true;
        m_nextKeyShiftModeBlinkSeconds = 0.0;
        return;
    }

    if (m_nextKeyShiftModeBlinkSeconds <= 0.0) {
        m_nextKeyShiftModeBlinkSeconds = monotonicSeconds + 0.4;
        return;
    }
    if (monotonicSeconds < m_nextKeyShiftModeBlinkSeconds)
        return;

    m_keyShiftModeBlinkOn = !m_keyShiftModeBlinkOn;
    m_nextKeyShiftModeBlinkSeconds = monotonicSeconds + 0.4;
    for (const QChar deck : { QLatin1Char('A'), QLatin1Char('B'),
                              QLatin1Char('C'), QLatin1Char('D') }) {
        if (padModeForDeck(deck) == MidiPadMode::KeyShift)
            refreshPadModeLeds(deck);
    }
}

void MidiControllerManager::refreshHotCueLeds(QChar deck, DjEngine* engine)
{
    if (!engine)
        return;

    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshHotcuePads(
            std::clamp(deck.toUpper().toLatin1() - 'A' + 1, 1, 4));
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

        // Key Shift has dedicated PAGE1/PAGE2 pad-note banks (0x70..0x7F),
        // separate from Sampler's 0x30 bank. Keep both hardware pages in sync
        // and highlight the pad matching the engine's currently selected
        // absolute offset. The root pad is not special once another pitch has
        // been selected.
        const bool keyShift = padModeForDeck(deck) == MidiPadMode::KeyShift;
        const int range = static_cast<int>(keyShiftRangeForDeck(deck));
        const bool selected = keyShift && engine
            && std::abs(engine->keySemitoneOffset()
                        - static_cast<double>(kFlx10KeyShiftTable[range][i])) < 0.01;
        const int keyShiftLed = selected ? 0x7F : 0;
        sendMidiNoteLed(status, 0x70 + i, keyShiftLed);
        sendMidiNoteLed(status, 0x78 + i, keyShiftLed);

        const CuePadInfo sample = cuePadInfo(engine, i);
        sendMidiNoteLed(status, 0x30 + i,
                        sample.set ? hotCueLedValueForColor(sample.color) : 0);
    }
}

void MidiControllerManager::refreshDeckLeds(QChar deck, DjEngine* engine)
{
    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshDeckLeds(
            std::clamp(deck.toUpper().toLatin1() - 'A' + 1, 1, 4));
        refreshPadModeLeds(deck);
        refreshPerformancePadLeds(deck, engine);
        return;
    }

    refreshTransportAndLoopLeds(deck, engine);
    refreshPadModeLeds(deck);
    refreshPerformancePadLeds(deck, engine);
}

void MidiControllerManager::refreshMixerLeds()
{
    // MASTER CUE is owned by the engine, not by the button. It can already be
    // engaged from the UI before the controller is connected, so the LED is
    // always driven from the engine's state rather than from a local latch.
    if (!m_deckA)
        return;
    sendMappedNoteLed(QStringLiteral("master_cue"), m_deckA->masterCueEnabled());
}

void MidiControllerManager::refreshAllDeckLeds()
{
    // Route every deck through the same wrapper so FLX10-specific pad-mode
    // and Key Shift banks are refreshed alongside the generic deck/hotcue
    // feedback. Calling MidiFeedbackController::refreshAll() alone would omit
    // those semantic mode LEDs.
    refreshDeckLeds(QLatin1Char('A'), m_deckA);
    refreshDeckLeds(QLatin1Char('B'), m_deckB);
    refreshDeckLeds(QLatin1Char('C'), m_deckC);
    refreshDeckLeds(QLatin1Char('D'), m_deckD);
    refreshFxLeds();
    refreshMixerLeds();
}

MidiPadMode MidiControllerManager::padModeForDeck(QChar deck) const
{
    switch (deck.toUpper().toLatin1()) {
    case 'A': return m_deckAPadMode;
    case 'B': return m_deckBPadMode;
    case 'C': return m_deckCPadMode;
    case 'D': return m_deckDPadMode;
    default: return MidiPadMode::HotCue;
    }
}

DjEngine* MidiControllerManager::engineForDeck(QChar deck) const noexcept
{
    switch (deck.toUpper().toLatin1()) {
    case 'A': return m_deckA;
    case 'B': return m_deckB;
    case 'C': return m_deckC;
    case 'D': return m_deckD;
    default: return nullptr;
    }
}

void MidiControllerManager::setPadModeForDeck(QChar deck, MidiPadMode mode)
{
    MidiPadMode* current = nullptr;
    switch (deck.toUpper().toLatin1()) {
    case 'A': current = &m_deckAPadMode; break;
    case 'B': current = &m_deckBPadMode; break;
    case 'C': current = &m_deckCPadMode; break;
    case 'D': current = &m_deckDPadMode; break;
    default: return;
    }
    const bool changed = *current != mode;
    *current = mode;
    if (changed && mode == MidiPadMode::KeyShift) {
        // Start each selection visibly lit; the control-clock callback takes
        // over the alternating phase after the first 400 ms.
        m_keyShiftModeBlinkOn = true;
        m_nextKeyShiftModeBlinkSeconds = 0.0;
    }
    if (changed && deck == QLatin1Char('A'))
        emit deckAPadModeChanged();
    else if (changed && deck == QLatin1Char('B'))
        emit deckBPadModeChanged();

    refreshPadModeLeds(deck);
    refreshPerformancePadLeds(deck, engineForDeck(deck));
}

Flx10KeyShiftRange MidiControllerManager::keyShiftRangeForDeck(QChar deck) const
{
    switch (deck.toUpper().toLatin1()) {
    case 'A': return m_deckAKeyShiftRange;
    case 'B': return m_deckBKeyShiftRange;
    case 'C': return m_deckCKeyShiftRange;
    case 'D': return m_deckDKeyShiftRange;
    default: return Flx10KeyShiftRange::Middle;
    }
}

void MidiControllerManager::setKeyShiftRangeForDeck(QChar deck, Flx10KeyShiftRange range)
{
    Flx10KeyShiftRange* current = nullptr;
    switch (deck.toUpper().toLatin1()) {
    case 'A': current = &m_deckAKeyShiftRange; break;
    case 'B': current = &m_deckBKeyShiftRange; break;
    case 'C': current = &m_deckCKeyShiftRange; break;
    case 'D': current = &m_deckDKeyShiftRange; break;
    default: return;
    }
    if (*current == range)
        return;
    *current = range;
    if (deck == QLatin1Char('A'))
        emit deckAKeyShiftRangeChanged();
    else if (deck == QLatin1Char('B'))
        emit deckBKeyShiftRangeChanged();
    refreshPerformancePadLeds(deck, engineForDeck(deck));
}

void MidiControllerManager::stepKeyShiftRange(QChar deck, int direction)
{
    const auto current = static_cast<int>(keyShiftRangeForDeck(deck));
    const auto next = static_cast<Flx10KeyShiftRange>(std::clamp(current + direction, 0, 2));
    if (next != keyShiftRangeForDeck(deck))
        setKeyShiftRangeForDeck(deck, next);
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
        const float currentWet = std::clamp(m_fxManager->wetDry1(), 0.0f, 1.0f);
        if (currentWet > 0.001f)
            m_beatFxLevelDepth = currentWet;
        // The unit's own engage flag, never the mix amount. Inferring the
        // on-state from a non-zero amount meant that any path which legitimately
        // wrote zero — the knob at rest, a sweep through the bottom of the mix
        // range — switched the effect off behind the user's back.
        const bool active = m_fxManager->enabled1();
        if (m_beatFxActive != active) {
            m_beatFxActive = active;
            emit beatFxActiveChanged();
        }
        refreshFxLeds();
    };

    QObject::connect(m_fxManager, &FxManager::wetDry1Changed, this, syncBeatFxState);
    QObject::connect(m_fxManager, &FxManager::enabled1Changed, this, syncBeatFxState);
    QObject::connect(m_fxManager, &FxManager::effectType1Changed, this, [this]
    {
        if (!m_fxManager)
            return;
        const int position = beatFxPositionForName(m_fxManager->effectType1());
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
    // Keep the hardware's beat division in step with the on-screen unit, and
    // re-derive the echo length whenever the assigned deck's tempo moves.
    QObject::connect(m_fxManager, &FxManager::beatDiv1Changed, this, [this]
    {
        if (!m_fxManager)
            return;
        m_beatFxDivision = m_fxManager->beatDiv1();
        pushBeatFxTiming();
    });
    QObject::connect(m_fxManager, &FxManager::displayBpm1Changed,
                     this, &MidiControllerManager::pushBeatFxTiming);
    syncBeatFxState();
    syncBeatFxDeck();
    pushBeatFxTiming();
}

void MidiControllerManager::applyBeatFxState(BeatFxEngage engage)
{
    // Picking an effect, a routing channel or a beat division says nothing about
    // whether FX should be on. The FX selector and the channel selector are
    // physical switches, so the controller reports their resting position on its
    // own — and every one of those reports used to push this object's idea of the
    // engage state onto the unit. Switch FX on anywhere, and the next such report
    // switched it straight back off, which is why it only ever happened with the
    // controller plugged in. Adopt the unit's state here instead of asserting it;
    // only the BEAT FX ON button gets to write it.
    if (m_fxManager && engage == BeatFxEngage::Follow) {
        const bool unitActive = m_fxManager->enabled1();
        if (m_beatFxActive != unitActive) {
            m_beatFxActive = unitActive;
            emit beatFxActiveChanged();
        }
    }

    const EffectType type = beatFxTypeForPosition(m_beatFxPosition);
    const float wet = m_beatFxActive ? m_beatFxLevelDepth : 0.0f;

    if (m_fxManager) {
        const MidiBeatFxTarget target = m_beatFxTarget;
        const int targetDeck = beatFxDeckNumber(target);
        m_fxManager->setEffectType1(beatFxNameForPosition(m_beatFxPosition));
        m_applyingBeatFxRouting = true;
        for (int deck = 1; deck <= 4; ++deck) {
            m_fxManager->setDeckAssignment(1, deck,
                targetDeck == deck);
        }
        m_applyingBeatFxRouting = false;
        // Engage state and mix amount are pushed separately. Only a knob that
        // has actually reported a position gets to dictate the amount; before
        // that the manager's own amount stands, so switching on never lands on
        // an inaudible zero.
        if (m_beatFxDepthFromKnob)
            m_fxManager->setWetDry1(m_beatFxLevelDepth);
        if (engage == BeatFxEngage::Write)
            m_fxManager->setUnitEnabled(1, m_beatFxActive);
        if (m_beatFxActive)
            m_beatFxLevelDepth = std::clamp(m_fxManager->wetDry1(), 0.0f, 1.0f);
        const float appliedWet = m_beatFxActive ? m_beatFxLevelDepth : 0.0f;
        AudioEngine::setMasterFx(target == MidiBeatFxTarget::Master ? type : EffectType::None,
                                 target == MidiBeatFxTarget::Master ? appliedWet : 0.0f);
        m_fxManager->setPrimaryParam(1, m_beatFxLevelDepth);
        // The hardware FX unit is beat-locked: BEAT ◄ / ► picks a division and
        // the echo/delay length follows the assigned channel's tempo.
        m_fxManager->setSyncEnabled(1, true);
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

    pushBeatFxTiming();
    refreshFxLeds();
}

void MidiControllerManager::pushBeatFxTiming()
{
    // Without an FX manager there is no tempo to derive from, so the effects
    // keep their own default times.
    float seconds = -1.0f;
    if (m_fxManager) {
        m_fxManager->setBeatDivision(1, m_beatFxDivision);
        const double bpm = m_fxManager->displayBpm1();
        if (m_fxManager->syncEnabled1() && bpm > 0.0)
            seconds = static_cast<float>((60.0 / bpm) * static_cast<double>(m_beatFxDivision));
    }

    // The master bus keeps its own copy of the timing — pushSyncedDelay only
    // reaches the per-channel FX slots.
    AudioEngine::setMasterFxTiming(seconds, m_beatFxLevelDepth);
}

void MidiControllerManager::stepBeatFxDivision(int direction)
{
    // Reaching for BEAT ◄ / ► is a request for a beat-locked effect length.
    if (m_fxManager)
        m_fxManager->setSyncEnabled(1, true);

    static constexpr std::array<float, 7> kDivisions {
        0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f
    };
    const float current = m_beatFxDivision;
    const auto nearest = std::min_element(
        kDivisions.begin(), kDivisions.end(), [current](float a, float b) {
            return std::abs(a - current) < std::abs(b - current);
        });
    int index = static_cast<int>(std::distance(kDivisions.begin(), nearest)) + direction;
    index = std::clamp(index, 0, static_cast<int>(kDivisions.size()) - 1);
    m_beatFxDivision = kDivisions[static_cast<std::size_t>(index)];
    pushBeatFxTiming();
}

void MidiControllerManager::updateBeatFxBlink()
{
    // Steady, not pulsed: see the timer setup in MidiControllerManager::start.
    m_beatFxBlinkOn = m_beatFxActive;
    sendMappedNoteLed(QStringLiteral("beat_fx_on"), m_beatFxActive);
}

void MidiControllerManager::refreshFxLeds()
{
    updateBeatFxBlink();
    for (const auto& [paramId, target] : kBeatFxChannels)
        sendMappedNoteLed(QString::fromLatin1(paramId), m_beatFxTarget == target);
    for (int position = 1; position <= 14; ++position) {
        sendMappedNoteLed(QStringLiteral("beat_fx_select_%1").arg(position),
                          position == m_beatFxPosition);
    }

    const QString soundColorMode = m_fxManager
        ? m_fxManager->soundColorMode()
        : QStringLiteral("Filter");
    for (const auto& [paramId, mode] : kSoundColorModes) {
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
    // Pad FX state is currently exposed by the A/B touch surface. C/D Key
    // Shift routing is independent and must never alias those A/B latches.
    if (!engine || (deck != QLatin1Char('A') && deck != QLatin1Char('B')))
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
    if (deck != QLatin1Char('A') && deck != QLatin1Char('B'))
        return;

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

    if (mode == MidiPadMode::KeyShift) {
        // Touch input shares the same semantic route as an unshifted physical
        // Key Shift pad; secondary actions remain exclusive to shift-pad MIDI
        // channels and cannot be synthesized accidentally by the UI.
        handleKeyShiftPad(deck, engine, padIndex, pressed, false);
        return;
    }

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

void MidiControllerManager::handleKeyShiftPad(QChar deck, DjEngine* engine, int padIndex,
                                              bool pressed, bool shifted)
{
    if (!engine || padIndex < 0 || padIndex >= 8 || !pressed)
        return;

    // Shifted performance pads arrive on their own MIDI channel (8/10 for
    // normal pads, 9/11 for shifted pads). Use that normalized fact instead
    // of racing the independent deck SHIFT note against the pad event.
    if (shifted) {
        switch (padIndex) {
        case 0: { // Key Sync: match the other deck's absolute offset.
            DjEngine* other = nullptr;
            switch (deck.toUpper().toLatin1()) {
            case 'A': other = m_deckB; break;
            case 'B': other = m_deckA; break;
            case 'C': other = m_deckD; break;
            case 'D': other = m_deckC; break;
            default: break;
            }
            if (other)
                engine->setKeySemitoneOffset(other->keySemitoneOffset());
            break;
        }
        case 1: // Key Reset
            engine->setKeySemitoneOffset(0.0);
            break;
        case 6: // Range down, clamped at Down.
            stepKeyShiftRange(deck, -1);
            break;
        case 7: // Range up, clamped at Up.
            stepKeyShiftRange(deck, 1);
            break;
        default:
            break;
        }
        return;
    }

    const int range = static_cast<int>(keyShiftRangeForDeck(deck));
    engine->setKeySemitoneOffset(static_cast<double>(kFlx10KeyShiftTable[range][padIndex]));
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
        // The router measured the platter speed over a constant number of
        // ticks, which resolves a slow crawl far better than this single
        // delta divided by this single interval ever could.
        engine->scratchBySecondsTimed(route.deltaSeconds, trackingInterval,
                                      route.estimatedRate);
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
    if (m_deckA)
        m_deckA->setRealtimeScratchInput(m_realtimeScratchIngress[0].stream());
    if (m_deckB)
        m_deckB->setRealtimeScratchInput(m_realtimeScratchIngress[1].stream());
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
    const bool deckAKeyShiftRangeWasChanged =
        m_deckAKeyShiftRange != Flx10KeyShiftRange::Middle;
    const bool deckBKeyShiftRangeWasChanged =
        m_deckBKeyShiftRange != Flx10KeyShiftRange::Middle;
    m_deckAPadMode = MidiPadMode::HotCue;
    m_deckBPadMode = MidiPadMode::HotCue;
    m_deckCPadMode = MidiPadMode::HotCue;
    m_deckDPadMode = MidiPadMode::HotCue;
    m_deckAKeyShiftRange = Flx10KeyShiftRange::Middle;
    m_deckBKeyShiftRange = Flx10KeyShiftRange::Middle;
    m_deckCKeyShiftRange = Flx10KeyShiftRange::Middle;
    m_deckDKeyShiftRange = Flx10KeyShiftRange::Middle;
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
    if (deckAKeyShiftRangeWasChanged)
        emit deckAKeyShiftRangeChanged();
    if (deckBKeyShiftRangeWasChanged)
        emit deckBKeyShiftRangeChanged();

    auto wireDeckLeds = [this](QChar deck, DjEngine* engine)
    {
        if (!engine)
            return;

        QObject::connect(engine, &DjEngine::trackLoaded,
                         this, [this, deck] {
                             // A freshly loaded track starts at its own root
                             // key: Key Shift's range page resets to Middle,
                             // even though switching pad modes and back does
                             // not. The offset itself is reset by DjEngine.
                             setKeyShiftRangeForDeck(deck, Flx10KeyShiftRange::Middle);
                             refreshAllDeckLeds();
                         });
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
        QObject::connect(engine, &DjEngine::keySemitoneOffsetChanged,
                         this, [this, deck, engine] { refreshPerformancePadLeds(deck, engine); });
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
    wireDeckLeds(QLatin1Char('C'), m_deckC);
    wireDeckLeds(QLatin1Char('D'), m_deckD);
    // MASTER CUE lives on deck A's engine and can be switched from the UI too.
    if (m_deckA)
        QObject::connect(m_deckA, &DjEngine::masterCueEnabledChanged,
                         this, [this] { refreshMixerLeds(); });
    refreshAllDeckLeds();

    if (!m_parameterStore)
        return;

    if (m_deckActionsConnection)
        QObject::disconnect(m_deckActionsConnection);

    // Route ParameterStore events to deck actions.
    // Volume/crossfader/mixer EQ: MixerControl (C++) owns these.
    // Button convention: 127/1.0 = press/on, 0 = release/off.
    m_deckActionsConnection = QObject::connect(m_parameterStore, &ParameterStore::parameterChanged,
        this, [this](const QString& id, float value)
    {
        DjEngine* const a = m_deckA;
        DjEngine* const b = m_deckB;
        DjEngine* const c = m_deckC;
        DjEngine* const d = m_deckD;

        auto engineForDeck = [a, b, c, d](QChar deck) -> DjEngine*
        {
            switch (deck.toUpper().toLatin1()) {
            case 'A': return a;
            case 'B': return b;
            case 'C': return c;
            case 'D': return d;
            default: return nullptr;
            }
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

        for (const auto& [paramId, mode] : kSoundColorModes) {
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

        };

        auto handleBeatJumpParam = [&engineForDeck](const QString& paramId) -> bool
        {
            QChar deck;
            double beats = 0.0;
            if (parseDeckButtonParam(paramId, QStringLiteral("beatjump_backward"), deck)
                || parseDeckButtonParam(paramId, QStringLiteral("beatjump_4_backward"), deck)) {
                beats = -4.0;
            } else if (parseDeckButtonParam(paramId, QStringLiteral("beatjump_forward"), deck)
                       || parseDeckButtonParam(paramId, QStringLiteral("beatjump_4_forward"), deck)) {
                beats = 4.0;
            } else if (parseDeckButtonParam(paramId, QStringLiteral("beatjump_16_backward"), deck)) {
                beats = -16.0;
            } else if (parseDeckButtonParam(paramId, QStringLiteral("beatjump_16_forward"), deck)) {
                beats = 16.0;
            } else {
                return false;
            }

            if (DjEngine* const engine = engineForDeck(deck))
                engine->beatJump(beats);
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
                    a->cueButtonPress();
                }
            } else {
                m_cueAHeld = false;
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
                    b->cueButtonPress();
                }
            } else {
                m_cueBHeld = false;
                if (b)
                    b->cueButtonRelease();
            }
        }
        else if (midi::isHotCueParam(id)) {
            if (value < 0.5f)
                return;

            QChar deck;
            int hotCueIndex = -1;
            bool clear = false;
            if (!parseHotCueParam(id, deck, hotCueIndex, clear))
                return;

            DjEngine* const deckEngine = engineForDeck(deck);
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

            if (parsePadModeParam(id, deck, mode)) {
                if (value < 0.5f)
                    return;

                DjEngine* const deckEngine = engineForDeck(deck);
                if (padModeForDeck(deck) == MidiPadMode::PadFx && mode != MidiPadMode::PadFx)
                    clearPadFxState(deck, deckEngine);
                if ((padModeForDeck(deck) == MidiPadMode::HotCue
                     || padModeForDeck(deck) == MidiPadMode::Sampler)
                    && padModeForDeck(deck) != mode) {
                    releaseHeldHotCue(deck, deckEngine);
                }
                setPadModeForDeck(deck, mode);
                return;
            }

            if (parseDeckButtonParam(id, QStringLiteral("keyshift_range_down"), deck)
                || parseDeckButtonParam(id, QStringLiteral("keyshift_range_up"), deck)) {
                if (value >= 0.5f && padModeForDeck(deck) == MidiPadMode::KeyShift)
                    stepKeyShiftRange(deck, id.endsWith(QStringLiteral("_up")) ? 1 : -1);
                return;
            }

            bool shiftedKeyPad = false;
            if (parseKeyShiftPadParam(id, deck, padIndex, shiftedKeyPad)) {
                DjEngine* const deckEngine = engineForDeck(deck);
                if (value >= 0.5f && padModeForDeck(deck) != MidiPadMode::KeyShift) {
                    if (padModeForDeck(deck) == MidiPadMode::PadFx)
                        clearPadFxState(deck, deckEngine);
                    releaseHeldHotCue(deck, deckEngine);
                    setPadModeForDeck(deck, MidiPadMode::KeyShift);
                }
                handleKeyShiftPad(deck, deckEngine, padIndex, value >= 0.5f,
                                  shiftedKeyPad);
                return;
            }

            if (parsePerformancePadParam(id, deck, padIndex, clearPad)) {
                DjEngine* const deckEngine = engineForDeck(deck);
                // Some FLX10 firmware/host combinations keep reporting the
                // physical pad through the Hot Cue bank briefly after 0x6f.
                // The explicit mode-select command remains authoritative. This
                // also stops that first stale pad packet from switching the app
                // straight back to Hot Cue. The shifted pad channel is already
                // normalized as clearPad by this legacy mapping.
                if (padModeForDeck(deck) == MidiPadMode::KeyShift) {
                    handleKeyShiftPad(deck, deckEngine, padIndex,
                                      value >= 0.5f, clearPad);
                    return;
                }
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

            if (parseDirectPadParam(id, QStringLiteral("padfx_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = engineForDeck(deck);
                if (value >= 0.5f && padModeForDeck(deck) != MidiPadMode::PadFx) {
                    releaseHeldHotCue(deck, deckEngine);
                    setPadModeForDeck(deck, MidiPadMode::PadFx);
                }
                handlePerformancePad(deck, deckEngine, MidiPadMode::PadFx,
                                     padIndex, value >= 0.5f, false);
                return;
            }

            if (parseDirectPadParam(id, QStringLiteral("beatjump_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = engineForDeck(deck);
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

            if (parseDirectPadParam(id, QStringLiteral("sampler_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = engineForDeck(deck);
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
            if (parseDeckFxSlotParam(id, deck, fxSlot)) {
                if (value >= 0.5f)
                    toggleFxSlot(deck, fxSlot);
                return;
            }
        }
        QChar directDeck;
        if (value >= 0.5f && handleBeatJumpParam(id))
            return;
        int beatFxPosition = -1;
        if (parseBeatFxSelectParam(id, beatFxPosition)) {
            if (value >= 0.5f) {
                m_beatFxPosition = beatFxPosition;
                applyBeatFxState();
            }
            return;
        }
        for (const auto& [paramId, target] : kBeatFxChannels) {
            if (id == QLatin1String(paramId)) {
                if (value >= 0.5f) {
                    m_beatFxTarget = target;
                    applyBeatFxState();
                }
                return;
            }
        }
        if (id == QStringLiteral("beat_fx_level_depth")) {
            m_beatFxLevelDepth = std::clamp(value, 0.0f, 1.0f);
            m_beatFxDepthFromKnob = true;
            if (m_beatFxActive)
                applyBeatFxState();
            return;
        }
        if (id == QStringLiteral("beat_fx_beat_minus")
            || id == QStringLiteral("beat_fx_beat_plus")) {
            if (value >= 0.5f)
                stepBeatFxDivision(id.endsWith(QStringLiteral("plus")) ? 1 : -1);
            return;
        }
        if (id == QStringLiteral("beat_fx_on")) {
            // This button latches in the hardware and reports the resulting
            // state, not the finger: one press sends velocity 127, the next
            // press sends velocity 0. There is no release event at all. Treating
            // it as momentary and flipping our own flag on the press edge
            // therefore ignored every switch-off and inverted the two sides
            // against each other. The reported state is simply adopted.
            const bool active = value >= 0.5f;
            if (m_beatFxActive != active) {
                m_beatFxActive = active;
                emit beatFxActiveChanged();
            }
            applyBeatFxState(BeatFxEngage::Write);
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("beat_sync"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setSyncEnabled(!engine->syncEnabled());
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("beatsync"), directDeck)) {
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
        if (parseDeckButtonParam(id, QStringLiteral("key_sync"), directDeck)
            || parseDeckButtonParam(id, QStringLiteral("keylock"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setKeylock(!engine->keylock());
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("slip"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setSlip(!engine->slipActive());
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("quantize"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setQuantizeEnabled(!engine->quantizeEnabled());
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("slip_reverse"), directDeck)) {
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
        if (parseDeckButtonParam(id, QStringLiteral("tempo_range_cycle"), directDeck)) {
            if (value >= 0.5f)
                cycleFlx10TempoRange(engineForDeck(directDeck));
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("tempo_reset"), directDeck)
            || parseDeckButtonParam(id, QStringLiteral("rate_reset"), directDeck)) {
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
        // Trim/EQ/filter: MixerControl applies these for all four decks.
        // Tempo fader: MIDI 0-1 -> current deck tempo range.
        else if (id == "deckA_tempo")  {
            if (a) {
                const double range = std::max(1.0, a->tempoRangePercent());
                const double percent = static_cast<double>(value) * 2.0 * range - range;
                if (!m_tempoInputSeen[0]) {
                    m_tempoInputSeen[0] = true;
                    if (m_midiTraceEnabled)
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
                    if (m_midiTraceEnabled)
                        qInfo() << "[MIDI IN] FLX10 tempo fader deck B detected"
                                << "normalized:" << value << "tempo:" << percent;
                }
                b->setTempoPercent(percent);
            }
        }
    });
}
