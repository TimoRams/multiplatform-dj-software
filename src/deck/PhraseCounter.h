#pragma once

// Phrase counter math shared by the deck display. Pure, unit tested.

#include <cmath>
#include <vector>

namespace brockdj::phrase {

inline constexpr int kBeatsPerBar = 4;
inline constexpr int kBarsPerPhrase = 8;

struct PhrasePosition {
    int phraseNumber = 1; // 1-based; pre-roll gives 0, -1, …
    int barInPhrase = 1;  // 1..kBarsPerPhrase
};

// barNumber is 1-based from the anchor downbeat (bar 0, -1 … during pre-roll).
inline PhrasePosition fromBar(int barNumber) noexcept
{
    const int zeroBased = barNumber - 1;
    const int phrase = zeroBased >= 0 ? zeroBased / kBarsPerPhrase
                                      : -((-zeroBased + kBarsPerPhrase - 1) / kBarsPerPhrase);
    return {phrase + 1, zeroBased - phrase * kBarsPerPhrase + 1};
}

struct SegmentStart {
    double startSeconds = 0;
    double endSeconds = 0;
};

struct SegmentLocation {
    int current = -1; // index of the segment containing position, -1 if none
    int next = -1;    // first segment starting after position, -1 if none
};

// Segments need not be sorted.
inline SegmentLocation locateSegment(const std::vector<SegmentStart>& segments, double position) noexcept
{
    SegmentLocation location;
    for (int i = 0; i < static_cast<int>(segments.size()); ++i) {
        const auto& s = segments[static_cast<std::size_t>(i)];
        if (s.startSeconds <= position && position < s.endSeconds)
            location.current = i;
        if (s.startSeconds > position + 1e-6
            && (location.next < 0
                || s.startSeconds < segments[static_cast<std::size_t>(location.next)].startSeconds))
            location.next = i;
    }
    return location;
}

// Whole bars left until a boundary that is beatsToBoundary beats away; the bar
// in progress counts, so 0.5 beats left is still "1".
inline int barsUntil(double beatsToBoundary) noexcept
{
    if (!(beatsToBoundary > 1e-6))
        return 0;
    return static_cast<int>(std::ceil(beatsToBoundary / kBeatsPerBar - 1e-6));
}

} // namespace brockdj::phrase
