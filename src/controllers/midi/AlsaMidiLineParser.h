#pragma once

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace midi_internal {

struct AlsaControlChange
{
    bool valid = false;
    int channel = 0;
    int control = 0;
    int value = 0;
};

inline bool shouldAcceptAlsaChannelFaderSource(const QString& sourcePort,
                                               const QString& primaryPort,
                                               int control)
{
    const bool isChannelFaderByte = control == 0x13 || control == 0x33;
    return !isChannelFaderByte || primaryPort.isEmpty() || sourcePort == primaryPort;
}

// This controller publishes several sequencer ports under one ALSA client, and
// the monitor subscribes to all of them because the sections are split across
// them. A button that is mirrored on two of those ports therefore arrives twice,
// which turns one press into two: an action that latches — BEAT FX ON above all
// — switches on and straight back off, and the LED shows a single blink.
//
// The channel faders were given a primary-port rule for the same reason
// (shouldAcceptAlsaChannelFaderSource). This is the button-shaped version of it,
// and it is deliberately narrower: only an identical event from a *different*
// port inside a very short window is dropped. A repeat from the same port, a
// different velocity, or anything later than the window is always a real event
// — no human presses the same button twice inside 30 ms.
class AlsaCrossPortButtonFilter final
{
public:
    static constexpr double kWindowSeconds = 0.030;

    // Each mirrored port is read by its own thread, so the two copies of a press
    // race each other by definition. The lock is held across a hash lookup and
    // nothing else, and none of this runs on the audio thread.
    [[nodiscard]] bool accept(const QString& sourcePort, int msgId, int rawValue,
                              double nowSeconds)
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_lastByMsgId.find(msgId);
        if (it != m_lastByMsgId.end()
            && it->second.port != sourcePort
            && it->second.rawValue == rawValue
            && nowSeconds - it->second.timeSeconds < kWindowSeconds) {
            // Anchored to the first arrival on purpose: a third mirrored port
            // inside the same window is still a duplicate, but a stream of them
            // can never hold the window open indefinitely.
            return false;
        }

        m_lastByMsgId[msgId] = Entry { sourcePort, rawValue, nowSeconds };
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
        QString port;
        int rawValue = 0;
        double timeSeconds = 0.0;
    };
    mutable std::mutex m_mutex;
    std::unordered_map<int, Entry> m_lastByMsgId;
};

template <typename Mapping>
int resolveMappedAlsaMessageId(int channelAwareMsgId,
                               int legacyMsgId,
                               const Mapping& mapping)
{
    // parseAlsaControlChange normalizes every recognized format to a zero-based
    // MIDI channel. Applying another one-based fallback here aliases an
    // unmapped channel 3 event onto channel 2 (for example Deck C's zero fader
    // snapshot onto deckB_vol).
    if (mapping.count(channelAwareMsgId))
        return channelAwareMsgId;
    if (mapping.count(legacyMsgId))
        return legacyMsgId;
    return channelAwareMsgId;
}

inline AlsaControlChange parseAlsaControlChange(const QString& line)
{
    static const QRegularExpression standardRx(
        R"(Control\s+change\s+(\d+)\s*,\s*(?:controller|control)\s+(\d+)\s*,\s*value\s+(-?\d+))",
        QRegularExpression::CaseInsensitiveOption);
    const auto standard = standardRx.match(line);
    if (standard.hasMatch()) {
        return {
            true,
            standard.captured(1).toInt(),
            standard.captured(2).toInt(),
            standard.captured(3).toInt()
        };
    }

    static const QRegularExpression parenthesizedRx(
        R"((?:Chan|Channel)\s+(\d+).*?\((\d+)\s+(\d+)\s+(\d+)\))",
        QRegularExpression::CaseInsensitiveOption);
    const auto parenthesized = parenthesizedRx.match(line);
    if (parenthesized.hasMatch()) {
        const int status = parenthesized.captured(2).toInt();
        if ((status & 0xf0) == 0xb0) {
            return {
                true,
                status & 0x0f,
                parenthesized.captured(3).toInt(),
                parenthesized.captured(4).toInt()
            };
        }
        return {};
    }

    static const QRegularExpression verboseRx(
        R"((?:Chan|Channel)\s+(\d+).*?(?:controller|control)\s+(\d+).*?value\s+(-?\d+))",
        QRegularExpression::CaseInsensitiveOption);
    const auto verbose = verboseRx.match(line);
    if (verbose.hasMatch()) {
        return {
            true,
            std::max(0, verbose.captured(1).toInt() - 1),
            verbose.captured(2).toInt(),
            verbose.captured(3).toInt()
        };
    }

    static const QRegularExpression numberRx(R"((\d+))");
    QList<int> numbers;
    auto matches = numberRx.globalMatch(line);
    while (matches.hasNext())
        numbers.push_back(matches.next().captured(1).toInt());
    if (numbers.size() < 3)
        return {};

    return {
        true,
        numbers.at(numbers.size() - 3),
        numbers.at(numbers.size() - 2),
        numbers.at(numbers.size() - 1)
    };
}

} // namespace midi_internal
