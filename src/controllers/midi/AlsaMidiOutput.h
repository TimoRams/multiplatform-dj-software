#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <cstdint>

#if defined(Q_OS_LINUX)
#include <alsa/asoundlib.h>

class AlsaMidiOutput
{
public:
    AlsaMidiOutput() = default;
    ~AlsaMidiOutput();

    bool open(const QString& target, QString* errorMessage = nullptr);
    void close();

    bool isOpen() const { return m_seq != nullptr && m_sourcePort >= 0; }
    bool sendShort(uint8_t status, uint8_t data1, uint8_t data2, QString* errorMessage = nullptr);
    bool sendMessage(const QByteArray& bytes, QString* errorMessage = nullptr);

private:
    snd_seq_t* m_seq = nullptr;
    int m_sourcePort = -1;
    int m_targetClient = -1;
    int m_targetPort = -1;
    bool m_connected = false;
};
#endif
