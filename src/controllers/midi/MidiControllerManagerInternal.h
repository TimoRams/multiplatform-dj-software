#pragma once

#include "MidiControllerManager.h"
#include "deck/DjEngine.h"
#include "controllers/flx10/Flx10JogRouter.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <ranges>

namespace midi_internal {
inline const ::juce::String kAllMidiInputsIdentifier("__all_midi_inputs__");
inline const QString kBuiltInFlx10ControllerName = QStringLiteral("DDJ-FLX10");
inline const QString kBuiltInFlx10MappingFile = QStringLiteral("DDJ-FLX10.brockdj.xml");
inline const QString kBuiltInFlx10MappingLabel = QStringLiteral("Built-in: DDJ-FLX10");
inline const QString kBuiltInFlx10MappingResource = QStringLiteral(":/controllers/mappings/midi/DDJ-FLX10.brockdj.xml");
struct SoundColorModeMapping {
    const char* paramId;
    const char* mode;
};
inline constexpr std::array<SoundColorModeMapping, 6> kSoundColorModes {{
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
inline constexpr std::array<BeatFxChannelMapping, 7> kBeatFxChannels {{
    { "beat_fx_channel_deck_a", MidiBeatFxTarget::DeckA },
    { "beat_fx_channel_deck_b", MidiBeatFxTarget::DeckB },
    { "beat_fx_channel_deck_c", MidiBeatFxTarget::DeckC },
    { "beat_fx_channel_deck_d", MidiBeatFxTarget::DeckD },
    { "beat_fx_channel_master", MidiBeatFxTarget::Master },
    { "beat_fx_channel_mic", MidiBeatFxTarget::Mic },
    { "beat_fx_channel_sampler", MidiBeatFxTarget::Sampler }
}};
inline int beatFxDeckNumber(MidiBeatFxTarget target)
{
    switch (target) {
    case MidiBeatFxTarget::DeckA: return 1;
    case MidiBeatFxTarget::DeckB: return 2;
    case MidiBeatFxTarget::DeckC: return 3;
    case MidiBeatFxTarget::DeckD: return 4;
    default: return 0;
    }
}

inline const char* beatFxTargetName(MidiBeatFxTarget target)
{
    switch (target) {
    case MidiBeatFxTarget::DeckA: return "deckA";
    case MidiBeatFxTarget::DeckB: return "deckB";
    case MidiBeatFxTarget::DeckC: return "deckC";
    case MidiBeatFxTarget::DeckD: return "deckD";
    case MidiBeatFxTarget::Master: return "master";
    case MidiBeatFxTarget::Mic: return "mic";
    case MidiBeatFxTarget::Sampler: return "sampler";
    }
    return "unknown";
}
inline bool isBuiltInFlx10Mapping(const QString& mappingName)
{
    return mappingName == kBuiltInFlx10MappingLabel
        || mappingName == kBuiltInFlx10MappingFile;
}

inline bool openDirectoryInFileManager(const QString& path)
{
    if (path.isEmpty())
        return false;

    QDir dir(path);
    if (!dir.exists() && !dir.mkpath("."))
        return false;

    const QString nativePath = QDir::toNativeSeparators(dir.absolutePath());

#if defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("open"), { nativePath }))
        return true;
#elif defined(Q_OS_WIN)
    if (QProcess::startDetached(QStringLiteral("explorer.exe"), { nativePath }))
        return true;
#elif defined(Q_OS_UNIX)
    if (QProcess::startDetached(QStringLiteral("xdg-open"), { nativePath }))
        return true;
#endif

    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

inline bool isHotCueParam(const QString& paramId)
{
    return paramId.startsWith(QStringLiteral("deckA_hotcue"))
        || paramId.startsWith(QStringLiteral("deckB_hotcue"));
}

inline bool isPerformancePadParam(const QString& paramId)
{
    return paramId.startsWith(QStringLiteral("deckA_pad"))
        || paramId.startsWith(QStringLiteral("deckB_pad"));
}

inline bool parsePerformancePadParam(const QString& paramId, QChar& deck, int& index, bool& clear)
{
    QString suffix;
    if (paramId.startsWith(QStringLiteral("deckA_pad"))) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(QStringLiteral("deckA_pad").size());
    } else if (paramId.startsWith(QStringLiteral("deckB_pad"))) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(QStringLiteral("deckB_pad").size());
    } else {
        return false;
    }

    clear = suffix.endsWith(QStringLiteral("_clear"));
    if (clear)
        suffix.chop(QStringLiteral("_clear").size());

    bool ok = false;
    index = suffix.toInt(&ok) - 1;
    return ok && index >= 0 && index < 8;
}

inline bool parseDirectPadParam(const QString& paramId, const QString& stem, QChar& deck, int& index)
{
    QString suffix;
    const QString deckAStem = QStringLiteral("deckA_") + stem;
    const QString deckBStem = QStringLiteral("deckB_") + stem;
    if (paramId.startsWith(deckAStem)) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(deckAStem.size());
    } else if (paramId.startsWith(deckBStem)) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(deckBStem.size());
    } else {
        return false;
    }

    bool ok = false;
    index = suffix.toInt(&ok) - 1;
    return ok && index >= 0 && index < 8;
}

inline bool parsePadModeParam(const QString& paramId, QChar& deck, MidiPadMode& mode)
{
    QString suffix;
    if (paramId.startsWith(QStringLiteral("deckA_pad_mode_"))) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(QStringLiteral("deckA_pad_mode_").size());
    } else if (paramId.startsWith(QStringLiteral("deckB_pad_mode_"))) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(QStringLiteral("deckB_pad_mode_").size());
    } else {
        return false;
    }

    if (suffix == QStringLiteral("hotcue")) {
        mode = MidiPadMode::HotCue;
        return true;
    }
    if (suffix == QStringLiteral("padfx")) {
        mode = MidiPadMode::PadFx;
        return true;
    }
    if (suffix == QStringLiteral("beatjump")) {
        mode = MidiPadMode::BeatJump;
        return true;
    }
    if (suffix == QStringLiteral("sampler")) {
        mode = MidiPadMode::Sampler;
        return true;
    }
    return false;
}

inline bool parseDeckButtonParam(const QString& paramId, const QString& suffix, QChar& deck)
{
    if (paramId == QStringLiteral("deckA_") + suffix) {
        deck = QLatin1Char('A');
        return true;
    }
    if (paramId == QStringLiteral("deckB_") + suffix) {
        deck = QLatin1Char('B');
        return true;
    }
    return false;
}

inline bool parseDeckFxSlotParam(const QString& paramId, QChar& deck, int& slot)
{
    QString suffix;
    if (paramId.startsWith(QStringLiteral("deckA_fx_slot"))) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(QStringLiteral("deckA_fx_slot").size());
    } else if (paramId.startsWith(QStringLiteral("deckB_fx_slot"))) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(QStringLiteral("deckB_fx_slot").size());
    } else {
        return false;
    }

    bool ok = false;
    slot = suffix.toInt(&ok);
    return ok && slot >= 1 && slot <= 3;
}

inline bool parseBeatFxSelectParam(const QString& paramId, int& position)
{
    if (!paramId.startsWith(QStringLiteral("beat_fx_select_")))
        return false;

    bool ok = false;
    position = paramId.mid(QStringLiteral("beat_fx_select_").size()).toInt(&ok);
    return ok && position >= 1 && position <= 14;
}

inline EffectType beatFxTypeForPosition(int position)
{
    static constexpr EffectType kTypes[14] = {
        EffectType::Echo,
        EffectType::LowCutEcho,
        EffectType::MtDelay,
        EffectType::Spiral,
        EffectType::Reverb,
        EffectType::Trans,
        EffectType::Flanger,
        EffectType::Phaser,
        EffectType::Bitcrusher,
        EffectType::PitchShifter,
        EffectType::Stretch,
        EffectType::EnigmaJet,
        EffectType::Roll,
        EffectType::SlipRoll
    };

    return kTypes[static_cast<size_t>(std::clamp(position, 1, 14) - 1)];
}

inline QString beatFxNameForPosition(int position)
{
    static const std::array<QString, 14> kNames = {
        QStringLiteral("Echo"),
        QStringLiteral("Low Cut Echo"),
        QStringLiteral("MT Delay"),
        QStringLiteral("Spiral"),
        QStringLiteral("Reverb"),
        QStringLiteral("Trans"),
        QStringLiteral("Flanger"),
        QStringLiteral("Phaser"),
        QStringLiteral("Bitcrusher"),
        QStringLiteral("Pitch Shifter"),
        QStringLiteral("Stretch"),
        QStringLiteral("Enigma Jet"),
        QStringLiteral("Roll"),
        QStringLiteral("Slip Roll")
    };
    return kNames[static_cast<size_t>(std::clamp(position, 1, 14) - 1)];
}

inline int beatFxPositionForName(const QString& name)
{
    for (int position = 1; position <= 14; ++position) {
        if (beatFxNameForPosition(position) == name)
            return position;
    }
    return -1;
}

inline bool parseHotCueParam(const QString& paramId, QChar& deck, int& index, bool& clear)
{
    QString suffix;
    if (paramId.startsWith(QStringLiteral("deckA_hotcue"))) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(QStringLiteral("deckA_hotcue").size());
    } else if (paramId.startsWith(QStringLiteral("deckB_hotcue"))) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(QStringLiteral("deckB_hotcue").size());
    } else {
        return false;
    }

    clear = suffix.endsWith(QStringLiteral("_clear"));
    if (clear)
        suffix.chop(QStringLiteral("_clear").size());

    bool ok = false;
    index = suffix.toInt(&ok) - 1;
    return ok && index >= 0 && index < 8;
}

inline QString toQString(const ::juce::String& value)
{
    return QString::fromStdString(value.toStdString());
}

inline QString hexByte(int value)
{
    return QStringLiteral("%1")
        .arg(value & 0xff, 2, 16, QLatin1Char('0'))
        .toUpper();
}

inline int clampMidi7bit(int value)
{
    return std::max(0, std::min(127, value));
}

inline bool containsIdentifier(const std::vector<::juce::String>& ids, const ::juce::String& needle)
{
    return std::ranges::find(ids, needle) != ids.end();
}

inline MidiInteractionType defaultInteractionTypeForParam(const QString& paramId)
{
    if (paramId == QStringLiteral("deckA_cue")
        || paramId == QStringLiteral("deckB_cue")
        || paramId == QStringLiteral("deckA_jog_touch")
        || paramId == QStringLiteral("deckB_jog_touch")
        || paramId == QStringLiteral("deckA_shift")
        || paramId == QStringLiteral("deckB_shift")
        || paramId == QStringLiteral("deckA_slip_reverse")
        || paramId == QStringLiteral("deckB_slip_reverse")
        || paramId == QStringLiteral("beat_fx_on")
        || paramId == QStringLiteral("beat_fx_beat_minus")
        || paramId == QStringLiteral("beat_fx_beat_plus")
        || paramId.startsWith(QStringLiteral("deckA_sampler_pad"))
        || paramId.startsWith(QStringLiteral("deckB_sampler_pad"))
        || (isPerformancePadParam(paramId)
            && !paramId.contains(QStringLiteral("_pad_mode_"))
            && !paramId.endsWith(QStringLiteral("_clear")))) {
        return MidiInteractionType::Momentary;
    }

    if (paramId == QStringLiteral("deckA_play")
        || paramId == QStringLiteral("deckB_play")
        || isHotCueParam(paramId)
        || (isPerformancePadParam(paramId) && paramId.endsWith(QStringLiteral("_clear")))
        || paramId.startsWith(QStringLiteral("deckA_pad_mode_"))
        || paramId.startsWith(QStringLiteral("deckB_pad_mode_"))
        || paramId.startsWith(QStringLiteral("deckA_loop_"))
        || paramId.startsWith(QStringLiteral("deckB_loop_"))
        || paramId.startsWith(QStringLiteral("deckA_beatjump_"))
        || paramId.startsWith(QStringLiteral("deckB_beatjump_"))
        || paramId.startsWith(QStringLiteral("deckA_beatjump_pad"))
        || paramId.startsWith(QStringLiteral("deckB_beatjump_pad"))
        || paramId.startsWith(QStringLiteral("deckA_padfx_pad"))
        || paramId.startsWith(QStringLiteral("deckB_padfx_pad"))
        || paramId.startsWith(QStringLiteral("deckA_fx_slot"))
        || paramId.startsWith(QStringLiteral("deckB_fx_slot"))
        || paramId == QStringLiteral("deckA_beat_sync")
        || paramId == QStringLiteral("deckB_beat_sync")
        || paramId == QStringLiteral("deckA_beatsync")
        || paramId == QStringLiteral("deckB_beatsync")
        || paramId == QStringLiteral("deckA_key_sync")
        || paramId == QStringLiteral("deckB_key_sync")
        || paramId == QStringLiteral("deckA_keylock")
        || paramId == QStringLiteral("deckB_keylock")
        || paramId == QStringLiteral("deckA_quantize")
        || paramId == QStringLiteral("deckB_quantize")
        || paramId == QStringLiteral("deckA_slip")
        || paramId == QStringLiteral("deckB_slip")
        || paramId == QStringLiteral("deckA_tempo_reset")
        || paramId == QStringLiteral("deckB_tempo_reset")
        || paramId == QStringLiteral("deckA_rate_reset")
        || paramId == QStringLiteral("deckB_rate_reset")
        || paramId == QStringLiteral("deckA_tempo_range_cycle")
        || paramId == QStringLiteral("deckB_tempo_range_cycle")
        || paramId.startsWith(QStringLiteral("beat_fx_"))
        || paramId == QStringLiteral("deckA_headphone_cue")
        || paramId == QStringLiteral("deckB_headphone_cue")
        || paramId == QStringLiteral("deckC_headphone_cue")
        || paramId == QStringLiteral("deckD_headphone_cue")
        || paramId == QStringLiteral("master_cue")
        || paramId.startsWith(QStringLiteral("library_load_"))
        || paramId == QStringLiteral("library_back")
        || paramId == QStringLiteral("library_expand")
        || paramId == QStringLiteral("library_collapse")
        || paramId == QStringLiteral("library_playlist_next")
        || paramId == QStringLiteral("library_playlist_prev")
        || paramId == QStringLiteral("library_view_toggle")
        || paramId.startsWith(QStringLiteral("sound_color_fx_"))) {
        return MidiInteractionType::Toggle;
    }

    if (paramId.endsWith(QStringLiteral("_jog_move"))
        || paramId.endsWith(QStringLiteral("_jog_nudge"))
        || paramId.endsWith(QStringLiteral("_jog_scratch"))
        || paramId == QStringLiteral("library_browse")) {
        return MidiInteractionType::EncoderRelative;
    }

    if (paramId == QStringLiteral("deckA_vol")
        || paramId == QStringLiteral("deckB_vol")
        || paramId == QStringLiteral("crossfader")
        || paramId == QStringLiteral("master_level")
        || paramId == QStringLiteral("headphone_mix")
        || paramId == QStringLiteral("headphone_level")
        || paramId == QStringLiteral("deckA_tempo")
        || paramId == QStringLiteral("deckB_tempo")) {
        return MidiInteractionType::Fader;
    }

    return MidiInteractionType::EncoderAbsolute;
}

inline QString interactionTypeToString(MidiInteractionType type)
{
    switch (type) {
    case MidiInteractionType::Momentary:
        return QStringLiteral("momentary");
    case MidiInteractionType::Toggle:
        return QStringLiteral("toggle");
    case MidiInteractionType::EncoderRelative:
        return QStringLiteral("encoder-relative");
    case MidiInteractionType::EncoderAbsolute:
        return QStringLiteral("encoder-absolute");
    case MidiInteractionType::Fader:
        return QStringLiteral("fader");
    }
    return QStringLiteral("encoder-absolute");
}

inline MidiInteractionType interactionTypeFromString(const QString& rawValue,
                                              const QString& paramId)
{
    const QString value = rawValue.trimmed().toLower();
    if (value == QStringLiteral("momentary"))
        return MidiInteractionType::Momentary;
    if (value == QStringLiteral("toggle"))
        return MidiInteractionType::Toggle;
    if (value == QStringLiteral("encoder-relative"))
        return MidiInteractionType::EncoderRelative;
    if (value == QStringLiteral("encoder-absolute"))
        return MidiInteractionType::EncoderAbsolute;
    if (value == QStringLiteral("fader"))
        return MidiInteractionType::Fader;
    return defaultInteractionTypeForParam(paramId);
}

inline bool isButtonInteraction(MidiInteractionType type)
{
    return type == MidiInteractionType::Momentary
        || type == MidiInteractionType::Toggle;
}

inline bool shouldAlwaysDispatch(MidiInteractionType type)
{
    return type == MidiInteractionType::Momentary
        || type == MidiInteractionType::Toggle
        || type == MidiInteractionType::EncoderRelative;
}

inline bool isRelativeInteraction(MidiInteractionType type)
{
    return type == MidiInteractionType::EncoderRelative;
}

inline bool isFlx10JogRelativeParam(const QString& paramId)
{
    return paramId.endsWith(QStringLiteral("_jog_move"))
        || paramId.endsWith(QStringLiteral("_jog_nudge"))
        || paramId.endsWith(QStringLiteral("_jog_scratch"));
}

inline bool isFlx10JogInputParam(const QString& paramId)
{
    return isFlx10JogRelativeParam(paramId)
        || paramId.endsWith(QStringLiteral("_jog_touch"));
}

inline float decodeRelativeCcValue(int rawValue, const QString& paramId)
{
    const int raw = clampMidi7bit(rawValue);
    if (isFlx10JogRelativeParam(paramId)) {
        // FLX10 jog CCs are relative around 0x40 for the full 7-bit range:
        // 0x40 = neutral, 0x41..0x7F = +1..+63, 0x3F..0x01 = -1..-63.
        // Treating 0x7F as two's-complement -1 makes fast spins reverse direction.
        return static_cast<float>(flx10::relativeTicksFromRaw(raw));
    }

    // Support the two common relative CC encodings for generic encoders:
    // - binary offset: 64 = neutral, 65..96 = +1..+32, 63..32 = -1..-32
    // - two's complement: 1..63 = +1..+63, 65..127 = -63..-1
    if (raw >= 32 && raw <= 96)
        return static_cast<float>(raw - 64);
    return static_cast<float>(raw < 64 ? raw : raw - 128);
}

inline float decodeWrappedAbsoluteDelta(int previousRaw, int currentRaw)
{
    int delta = clampMidi7bit(currentRaw) - clampMidi7bit(previousRaw);
    if (delta > 64)
        delta -= 128;
    else if (delta < -64)
        delta += 128;
    return static_cast<float>(delta);
}

inline MidiMappingEntry makeMappingEntry(const QString& paramId)
{
    return { paramId, defaultInteractionTypeForParam(paramId) };
}

inline MidiMappingEntry makeMappingEntry(const QString& paramId,
                                  MidiInteractionType type)
{
    return { paramId, type };
}

inline QString midiControlLabel(int msgId)
{
    if (msgId >= 10000) {
        const int remainder = msgId - 10000;
        const int channel = remainder / 2000;
        const int sub = remainder % 2000;
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

inline int indexOfIdentifier(const std::vector<::juce::String>& ids, const ::juce::String& needle)
{
    const auto it = std::ranges::find(ids, needle);
    if (it == ids.end())
        return -1;
    return static_cast<int>(std::distance(ids.begin(), it));
}

inline QString midiMatchKey(QString value)
{
    value = value.toLower();
    value.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QString());
    return value;
}

inline bool looksLikeFlx10Name(const QString& value)
{
    const QString key = midiMatchKey(value);
    return key.contains(QStringLiteral("flx10"))
        || key.contains(QStringLiteral("ddjflx10"));
}

template <typename DeviceList>
inline void appendMidiDeviceNames(const DeviceList& devices,
                           std::vector<::juce::String>& outIdentifiers,
                           QStringList& outNames)
{
    outIdentifiers.reserve(outIdentifiers.size() + devices.size());
    for (const auto& dev : devices) {
        outIdentifiers.push_back(dev.identifier);
        outNames.push_back(toQString(dev.name));
    }
}

} // namespace midi_internal
