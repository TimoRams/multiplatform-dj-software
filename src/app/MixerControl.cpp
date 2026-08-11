#include "MixerControl.h"

#include "DeckChannels.h"
#include "deck/DjEngine.h"
#include "audio/AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace {

CrossfaderAssignment assignmentFromString(const QString& assignment)
{
    if (assignment == QLatin1String("A"))
        return CrossfaderAssignment::A;
    if (assignment == QLatin1String("B"))
        return CrossfaderAssignment::B;
    return CrossfaderAssignment::Thru;
}

CrossfaderCurve curveFromState(const QString& mode, float sharpness)
{
    if (mode == QLatin1String("linear"))
        return CrossfaderCurve::Smooth;
    return sharpness >= 0.75f ? CrossfaderCurve::Scratch
                             : CrossfaderCurve::ConstantPower;
}

} // namespace

MixerControl::MixerControl(QObject* parent)
    : QObject(parent)
{
}

void MixerControl::setDecks(DjEngine* deckA, DjEngine* deckB, DjEngine* deckC, DjEngine* deckD)
{
    m_deckA = deckA;
    m_deckB = deckB;
    m_deckC = deckC;
    m_deckD = deckD;

    applyAllMixState();
    applyAllVolumes();
}

DjEngine* MixerControl::deckForChannelId(const QString& channelId) const
{
    return ::deckForChannelId(channelId, m_deckA, m_deckB, m_deckC, m_deckD);
}

MixerControl::ChannelMixState* MixerControl::mixStateForChannel(const QString& channelId)
{
    if (channelId == QLatin1String("deckA")) return &m_mixA;
    if (channelId == QLatin1String("deckB")) return &m_mixB;
    if (channelId == QLatin1String("deckC")) return &m_mixC;
    if (channelId == QLatin1String("deckD")) return &m_mixD;
    return nullptr;
}

const MixerControl::ChannelMixState* MixerControl::mixStateForChannel(const QString& channelId) const
{
    if (channelId == QLatin1String("deckA")) return &m_mixA;
    if (channelId == QLatin1String("deckB")) return &m_mixB;
    if (channelId == QLatin1String("deckC")) return &m_mixC;
    if (channelId == QLatin1String("deckD")) return &m_mixD;
    return nullptr;
}

void MixerControl::applyChannelVolume(const QString& channelId)
{
    DjEngine* const deck = deckForChannelId(channelId);
    if (!deck)
        return;

    float fader = 1.0f;
    if (channelId == QLatin1String("deckA")) fader = m_faderA;
    else if (channelId == QLatin1String("deckB")) fader = m_faderB;
    else if (channelId == QLatin1String("deckC")) fader = m_faderC;
    else if (channelId == QLatin1String("deckD")) fader = m_faderD;

    deck->applyVolume(static_cast<double>(fader));
}

void MixerControl::applyChannelMixState(const QString& channelId)
{
    DjEngine* const deck = deckForChannelId(channelId);
    const ChannelMixState* const state = mixStateForChannel(channelId);
    if (!deck || !state)
        return;

    deck->applyTrim(state->trim);
    deck->applyEqHigh(state->eqHigh);
    deck->applyEqMid(state->eqMid);
    deck->applyEqLow(state->eqLow);
    deck->applyFilter(state->filter);
    deck->applyPolarityInverted(state->polarityInverted);
}

void MixerControl::setTrim(const QString& channelId, double value)
{
    if (ChannelMixState* const state = mixStateForChannel(channelId))
        state->trim = value;
    if (DjEngine* const deck = deckForChannelId(channelId))
        deck->applyTrim(value);
}

void MixerControl::setEqHigh(const QString& channelId, double value)
{
    if (ChannelMixState* const state = mixStateForChannel(channelId))
        state->eqHigh = value;
    if (DjEngine* const deck = deckForChannelId(channelId))
        deck->applyEqHigh(value);
}

void MixerControl::setEqMid(const QString& channelId, double value)
{
    if (ChannelMixState* const state = mixStateForChannel(channelId))
        state->eqMid = value;
    if (DjEngine* const deck = deckForChannelId(channelId))
        deck->applyEqMid(value);
}

void MixerControl::setEqLow(const QString& channelId, double value)
{
    if (ChannelMixState* const state = mixStateForChannel(channelId))
        state->eqLow = value;
    if (DjEngine* const deck = deckForChannelId(channelId))
        deck->applyEqLow(value);
}

void MixerControl::setFilter(const QString& channelId, double value)
{
    if (ChannelMixState* const state = mixStateForChannel(channelId))
        state->filter = value;
    if (DjEngine* const deck = deckForChannelId(channelId))
        deck->applyFilter(value);
}

void MixerControl::setPolarityInverted(const QString& channelId, bool inverted)
{
    if (ChannelMixState* const state = mixStateForChannel(channelId))
        state->polarityInverted = inverted;
    if (DjEngine* const deck = deckForChannelId(channelId))
        deck->applyPolarityInverted(inverted);
}

void MixerControl::toggleCue(const QString& channelId)
{
    if (DjEngine* const deck = deckForChannelId(channelId))
        deck->setCueEnabled(!deck->cueEnabled());
}

void MixerControl::setChannelFader(const QString& channelId, double level)
{
    const float clamped = std::clamp(static_cast<float>(level), 0.0f, 1.0f);
    if (channelId == QLatin1String("deckA")) m_faderA = clamped;
    else if (channelId == QLatin1String("deckB")) m_faderB = clamped;
    else if (channelId == QLatin1String("deckC")) m_faderC = clamped;
    else if (channelId == QLatin1String("deckD")) m_faderD = clamped;
    else return;

    applyChannelVolume(channelId);
}

void MixerControl::setCrossfaderPosition(float cfPos)
{
    m_cfPos = std::clamp(cfPos, -1.0f, 1.0f);
    AudioEngine::setCrossfaderPosition(m_cfPos);
}

void MixerControl::syncCrossfaderState(float cfPos,
                                       const QString& assignA,
                                       const QString& assignB,
                                       const QString& assignC,
                                       const QString& assignD,
                                       float cfSharpness,
                                       const QString& cfCurveMode)
{
    m_cfPos = cfPos;
    m_assignA = assignA;
    m_assignB = assignB;
    m_assignC = assignC;
    m_assignD = assignD;
    m_cfSharpness = cfSharpness;
    m_cfCurveMode = cfCurveMode;
    AudioEngine::setCrossfaderPosition(std::clamp(m_cfPos, -1.0f, 1.0f));
    AudioEngine::setCrossfaderCurve(curveFromState(m_cfCurveMode, m_cfSharpness));
    AudioEngine::setCrossfaderAssignment(0, assignmentFromString(m_assignA));
    AudioEngine::setCrossfaderAssignment(1, assignmentFromString(m_assignB));
    AudioEngine::setCrossfaderAssignment(2, assignmentFromString(m_assignC));
    AudioEngine::setCrossfaderAssignment(3, assignmentFromString(m_assignD));
}

void MixerControl::applyAllVolumes()
{
    applyChannelVolume(QStringLiteral("deckA"));
    applyChannelVolume(QStringLiteral("deckB"));
    applyChannelVolume(QStringLiteral("deckC"));
    applyChannelVolume(QStringLiteral("deckD"));
}

void MixerControl::applyAllMixState()
{
    applyChannelMixState(QStringLiteral("deckA"));
    applyChannelMixState(QStringLiteral("deckB"));
    applyChannelMixState(QStringLiteral("deckC"));
    applyChannelMixState(QStringLiteral("deckD"));
}

double MixerControl::faderLevel(const QString& channelId) const
{
    if (channelId == QLatin1String("deckA")) return m_faderA;
    if (channelId == QLatin1String("deckB")) return m_faderB;
    if (channelId == QLatin1String("deckC")) return m_faderC;
    if (channelId == QLatin1String("deckD")) return m_faderD;
    return 1.0;
}

void MixerControl::syncMixFromNormalized(const QString& channelId,
                                         const QString& suffix,
                                         float normalized)
{
    ChannelMixState* const state = mixStateForChannel(channelId);
    if (!state)
        return;

    if (suffix == QLatin1String("gain"))
        state->trim = mixerTrimFromNormalized(normalized);
    else if (suffix == QLatin1String("eqHigh"))
        state->eqHigh = mixerBipolarFromNormalized(normalized);
    else if (suffix == QLatin1String("eqMid"))
        state->eqMid = mixerBipolarFromNormalized(normalized);
    else if (suffix == QLatin1String("eqLow"))
        state->eqLow = mixerBipolarFromNormalized(normalized);
    else if (suffix == QLatin1String("filter"))
        state->filter = mixerBipolarFromNormalized(normalized);
}
