#include "AlsaMidiInput.h"

#if defined(Q_OS_LINUX)

#include <QStringList>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <utility>
#include <vector>

namespace {

bool parseSourcePort(QString source, int& client, int& port)
{
    source = source.trimmed();
    if (source.startsWith(QStringLiteral("alsa:")))
        source.remove(0, QStringLiteral("alsa:").size());

    const QStringList parts = source.split(':');
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

double monotonicSeconds() noexcept
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool decodeEvent(const snd_seq_event_t& event, AlsaMidiInputEvent& decoded) noexcept
{
    switch (event.type) {
    case SND_SEQ_EVENT_CONTROLLER:
        decoded.status = static_cast<std::uint8_t>(
            0xB0 | std::clamp(static_cast<int>(event.data.control.channel), 0, 15));
        decoded.data1 = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(event.data.control.param), 0, 127));
        decoded.data2 = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(event.data.control.value), 0, 127));
        return true;
    case SND_SEQ_EVENT_NOTEON:
    case SND_SEQ_EVENT_NOTEOFF:
        decoded.status = static_cast<std::uint8_t>(
            (event.type == SND_SEQ_EVENT_NOTEON ? 0x90 : 0x80)
            | std::clamp(static_cast<int>(event.data.note.channel), 0, 15));
        decoded.data1 = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(event.data.note.note), 0, 127));
        decoded.data2 = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(event.data.note.velocity), 0, 127));
        return true;
    case SND_SEQ_EVENT_PITCHBEND: {
        const int value = std::clamp(event.data.control.value + 8192, 0, 16383);
        decoded.status = static_cast<std::uint8_t>(
            0xE0 | std::clamp(static_cast<int>(event.data.control.channel), 0, 15));
        decoded.data1 = static_cast<std::uint8_t>(value & 0x7F);
        decoded.data2 = static_cast<std::uint8_t>((value >> 7) & 0x7F);
        return true;
    }
    default:
        return false;
    }
}

} // namespace

AlsaMidiInput::~AlsaMidiInput()
{
    close();
}

bool AlsaMidiInput::open(const QString& source, Callback callback,
                         QString* errorMessage)
{
    close();
    if (!parseSourcePort(source, m_sourceClient, m_sourcePort)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("invalid ALSA source '%1'").arg(source);
        return false;
    }

    const int openResult = snd_seq_open(
        &m_seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
    if (openResult < 0) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(snd_strerror(openResult));
        m_seq = nullptr;
        return false;
    }

    snd_seq_set_client_name(m_seq, "BrockDJ MIDI Input");
    m_destinationPort = snd_seq_create_simple_port(
        m_seq,
        "BrockDJ MIDI IN",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (m_destinationPort < 0) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(snd_strerror(m_destinationPort));
        close();
        return false;
    }

    const int connectResult = snd_seq_connect_from(
        m_seq, m_destinationPort, m_sourceClient, m_sourcePort);
    if (connectResult < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("could not connect from source %1:%2: %3")
                .arg(m_sourceClient)
                .arg(m_sourcePort)
                .arg(QString::fromUtf8(snd_strerror(connectResult)));
        }
        close();
        return false;
    }

    m_connected = true;
    m_callback = std::move(callback);
    m_stopping.store(false, std::memory_order_release);
    m_thread = std::thread([this] { inputLoop(); });
    return true;
}

void AlsaMidiInput::close() noexcept
{
    m_stopping.store(true, std::memory_order_release);
    if (m_thread.joinable())
        m_thread.join();

    if (m_seq && m_connected && m_destinationPort >= 0
        && m_sourceClient >= 0 && m_sourcePort >= 0) {
        snd_seq_disconnect_from(
            m_seq, m_destinationPort, m_sourceClient, m_sourcePort);
    }
    if (m_seq)
        snd_seq_close(m_seq);

    m_seq = nullptr;
    m_destinationPort = -1;
    m_sourceClient = -1;
    m_sourcePort = -1;
    m_connected = false;
    m_callback = {};
}

void AlsaMidiInput::inputLoop() noexcept
{
    const int descriptorCount = snd_seq_poll_descriptors_count(
        m_seq, POLLIN);
    if (descriptorCount <= 0)
        return;

    std::vector<pollfd> descriptors(static_cast<std::size_t>(descriptorCount));
    snd_seq_poll_descriptors(
        m_seq, descriptors.data(), descriptorCount, POLLIN);

    while (!m_stopping.load(std::memory_order_acquire)) {
        const int ready = ::poll(descriptors.data(), descriptors.size(), 20);
        if (ready <= 0)
            continue;

        for (;;) {
            snd_seq_event_t* event = nullptr;
            const int result = snd_seq_event_input(m_seq, &event);
            if (result == -EAGAIN || result == -EWOULDBLOCK)
                break;
            if (result < 0 || event == nullptr)
                break;

            AlsaMidiInputEvent decoded;
            const bool recognized = decodeEvent(*event, decoded);
            snd_seq_free_event(event);
            if (!recognized)
                continue;
            decoded.timestampSeconds = monotonicSeconds();
            if (m_callback)
                m_callback(decoded);
        }
    }
}

#endif
