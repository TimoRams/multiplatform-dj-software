#include "audio/MixTracklist.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace brockdj::tracklist;

namespace {

int g_failures = 0;

void expectTrue(const char* what, bool condition)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++g_failures;
    }
}

void expectEq(const char* what, const std::string& actual, const std::string& expected)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s:\n--- got ---\n%s\n--- expected ---\n%s\n", what,
                     actual.c_str(), expected.c_str());
        ++g_failures;
    }
}

DeckObservation deck(const char* key, float gain, const char* artist = "Artist")
{
    return {key, std::string("Title ") + key, artist, gain};
}

// Feeds the same observation for [from, to) at 4 Hz, collecting new entries.
void feed(AudibleTrackTracker& t, std::vector<Entry>& out, double from, double to,
          const std::vector<DeckObservation>& decks)
{
    for (double now = from; now < to - 1e-9; now += 0.25)
        if (auto e = t.update(now, decks))
            out.push_back(*e);
}

void testHoldAndBackdate()
{
    AudibleTrackTracker t(8.0);
    std::vector<Entry> out;
    feed(t, out, 0, 7.5, {deck("a", 1.0f), deck("", 0)});
    expectTrue("no entry before hold", out.empty());
    feed(t, out, 7.5, 9, {deck("a", 1.0f), deck("", 0)});
    expectTrue("entry after hold", out.size() == 1);
    expectTrue("entry back-dated", !out.empty() && out[0].startSeconds == 0.0 && out[0].deck == 0);
}

void testShortCheckIgnored()
{
    AudibleTrackTracker t(8.0);
    std::vector<Entry> out;
    feed(t, out, 0, 20, {deck("a", 1.0f), deck("b", 0)});
    // B is briefly the loudest (e.g. a quick cut/scratch) for 3 s.
    feed(t, out, 20, 23, {deck("a", 0.2f), deck("b", 1.0f)});
    feed(t, out, 23, 40, {deck("a", 1.0f), deck("b", 0)});
    expectTrue("short cut ignored", out.size() == 1 && out[0].key == "a");
}

void testBlendTransition()
{
    AudibleTrackTracker t(8.0);
    std::vector<Entry> out;
    feed(t, out, 0, 60, {deck("a", 1.0f), deck("b", 0)});
    feed(t, out, 60, 70, {deck("a", 1.0f), deck("b", 0.5f)});  // B fading in
    feed(t, out, 70, 90, {deck("a", 0.3f), deck("b", 1.0f)});  // B dominant from 70 s
    expectTrue("two entries", out.size() == 2);
    expectTrue("second is b at 70", out.size() == 2 && out[1].key == "b" && out[1].startSeconds == 70.0
               && out[1].deck == 1);
}

void testEqualGainNoFlapping()
{
    AudibleTrackTracker t(2.0);
    std::vector<Entry> out;
    feed(t, out, 0, 10, {deck("a", 1.0f), deck("b", 0)});
    feed(t, out, 10, 60, {deck("a", 1.0f), deck("b", 1.0f)});
    expectTrue("equal gain keeps current", out.size() == 1);
}

void testSameTrackOtherDeck()
{
    AudibleTrackTracker t(2.0);
    std::vector<Entry> out;
    feed(t, out, 0, 10, {deck("a", 1.0f), deck("", 0)});
    feed(t, out, 10, 20, {deck("", 0), deck("a", 1.0f)});
    expectTrue("same track on other deck no duplicate", out.size() == 1);
    expectTrue("current deck follows", t.current() && t.current()->deck == 1);
}

void testSilenceResetsPending()
{
    AudibleTrackTracker t(8.0);
    std::vector<Entry> out;
    feed(t, out, 0, 5, {deck("a", 1.0f)});
    feed(t, out, 5, 6, {deck("a", 0.0f)});  // paused / fader down
    feed(t, out, 6, 13, {deck("a", 1.0f)});
    expectTrue("pending restarted after silence", out.empty());
    feed(t, out, 13, 15, {deck("a", 1.0f)});
    expectTrue("committed at resume time", out.size() == 1 && out[0].startSeconds == 6.0);
}

void testFormats()
{
    expectEq("clock", formatClock(3725.9), "01:02:05");
    expectEq("cue index", formatCueIndex(61.5), "01:01:37");
    expectEq("cue index long", formatCueIndex(6000.0), "100:00:00");

    std::vector<Entry> entries {
        {12.0, 0, "/m/a.mp3", "Intro", "DJ \"X\""},
        {245.25, 1, "/m/b.flac", "", ""},
    };
    expectEq("txt", formatText(entries),
             "00:00:12  DJ \"X\" - Intro\n"
             "00:04:05  /m/b.flac\n");
    expectEq("cue", formatCue(entries, "Set-1.wav", "Set-1"),
             "PERFORMER \"BrockDJ\"\n"
             "TITLE \"Set-1\"\n"
             "FILE \"Set-1.wav\" WAVE\n"
             "  TRACK 01 AUDIO\n"
             "    TITLE \"Intro\"\n"
             "    PERFORMER \"DJ 'X'\"\n"
             "    INDEX 01 00:00:00\n"
             "  TRACK 02 AUDIO\n"
             "    TITLE \"/m/b.flac\"\n"
             "    INDEX 01 04:05:18\n");
}

} // namespace

int main()
{
    testHoldAndBackdate();
    testShortCheckIgnored();
    testBlendTransition();
    testEqualGainNoFlapping();
    testSameTrackOtherDeck();
    testSilenceResetsPending();
    testFormats();
    if (g_failures == 0)
        std::puts("mix tracklist tests passed");
    return g_failures == 0 ? 0 : 1;
}
