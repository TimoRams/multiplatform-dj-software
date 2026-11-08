#include "deck/PhraseCounter.h"

#include <cstdio>

using namespace brockdj::phrase;

namespace {

int g_failures = 0;

void expect(const char* what, bool condition)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++g_failures;
    }
}

bool phraseIs(int bar, int phrase, int barInPhrase)
{
    const auto p = fromBar(bar);
    return p.phraseNumber == phrase && p.barInPhrase == barInPhrase;
}

} // namespace

int main()
{
    expect("bar 1", phraseIs(1, 1, 1));
    expect("bar 8", phraseIs(8, 1, 8));
    expect("bar 9", phraseIs(9, 2, 1));
    expect("bar 17", phraseIs(17, 3, 1));
    expect("pre-roll bar 0", phraseIs(0, 0, 8));
    expect("pre-roll bar -7", phraseIs(-7, 0, 1));
    expect("pre-roll bar -8", phraseIs(-8, -1, 8));

    const std::vector<SegmentStart> segments {{0, 30}, {30, 90}, {90, 150}};
    auto l = locateSegment(segments, 10);
    expect("in first", l.current == 0 && l.next == 1);
    l = locateSegment(segments, 30);
    expect("on boundary", l.current == 1 && l.next == 2);
    l = locateSegment(segments, 120);
    expect("last has no next", l.current == 2 && l.next == -1);
    l = locateSegment({{5, 10}}, 1);
    expect("before first", l.current == -1 && l.next == 0);
    l = locateSegment({{90, 150}, {0, 30}, {30, 90}}, 10);
    expect("unsorted", l.current == 1 && l.next == 2);

    expect("bars 0", barsUntil(0) == 0);
    expect("bars half beat", barsUntil(0.5) == 1);
    expect("bars exactly 4 beats", barsUntil(4.0) == 1);
    expect("bars 4.1 beats", barsUntil(4.1) == 2);
    expect("bars 16 beats", barsUntil(16.0) == 4);

    if (g_failures == 0)
        std::puts("phrase counter tests passed");
    return g_failures == 0 ? 0 : 1;
}
