#include "ParameterStore.h"

ParameterStore::ParameterStore(QObject* parent)
    : QObject(parent)
{
    // Passive MIDI controls do not necessarily publish their position when a
    // controller is connected. Keep the software mixer in a usable neutral
    // state until the first real hardware event arrives.
    for (const QString& deck : {QStringLiteral("deckA"), QStringLiteral("deckB"),
                                QStringLiteral("deckC"), QStringLiteral("deckD")}) {
        m_parameters.emplace(deck + QStringLiteral("_vol"), 1.0f);
        m_parameters.emplace(deck + QStringLiteral("_gain"), 0.5f);
        m_parameters.emplace(deck + QStringLiteral("_eqHigh"), 0.5f);
        m_parameters.emplace(deck + QStringLiteral("_eqMid"), 0.5f);
        m_parameters.emplace(deck + QStringLiteral("_eqLow"), 0.5f);
        m_parameters.emplace(deck + QStringLiteral("_filter"), 0.5f);
    }
    m_parameters.emplace(QStringLiteral("deckA_sound_color"), 0.5f);
    m_parameters.emplace(QStringLiteral("deckB_sound_color"), 0.5f);
    m_parameters.emplace(QStringLiteral("crossfader"), 0.5f);
    m_parameters.emplace(QStringLiteral("headphone_mix"), 0.5f);
}

void ParameterStore::setParameter(const QString& id, float value)
{
    const auto current = m_parameters.find(id);
    if (current != m_parameters.end() && current->second == value)
        return;

    m_parameters[id] = value;
    emit parameterChanged(id, value);
}

void ParameterStore::setMidiParameter(const QString& id, float value)
{
    m_parameters[id] = value;
    emit parameterChanged(id, value);
}

float ParameterStore::getParameter(const QString& id) const
{
    auto it = m_parameters.find(id);
    if (it != m_parameters.end()) {
        return it->second;
    }
    return 0.0f; // Default value if not found
}
