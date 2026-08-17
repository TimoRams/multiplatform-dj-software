#pragma once

#include <mutex>
#include <unordered_map>

namespace midi_internal {

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
