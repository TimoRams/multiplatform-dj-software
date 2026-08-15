#pragma once

#include "MidiControllerManager.h"
#include "controllers/flx10/Flx10JogRouter.h"

#include <QString>

#include <algorithm>

// How a mapped MIDI parameter behaves: whether it latches, what its raw value
// means, and how a controller's encoders should be read. Owned by
// MidiParameterDispatch.cpp, which is the only place that acts on it; the
// mapping loader and the FLX10 bridge read it so a parameter has one set of
// semantics no matter which of them touched it.
namespace midi {

constexpr int clampMidi7bit(int value) noexcept
{
    return std::clamp(value, 0, 127);
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

    if (isFlx10JogRelativeParam(paramId) || paramId == QStringLiteral("library_browse"))
        return MidiInteractionType::EncoderRelative;

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
    case MidiInteractionType::Momentary:       return QStringLiteral("momentary");
    case MidiInteractionType::Toggle:          return QStringLiteral("toggle");
    case MidiInteractionType::EncoderRelative: return QStringLiteral("encoder-relative");
    case MidiInteractionType::EncoderAbsolute: return QStringLiteral("encoder-absolute");
    case MidiInteractionType::Fader:           return QStringLiteral("fader");
    }
    return QStringLiteral("encoder-absolute");
}

inline MidiInteractionType interactionTypeFromString(const QString& rawValue,
                                                     const QString& paramId)
{
    const QString value = rawValue.trimmed().toLower();
    if (value == QStringLiteral("momentary"))        return MidiInteractionType::Momentary;
    if (value == QStringLiteral("toggle"))           return MidiInteractionType::Toggle;
    if (value == QStringLiteral("encoder-relative")) return MidiInteractionType::EncoderRelative;
    if (value == QStringLiteral("encoder-absolute")) return MidiInteractionType::EncoderAbsolute;
    if (value == QStringLiteral("fader"))            return MidiInteractionType::Fader;
    return defaultInteractionTypeForParam(paramId);
}

constexpr bool isButtonInteraction(MidiInteractionType type) noexcept
{
    return type == MidiInteractionType::Momentary || type == MidiInteractionType::Toggle;
}

constexpr bool shouldAlwaysDispatch(MidiInteractionType type) noexcept
{
    return isButtonInteraction(type) || type == MidiInteractionType::EncoderRelative;
}

constexpr bool isRelativeInteraction(MidiInteractionType type) noexcept
{
    return type == MidiInteractionType::EncoderRelative;
}

inline float decodeRelativeCcValue(int rawValue, const QString& paramId)
{
    const int raw = clampMidi7bit(rawValue);
    if (isFlx10JogRelativeParam(paramId)) {
        // Jog CCs on this hardware are relative around 0x40 across the full
        // 7-bit range: 0x40 = neutral, 0x41..0x7F = +1..+63, 0x3F..0x01 =
        // -1..-63. Treating 0x7F as two's-complement -1 would make fast spins
        // reverse direction.
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

inline MidiMappingEntry makeMappingEntry(const QString& paramId, MidiInteractionType type)
{
    return { paramId, type };
}

} // namespace midi
