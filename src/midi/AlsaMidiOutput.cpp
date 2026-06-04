#include "AlsaMidiOutput.h"

#if defined(Q_OS_LINUX)
#include <QStringList>

namespace {
bool parseTargetPort(const QString& rawTarget, int& client, int& port)
{
    QString target = rawTarget.trimmed();
    if (target.startsWith(QStringLiteral("alsa-out:")))
        target.remove(0, QStringLiteral("alsa-out:").size());

    const QStringList parts = target.split(':');
    if (parts.size() != 2)
        return false;

    bool clientOk = false;
    bool portOk = false;
    const int parsedClient = parts.at(0).toInt(&clientOk);
    const int parsedPort = parts.at(1).toInt(&portOk);
    if (!clientOk || !portOk || parsedClient < 0 || parsedPort < 0)
        return false;

    client = parsedClient;
    port = parsedPort;
    return true;
}
}

AlsaMidiOutput::~AlsaMidiOutput()
{
    close();
}

bool AlsaMidiOutput::open(const QString& target, QString* errorMessage)
{
    close();

    if (!parseTargetPort(target, m_targetClient, m_targetPort)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("invalid ALSA target '%1'").arg(target);
        return false;
    }

    const int openResult = snd_seq_open(&m_seq, "default", SND_SEQ_OPEN_OUTPUT, 0);
    if (openResult < 0) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(snd_strerror(openResult));
        m_seq = nullptr;
        return false;
    }

    snd_seq_set_client_name(m_seq, "BrockDJ");
    m_sourcePort = snd_seq_create_simple_port(
        m_seq,
        "BrockDJ MIDI OUT",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    if (m_sourcePort < 0) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(snd_strerror(m_sourcePort));
        close();
        return false;
    }

    snd_seq_port_info_t* targetInfo = nullptr;
    snd_seq_port_info_alloca(&targetInfo);
    const int queryResult = snd_seq_get_any_port_info(m_seq, m_targetClient, m_targetPort, targetInfo);
    if (queryResult < 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("target %1:%2 not found: %3")
                .arg(m_targetClient)
                .arg(m_targetPort)
                .arg(QString::fromUtf8(snd_strerror(queryResult)));
        close();
        return false;
    }

    const unsigned int caps = snd_seq_port_info_get_capability(targetInfo);
    const bool writable = (caps & SND_SEQ_PORT_CAP_WRITE) != 0
        && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE) != 0;
    if (!writable) {
        if (errorMessage)
            *errorMessage = QStringLiteral("target %1:%2 is not a writable ALSA MIDI input port")
                .arg(m_targetClient)
                .arg(m_targetPort);
        close();
        return false;
    }

    const int connectResult = snd_seq_connect_to(m_seq, m_sourcePort, m_targetClient, m_targetPort);
    if (connectResult < 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("could not connect to target %1:%2: %3")
                .arg(m_targetClient)
                .arg(m_targetPort)
                .arg(QString::fromUtf8(snd_strerror(connectResult)));
        close();
        return false;
    }

    m_connected = true;
    return true;
}

void AlsaMidiOutput::close()
{
    if (m_seq && m_connected && m_sourcePort >= 0 && m_targetClient >= 0 && m_targetPort >= 0)
        snd_seq_disconnect_to(m_seq, m_sourcePort, m_targetClient, m_targetPort);

    if (m_seq) {
        snd_seq_close(m_seq);
        m_seq = nullptr;
    }

    m_sourcePort = -1;
    m_targetClient = -1;
    m_targetPort = -1;
    m_connected = false;
}

bool AlsaMidiOutput::sendShort(uint8_t status, uint8_t data1, uint8_t data2, QString* errorMessage)
{
    if (!isOpen()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("ALSA MIDI output is not open");
        return false;
    }

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, m_sourcePort);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    const int type = status & 0xf0;
    const int channel = status & 0x0f;
    switch (type) {
    case 0x80:
        snd_seq_ev_set_noteoff(&ev, channel, data1 & 0x7f, data2 & 0x7f);
        break;
    case 0x90:
        snd_seq_ev_set_noteon(&ev, channel, data1 & 0x7f, data2 & 0x7f);
        break;
    case 0xB0:
        snd_seq_ev_set_controller(&ev, channel, data1 & 0x7f, data2 & 0x7f);
        break;
    default:
        if (errorMessage)
            *errorMessage = QStringLiteral("unsupported MIDI status 0x%1")
                .arg(status, 2, 16, QLatin1Char('0'))
                .toUpper();
        return false;
    }

    const int result = snd_seq_event_output_direct(m_seq, &ev);
    if (result < 0) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(snd_strerror(result));
        return false;
    }

    return true;
}

bool AlsaMidiOutput::sendMessage(const QByteArray& bytes, QString* errorMessage)
{
    if (bytes.size() == 3) {
        return sendShort(static_cast<uint8_t>(bytes.at(0)),
                         static_cast<uint8_t>(bytes.at(1)),
                         static_cast<uint8_t>(bytes.at(2)),
                         errorMessage);
    }

    if (!isOpen()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("ALSA MIDI output is not open");
        return false;
    }

    if (bytes.isEmpty() || static_cast<uint8_t>(bytes.front()) != 0xF0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("ALSA MIDI output only supports 3-byte short messages or SysEx");
        return false;
    }

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, m_sourcePort);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_sysex(&ev,
                         static_cast<unsigned int>(bytes.size()),
                         const_cast<char*>(bytes.constData()));

    const int result = snd_seq_event_output_direct(m_seq, &ev);
    if (result < 0) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(snd_strerror(result));
        return false;
    }

    return true;
}
#endif
