#include "MixerControl.h"

#include "audio/AudioEngine.h"
#include "controllers/midi/ParameterStore.h"
#include "deck/DjEngine.h"

#include <algorithm>
#include <cmath>

using domain::DeckId;

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

// ParameterStore / MIDI normalized 0–1 → engine mixer ranges.
[[nodiscard]] constexpr double trimFromNormalized(float normalized) noexcept
{
    return static_cast<double>(normalized) * 2.0;
}

[[nodiscard]] constexpr double bipolarFromNormalized(float normalized) noexcept
{
    return static_cast<double>(normalized) * 2.0 - 1.0;
}

} // namespace

MixerControl::MixerControl(QObject* parent)
    : QObject(parent)
{
}

void MixerControl::setDecks(DjEngine* deckA, DjEngine* deckB, DjEngine* deckC, DjEngine* deckD)
{
    m_decks = { deckA, deckB, deckC, deckD };

    applyAllMixState();
    applyAllVolumes();
}

void MixerControl::attachParameterStore(ParameterStore* store)
{
    if (!store)
        return;
    connect(store, &ParameterStore::parameterChanged,
            this, &MixerControl::onParameterChanged);
}

DjEngine* MixerControl::deck(DeckId id) const
{
    return m_decks[domain::toIndex(id)];
}

void MixerControl::applyChannelVolume(DeckId id)
{
    if (DjEngine* const engine = deck(id))
        engine->applyVolume(static_cast<double>(m_mix[domain::toIndex(id)].fader));
}

void MixerControl::applyChannelMixState(DeckId id)
{
    DjEngine* const engine = deck(id);
    if (!engine)
        return;

    const ChannelMixState& state = m_mix[domain::toIndex(id)];
    engine->applyTrim(state.trim);
    engine->applyEqHigh(state.eqHigh);
    engine->applyEqMid(state.eqMid);
    engine->applyEqLow(state.eqLow);
    engine->applyFilter(state.filter);
    engine->applyPolarityInverted(state.polarityInverted);
}

void MixerControl::setTrim(const QString& channelId, double value)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    m_mix[domain::toIndex(*id)].trim = value;
    if (DjEngine* const engine = deck(*id))
        engine->applyTrim(value);
}

void MixerControl::setEqHigh(const QString& channelId, double value)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    m_mix[domain::toIndex(*id)].eqHigh = value;
    if (DjEngine* const engine = deck(*id))
        engine->applyEqHigh(value);
}

void MixerControl::setEqMid(const QString& channelId, double value)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    m_mix[domain::toIndex(*id)].eqMid = value;
    if (DjEngine* const engine = deck(*id))
        engine->applyEqMid(value);
}

void MixerControl::setEqLow(const QString& channelId, double value)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    m_mix[domain::toIndex(*id)].eqLow = value;
    if (DjEngine* const engine = deck(*id))
        engine->applyEqLow(value);
}

void MixerControl::setFilter(const QString& channelId, double value)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    m_mix[domain::toIndex(*id)].filter = value;
    if (DjEngine* const engine = deck(*id))
        engine->applyFilter(value);
}

void MixerControl::setPolarityInverted(const QString& channelId, bool inverted)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    m_mix[domain::toIndex(*id)].polarityInverted = inverted;
    if (DjEngine* const engine = deck(*id))
        engine->applyPolarityInverted(inverted);
}

void MixerControl::toggleCue(const QString& channelId)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    if (DjEngine* const engine = deck(*id))
        engine->setCueEnabled(!engine->cueEnabled());
}

void MixerControl::setChannelFader(const QString& channelId, double level)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    m_mix[domain::toIndex(*id)].fader = std::clamp(static_cast<float>(level), 0.0f, 1.0f);
    applyChannelVolume(*id);
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
    for (const DeckId id : domain::kAllDecks)
        applyChannelVolume(id);
}

void MixerControl::applyAllMixState()
{
    for (const DeckId id : domain::kAllDecks)
        applyChannelMixState(id);
}

double MixerControl::faderLevel(const QString& channelId) const
{
    const auto id = domain::deckFromChannelId(channelId);
    return id ? m_mix[domain::toIndex(*id)].fader : 1.0;
}

void MixerControl::onParameterChanged(const QString& id, float value)
{
    // Monitoring controls have one owner here. Routing them through both this
    // class and MidiControllerManager used to toggle channel CUE twice, making
    // a correct hardware press appear to do nothing.
    if (id == QLatin1String("master_cue")) {
        if (value >= 0.5f) {
            if (DjEngine* const engine = deck(DeckId::A))
                engine->setMasterCueEnabled(!engine->masterCueEnabled());
        }
        return;
    }
    if (id == QLatin1String("headphone_mix")) {
        if (DjEngine* const engine = deck(DeckId::A))
            engine->setHeadphoneMix(static_cast<double>(value));
        else
            AudioEngine::setHeadphoneMix(value);
        return;
    }
    if (id == QLatin1String("headphone_level")) {
        AudioEngine::setHeadphoneGain(std::clamp(value, 0.0f, 1.0f) * 2.0f);
        return;
    }
    if (id == QLatin1String("master_level")) {
        AudioEngine::setMasterVolume(std::clamp(value, 0.0f, 1.0f));
        return;
    }
    if (id == QLatin1String("crossfader")) {
        setCrossfaderPosition(value * 2.0f - 1.0f);
        return;
    }

    const int sep = id.indexOf(QLatin1Char('_'));
    if (sep <= 4)
        return;

    const QString channelId = id.left(sep);
    const auto deckId = domain::deckFromChannelId(channelId);
    if (!deckId)
        return;

    // The setters below are plain functions, not property writers, so feeding
    // store updates through them cannot bounce back out as a QML binding change.
    const QStringView suffix = QStringView(id).mid(sep + 1);
    if (suffix == QLatin1String("gain"))
        setTrim(channelId, trimFromNormalized(value));
    else if (suffix == QLatin1String("eqHigh"))
        setEqHigh(channelId, bipolarFromNormalized(value));
    else if (suffix == QLatin1String("eqMid"))
        setEqMid(channelId, bipolarFromNormalized(value));
    else if (suffix == QLatin1String("eqLow"))
        setEqLow(channelId, bipolarFromNormalized(value));
    else if (suffix == QLatin1String("filter"))
        setFilter(channelId, bipolarFromNormalized(value));
    else if (suffix == QLatin1String("vol"))
        setChannelFader(channelId, static_cast<double>(value));
    else if (suffix == QLatin1String("headphone_cue") && value >= 0.5f)
        toggleCue(channelId);
}
