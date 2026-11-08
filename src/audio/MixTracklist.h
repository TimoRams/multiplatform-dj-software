#pragma once

// Pure logic for "which track is the audience hearing" plus .cue/.txt writers
// for set recordings. No Qt/JUCE so it can be unit tested in isolation.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace brockdj::tracklist {

struct DeckObservation {
    std::string key;    // stable track identity (file path); empty = no track
    std::string title;
    std::string artist;
    float airGain = 0;  // channel fader × crossfader, 0 when the deck is not playing
};

struct Entry {
    double startSeconds = 0; // time base of the caller (monotonic or set-relative)
    int deck = -1;
    std::string key, title, artist;
};

// A deck becomes "now playing" once it has been the loudest routed deck for
// holdSeconds. Short cue checks, scratches and quick cuts are ignored; the
// entry start is back-dated to when the deck first became dominant.
class AudibleTrackTracker {
public:
    static constexpr float kMinimumGain = 0.05f;

    explicit AudibleTrackTracker(double holdSeconds = 8.0) : m_hold(holdSeconds) {}

    void reset()
    {
        m_current.reset();
        m_pendingDeck = -1;
        m_pendingKey.clear();
    }

    [[nodiscard]] const std::optional<Entry>& current() const noexcept { return m_current; }

    // Returns a new entry when the audible track changes.
    std::optional<Entry> update(double now, const std::vector<DeckObservation>& decks)
    {
        int dominant = -1;
        float best = kMinimumGain;
        for (int i = 0; i < static_cast<int>(decks.size()); ++i) {
            const auto& d = decks[static_cast<std::size_t>(i)];
            if (d.key.empty() || !(d.airGain >= kMinimumGain))
                continue;
            // Keep the current deck on (near-)equal gain to avoid flapping mid-blend.
            const bool isCurrent = m_current && m_current->deck == i && m_current->key == d.key;
            const float gain = isCurrent ? d.airGain * 1.05f : d.airGain;
            if (gain > best) {
                best = gain;
                dominant = i;
            }
        }

        if (dominant < 0) {
            m_pendingDeck = -1;
            m_pendingKey.clear();
            return std::nullopt;
        }

        const auto& d = decks[static_cast<std::size_t>(dominant)];
        if (m_current && m_current->key == d.key) {
            // Same track (also when re-loaded on the other deck): no new entry.
            m_current->deck = dominant;
            m_pendingDeck = -1;
            m_pendingKey.clear();
            return std::nullopt;
        }

        if (m_pendingDeck != dominant || m_pendingKey != d.key) {
            m_pendingDeck = dominant;
            m_pendingKey = d.key;
            m_pendingSince = now;
        }
        if (now - m_pendingSince + 1e-9 < m_hold)
            return std::nullopt;

        m_current = Entry {m_pendingSince, dominant, d.key, d.title, d.artist};
        m_pendingDeck = -1;
        m_pendingKey.clear();
        return m_current;
    }

private:
    double m_hold;
    std::optional<Entry> m_current;
    int m_pendingDeck = -1;
    std::string m_pendingKey;
    double m_pendingSince = 0;
};

inline std::string displayName(const Entry& e)
{
    if (e.artist.empty())
        return e.title.empty() ? e.key : e.title;
    return e.artist + " - " + (e.title.empty() ? e.key : e.title);
}

inline std::string formatClock(double seconds)
{
    const long total = static_cast<long>(std::floor(std::max(0.0, seconds) + 1e-6));
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%02ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
    return buffer;
}

// CUE INDEX uses mm:ss:ff with 75 frames per second; minutes may exceed 99.
inline std::string formatCueIndex(double seconds)
{
    const long frames = static_cast<long>(std::floor(std::max(0.0, seconds) * 75.0 + 1e-6));
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%02ld:%02ld:%02ld", frames / 4500, (frames / 75) % 60, frames % 75);
    return buffer;
}

inline std::string cueQuote(std::string text)
{
    for (char& c : text) {
        if (c == '"') c = '\'';
        else if (c == '\n' || c == '\r') c = ' ';
    }
    return "\"" + text + "\"";
}

inline std::string formatText(const std::vector<Entry>& entries)
{
    std::string out;
    for (const auto& e : entries)
        out += formatClock(e.startSeconds) + "  " + displayName(e) + "\n";
    return out;
}

inline std::string formatCue(const std::vector<Entry>& entries, const std::string& audioFileName,
                             const std::string& setTitle)
{
    std::string out = "PERFORMER " + cueQuote("BrockDJ") + "\n"
        + "TITLE " + cueQuote(setTitle) + "\n"
        + "FILE " + cueQuote(audioFileName) + " WAVE\n";
    int number = 1;
    for (const auto& e : entries) {
        char track[16];
        std::snprintf(track, sizeof track, "%02d", number++);
        out += std::string("  TRACK ") + track + " AUDIO\n";
        out += "    TITLE " + cueQuote(e.title.empty() ? e.key : e.title) + "\n";
        if (!e.artist.empty())
            out += "    PERFORMER " + cueQuote(e.artist) + "\n";
        // The first index must be 00:00:00 for a valid single-file cue sheet.
        out += "    INDEX 01 " + formatCueIndex(number == 2 ? 0.0 : e.startSeconds) + "\n";
    }
    return out;
}

} // namespace brockdj::tracklist
