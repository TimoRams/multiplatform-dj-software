#include "AppConfig.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

AppConfig::AppConfig(QObject* parent) : QObject(parent) {}

void AppConfig::init(const QString& configDirectoryPath)
{
    m_filePath = QDir(configDirectoryPath).filePath("app_config.xml");
    load();
    qDebug() << "[AppConfig] initialized, path=" << m_filePath
             << "firstRunCompleted=" << m_firstRunCompleted;
}

void AppConfig::completeFirstRun(bool persist)
{
    if (m_firstRunCompleted)
        return;

    m_firstRunCompleted = true;
    if (persist)
        save();
    emit firstRunCompletedChanged();
}

void AppConfig::load()
{
    QFile file(m_filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name().toString() == QStringLiteral("FirstRun"))
            m_firstRunCompleted = xml.attributes().value("completed").toString() == QStringLiteral("1");
    }
}

void AppConfig::save()
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[AppConfig] Could not write to" << m_filePath;
        return;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("RamsbrockDJ_AppConfig");
    xml.writeAttribute("version", "1");

    xml.writeStartElement("FirstRun");
    xml.writeAttribute("completed", m_firstRunCompleted ? "1" : "0");
    xml.writeEndElement();

    xml.writeEndElement();
    xml.writeEndDocument();

    qDebug() << "[AppConfig] Saved to" << m_filePath;
}
