#pragma once

#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace midi_internal {

class Midi14BitAccumulator
{
public:
    void pushMsb(int value) noexcept
    {
        m_msb = std::clamp(value, 0, 127);
    }

    void pushLsb(int value) noexcept
    {
        m_lsb = std::clamp(value, 0, 127);
    }

    [[nodiscard]] std::optional<int> takeValue() noexcept
    {
        if (!m_msb || !m_lsb)
            return std::nullopt;

        const int value = (*m_msb << 7) | *m_lsb;
        m_msb.reset();
        m_lsb.reset();
        return value;
    }

    void reset() noexcept
    {
        m_msb.reset();
        m_lsb.reset();
    }

private:
    std::optional<int> m_msb;
    std::optional<int> m_lsb;
};

// Some controllers expose a nominal 14-bit control but occasionally publish
// only its MSB when a port is opened. Hold that first coarse sample until a
// matching LSB arrives or actual movement proves the MSB stream is live.
class MidiUnpairedMsbGate
{
public:
    [[nodiscard]] bool shouldPublish(int value) noexcept
    {
        const int clamped = std::clamp(value, 0, 127);
        if (m_confirmed)
            return true;

        if (!m_baseline) {
            m_baseline = clamped;
            return false;
        }

        if (*m_baseline == clamped)
            return false;

        m_confirmed = true;
        return true;
    }

    void confirmPair() noexcept
    {
        m_confirmed = true;
    }

    void reset() noexcept
    {
        m_baseline.reset();
        m_confirmed = false;
    }

private:
    std::optional<int> m_baseline;
    bool m_confirmed = false;
};

// Drops incoming note events that are this application's own LED output coming
// back in.
//
// The controller publishes several sequencer ports under one client and the
// monitor subscribes to all of them, while the feedback output writes to the
// same client. Any path that loops a written note back onto one of those input
// ports turns a lamp update into a phantom button press. For a latching action
// that is fatal: BEAT FX ON lights up, the lamp write comes back as a press,
// and the unit switches straight off again — which is exactly the "it turns
// itself off, but only with the controller plugged in" symptom.
//
// The guard is deliberately narrow:
//  * only note-on style events (velocity > 0) are ever dropped. Releases must
//    always get through, or a momentary button would stay stuck down.
//  * the incoming velocity has to equal the value that was written.
//  * the copy has to arrive inside a very short window. A loopback is a
//    kernel/sequencer round trip, so it lands within a millisecond or two; a
//    human hand cannot.
// Lamp writes are also de-duplicated before they reach the wire, so a given
// note is normally only written when its state actually changes. That keeps
// these windows rare and short.
class MidiOutputEchoGuard final
{
public:
    static constexpr double kWindowSeconds = 0.020;

    // Called from whatever thread performs the feedback write.
    void noteSent(int msgId, int value, double nowSeconds)
    {
        if (value <= 0)
            return;
        const std::lock_guard lock(m_mutex);
        m_lastByMsgId[msgId] = Entry { value, nowSeconds };
    }

    // Called from the MIDI input threads. True means: this is our own lamp
    // write returning, discard it.
    [[nodiscard]] bool isEcho(int msgId, int value, double nowSeconds)
    {
        if (value <= 0)
            return false;
        const std::lock_guard lock(m_mutex);
        const auto it = m_lastByMsgId.find(msgId);
        if (it == m_lastByMsgId.end())
            return false;
        if (it->second.value != value
            || nowSeconds - it->second.timeSeconds >= kWindowSeconds
            || nowSeconds < it->second.timeSeconds) {
            return false;
        }
        // One write can only be echoed once. Consuming the entry means a second
        // genuine press inside the same window is still delivered.
        m_lastByMsgId.erase(it);
        return true;
    }

    void clear()
    {
        const std::lock_guard lock(m_mutex);
        m_lastByMsgId.clear();
    }

private:
    struct Entry final
    {
        int value = 0;
        double timeSeconds = 0.0;
    };
    mutable std::mutex m_mutex;
    std::unordered_map<int, Entry> m_lastByMsgId;
};

} // namespace midi_internal
