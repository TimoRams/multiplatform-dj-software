#include "ParameterStore.h"

ParameterStore::ParameterStore(QObject* parent)
    : QObject(parent)
{
}

void ParameterStore::setParameter(const QString& id, float value)
{
    if (m_parameters[id] != value) {
        m_parameters[id] = value;
        emit parameterChanged(id, value);
    }
}

float ParameterStore::getParameter(const QString& id) const
{
    auto it = m_parameters.find(id);
    if (it != m_parameters.end()) {
        return it->second;
    }
    return 0.0f; // Default value if not found
}
