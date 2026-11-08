#pragma once

// KEY SYNC (CDJ-3000 / XDJ-AZ style): shift this deck's pitch so that its
// audible key becomes harmonically compatible with another deck.
//
// Pure, Qt-free logic so it can be unit-tested without an audio graph.
// Keys are accepted in Camelot ("8A", "08a") or musical notation ("Am",
// "F#m", "Bb", "C minor", "Eb major").

#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace brockdj::keysync {

struct MusicalKey {
    int pitchClass = 0; // 0 = C … 11 = B (tonic)
    bool minor = false;
};

namespace detail {

inline std::string normalize(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            out.push_back(c);
    }
    return out;
}

inline bool endsWithNoCase(const std::string& s, std::string_view suffix)
{
    if (s.size() < suffix.size())
        return false;
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto a = std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i]));
        const auto b = std::tolower(static_cast<unsigned char>(suffix[i]));
        if (a != b)
            return false;
    }
    return true;
}

inline int wrap12(int value)
{
    return ((value % 12) + 12) % 12;
}

} // namespace detail

inline std::optional<MusicalKey> parseKey(std::string_view text)
{
    std::string s = detail::normalize(text);
    if (s.empty())
        return std::nullopt;

    // Camelot: 1..12 followed by A (minor) or B (major).
    if (std::isdigit(static_cast<unsigned char>(s.front()))) {
        std::size_t i = 0;
        int number = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && i < 3) {
            number = number * 10 + (s[i] - '0');
            ++i;
        }
        if (i + 1 != s.size() || number < 1 || number > 12)
            return std::nullopt;
        const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
        if (letter != 'A' && letter != 'B')
            return std::nullopt;
        const bool minor = letter == 'A';
        // 8A = A minor (9), 8B = C major (0); each Camelot step is a fifth.
        const int reference = minor ? 9 : 0;
        return MusicalKey { detail::wrap12(reference + 7 * (number - 8)), minor };
    }

    bool minor = false;
    if (detail::endsWithNoCase(s, "minor")) {
        minor = true;
        s.resize(s.size() - 5);
    } else if (detail::endsWithNoCase(s, "major")) {
        s.resize(s.size() - 5);
    } else if (detail::endsWithNoCase(s, "min")) {
        minor = true;
        s.resize(s.size() - 3);
    } else if (detail::endsWithNoCase(s, "maj")) {
        s.resize(s.size() - 3);
    } else if (!s.empty() && s.back() == 'm') {
        minor = true;
        s.pop_back();
    }
    if (s.empty() || s.size() > 2)
        return std::nullopt;

    static constexpr std::array<int, 7> kNatural { 9, 11, 0, 2, 4, 5, 7 }; // A..G
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    if (letter < 'A' || letter > 'G')
        return std::nullopt;
    int pc = kNatural[static_cast<std::size_t>(letter - 'A')];
    if (s.size() == 2) {
        if (s[1] == '#')
            ++pc;
        else if (s[1] == 'b')
            --pc;
        else
            return std::nullopt;
    }
    return MusicalKey { detail::wrap12(pc), minor };
}

// Pitch movement (semitones) a deck currently applies to its track: the manual
// Key Shift offset plus, without keylock, the varispeed shift of its tempo.
inline double audiblePitchSemitones(double keyOffsetSemitones, double tempoRatio, bool keylock)
{
    double pitch = std::isfinite(keyOffsetSemitones) ? keyOffsetSemitones : 0.0;
    if (!keylock && std::isfinite(tempoRatio) && tempoRatio > 0.0)
        pitch += 12.0 * std::log2(tempoRatio);
    return pitch;
}

// Wraps a semitone distance into [-6, +6).
inline double wrapSemitones(double value)
{
    double wrapped = std::fmod(value + 6.0, 12.0);
    if (wrapped < 0.0)
        wrapped += 12.0;
    return wrapped - 6.0;
}

// Returns the Key Shift offset (semitones, |x| <= 6) this deck needs, with
// keylock enabled, so that its audible key matches the target deck's audible
// key (or its relative major/minor), or sits a perfect fifth from it — the
// Camelot ±1 neighbours. The smallest movement wins; an exact match is
// preferred over a fifth when they are equally far.
inline std::optional<double> keySyncOffset(std::string_view ownKey,
                                           std::string_view targetKey,
                                           double targetPitchSemitones)
{
    const auto own = parseKey(ownKey);
    const auto target = parseKey(targetKey);
    if (!own || !target || !std::isfinite(targetPitchSemitones))
        return std::nullopt;

    // Express the target's tonic in this deck's mode (relative major/minor).
    double targetTonic = static_cast<double>(target->pitchClass) + targetPitchSemitones;
    if (target->minor && !own->minor)
        targetTonic += 3.0;
    else if (!target->minor && own->minor)
        targetTonic -= 3.0;

    const double exact = wrapSemitones(targetTonic - static_cast<double>(own->pitchClass));
    double best = exact;
    for (const double fifth : { 7.0, -7.0 }) {
        const double candidate = wrapSemitones(exact + fifth);
        if (std::abs(candidate) + 1e-9 < std::abs(best))
            best = candidate;
    }
    if (std::abs(best) < 1e-6)
        best = 0.0;
    return best;
}

} // namespace brockdj::keysync
