#pragma once

#include <QString>
#include <QtGlobal>

#if defined(Q_OS_LINUX)

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

struct AlsaMidiInputEvent final
{
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    double timestampSeconds = 0.0;
};

// Direct ALSA sequencer input. DJ jog streams are too dense to route through
// an aseqdump child process and parse as text on the UI thread without adding
// avoidable latency and timestamp distortion.
class AlsaMidiInput final
{
public:
    using Callback = std::function<void(const AlsaMidiInputEvent&)>;

    AlsaMidiInput() = default;
    ~AlsaMidiInput();

    AlsaMidiInput(const AlsaMidiInput&) = delete;
    AlsaMidiInput& operator=(const AlsaMidiInput&) = delete;

    bool open(const QString& source, Callback callback,
              QString* errorMessage = nullptr);
    void close() noexcept;

    [[nodiscard]] bool isOpen() const noexcept
    {
        return m_seq != nullptr && m_destinationPort >= 0 && m_thread.joinable();
    }

private:
    void inputLoop() noexcept;

    snd_seq_t* m_seq = nullptr;
    int m_destinationPort = -1;
    int m_sourceClient = -1;
    int m_sourcePort = -1;
    bool m_connected = false;
    Callback m_callback;
    std::thread m_thread;
    std::atomic<bool> m_stopping {false};
};

#endif
