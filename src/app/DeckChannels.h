#pragma once

#include <QString>

class DjEngine;

// Shared deckA–deckD lookup used by MixerControl, MixerParameterBridge, FxManager, etc.
[[nodiscard]] inline DjEngine* deckForChannelId(const QString& channelId,
                                                DjEngine* deckA,
                                                DjEngine* deckB,
                                                DjEngine* deckC,
                                                DjEngine* deckD) noexcept
{
    if (channelId == QLatin1String("deckA")) return deckA;
    if (channelId == QLatin1String("deckB")) return deckB;
    if (channelId == QLatin1String("deckC")) return deckC;
    if (channelId == QLatin1String("deckD")) return deckD;
    return nullptr;
}

// ParameterStore / MIDI normalized 0–1 → engine mixer ranges.
[[nodiscard]] inline double mixerTrimFromNormalized(float normalized) noexcept
{
    return static_cast<double>(normalized) * 2.0;
}

[[nodiscard]] inline double mixerBipolarFromNormalized(float normalized) noexcept
{
    return static_cast<double>(normalized) * 2.0 - 1.0;
}
