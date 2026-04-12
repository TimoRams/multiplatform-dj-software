#pragma once

#include <QObject>
#include <QString>
#include <unordered_map>

class ParameterStore : public QObject
{
    Q_OBJECT

public:
    explicit ParameterStore(QObject* parent = nullptr);

    Q_INVOKABLE void setParameter(const QString& id, float value);
    Q_INVOKABLE float getParameter(const QString& id) const;

signals:
    void parameterChanged(const QString& id, float value);

private:
    std::unordered_map<QString, float> m_parameters;
};
