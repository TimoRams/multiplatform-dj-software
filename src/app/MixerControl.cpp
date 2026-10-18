#include "MixerControl.h"
#include "SettingsManager.h"

#include "audio/AudioEngine.h"
#include "controllers/midi/ParameterStore.h"
#include "deck/DjEngine.h"

#include <QDebug>
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
    m_settingsSaveTimer.setSingleShot(true);
    m_settingsSaveTimer.setInterval(250);
    connect(&m_settingsSaveTimer, &QTimer::timeout,
            this, &MixerControl::persistCrossfaderSettings);
}

void MixerControl::attachSettingsManager(SettingsManager* settings)
{
    m_settings = settings;
    if (!settings)
        return;
    syncCrossfaderState(settings->getCrossfaderPosition(),
                        settings->getCrossfaderAssignA(), settings->getCrossfaderAssignB(),
                        settings->getCrossfaderAssignC(), settings->getCrossfaderAssignD(),
                        settings->getCrossfaderSharpness(), settings->getCrossfaderCurveMode());
    m_settingsSaveTimer.stop();
}

QVariantMap MixerControl::crossfaderState() const
{
    return {{QStringLiteral("position"), m_cfPos},
            {QStringLiteral("sharpness"), m_cfSharpness},
            {QStringLiteral("curveMode"), m_cfCurveMode},
            {QStringLiteral("assignA"), m_assignA}, {QStringLiteral("assignB"), m_assignB},
            {QStringLiteral("assignC"), m_assignC}, {QStringLiteral("assignD"), m_assignD}};
}

void MixerControl::publishCrossfaderState()
{
    if (m_parameterStore)
        m_parameterStore->setParameter(QStringLiteral("crossfader"), (m_cfPos + 1.0f) * 0.5f);
    if (m_settings && !m_settingsSaveTimer.isActive())
        m_settingsSaveTimer.start();
    emit crossfaderStateChanged();
}

void MixerControl::persistCrossfaderSettings()
{
    m_settingsSaveTimer.stop();
    if (!m_settings)
        return;
    m_settings->setCrossfaderPosition(m_cfPos);
    m_settings->setCrossfaderSharpness(m_cfSharpness);
    m_settings->setCrossfaderCurveMode(m_cfCurveMode);
    m_settings->setCrossfaderAssignA(m_assignA);
    m_settings->setCrossfaderAssignB(m_assignB);
    m_settings->setCrossfaderAssignC(m_assignC);
    m_settings->setCrossfaderAssignD(m_assignD);
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
    m_parameterStore = store;
    store->setParameter(QStringLiteral("crossfader"), (m_cfPos + 1.0f) * 0.5f);
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

void MixerControl::setChannelParameter(const QString& channelId, double value,
                                       double ChannelMixState::* stateMember,
                                       void (DjEngine::* applyParameter)(double))
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id)
        return;
    m_mix[domain::toIndex(*id)].*stateMember = value;
    if (DjEngine* const engine = deck(*id))
        (engine->*applyParameter)(value);
}

void MixerControl::setTrim(const QString& channelId, double value)
{
    setChannelParameter(channelId, value, &ChannelMixState::trim, &DjEngine::applyTrim);
}

void MixerControl::setEqHigh(const QString& channelId, double value)
{
    setChannelParameter(channelId, value, &ChannelMixState::eqHigh, &DjEngine::applyEqHigh);
}

void MixerControl::setEqMid(const QString& channelId, double value)
{
    setChannelParameter(channelId, value, &ChannelMixState::eqMid, &DjEngine::applyEqMid);
}

void MixerControl::setEqLow(const QString& channelId, double value)
{
    setChannelParameter(channelId, value, &ChannelMixState::eqLow, &DjEngine::applyEqLow);
}

void MixerControl::setFilter(const QString& channelId, double value)
{
    setChannelParameter(channelId, value, &ChannelMixState::filter, &DjEngine::applyFilter);
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
    const float position = std::clamp(cfPos, -1.0f, 1.0f);
    if (m_cfPos == position)
        return;
    m_cfPos = position;
    AudioEngine::setCrossfaderPosition(m_cfPos);
    publishCrossfaderState();
}

void MixerControl::setCrossfaderAssignment(const QString& channelId, const QString& assignment)
{
    const auto id = domain::deckFromChannelId(channelId);
    if (!id || (assignment != QLatin1String("A") && assignment != QLatin1String("T")
                && assignment != QLatin1String("B"))) {
        qWarning() << "MixerControl: invalid crossfader assignment" << channelId << assignment;
        return;
    }
    const std::array<QString*, 4> assignments {&m_assignA, &m_assignB, &m_assignC, &m_assignD};
    auto& current = *assignments[domain::toIndex(*id)];
    if (current == assignment)
        return;
    current = assignment;
    AudioEngine::setCrossfaderAssignment(static_cast<int>(domain::toIndex(*id)),
                                         assignmentFromString(assignment));
    publishCrossfaderState();
}

void MixerControl::syncCrossfaderState(float cfPos,
                                       const QString& assignA,
                                       const QString& assignB,
                                       const QString& assignC,
                                       const QString& assignD,
                                       float cfSharpness,
                                       const QString& cfCurveMode)
{
    const auto previous = crossfaderState();
    m_cfPos = std::clamp(cfPos, -1.0f, 1.0f);
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
    if (previous != crossfaderState())
        publishCrossfaderState();
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
    if (suffix.startsWith(QLatin1String("crossfader_assign_"))) {
        // A switch reports all three notes; only its active (127) note selects a side.
        if (value < 0.5f)
            return;
        if (suffix == QLatin1String("crossfader_assign_a"))
            setCrossfaderAssignment(channelId, QStringLiteral("A"));
        else if (suffix == QLatin1String("crossfader_assign_thru"))
            setCrossfaderAssignment(channelId, QStringLiteral("T"));
        else if (suffix == QLatin1String("crossfader_assign_b"))
            setCrossfaderAssignment(channelId, QStringLiteral("B"));
        else
            qWarning() << "MixerControl: unknown crossfader assignment parameter" << id;
    } else if (suffix == QLatin1String("gain"))
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
