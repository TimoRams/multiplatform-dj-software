#include "analysis/SongStructure.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace brockdj::structure;

namespace {

int failures = 0;

void expect(const char* name, bool ok)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++failures;
    }
}

std::string labels(const std::vector<Part>& parts)
{
    std::string s;
    for (const auto& p : parts) {
        if (!s.empty()) s += ' ';
        s += p.label;
    }
    return s;
}

LevelRun run(double a, double b, Level l) { return {a, b, l, 0.7f}; }

} // namespace

int main()
{
    // Classic EDM: intro, build, drop, breakdown, build, drop, outro.
    {
        const auto p = classify({run(0, 30, Level::Mid), run(30, 45, Level::Low),
                                 run(45, 75, Level::High), run(75, 105, Level::Low),
                                 run(105, 120, Level::Mid), run(120, 180, Level::High),
                                 run(180, 220, Level::Mid)});
        expect("edm structure", labels(p) == "Intro Build Drop Breakdown Build Drop Outro");
        expect("edm boundaries", p.size() == 7 && p[2].startSec == 45 && p[6].endSec == 220);
    }
    // Leading/trailing silence stays silence; single intro run is no build.
    {
        const auto p = classify({run(0, 2, Level::Silence), run(2, 30, Level::Low),
                                 run(30, 90, Level::High), run(90, 100, Level::Silence)});
        expect("silence edges", labels(p) == "Silence Intro Drop Silence");
    }
    // Mid between drops without a breakdown is a verse; adjacent equal parts merge.
    {
        const auto p = classify({run(0, 20, Level::Low), run(20, 40, Level::Low),
                                 run(40, 60, Level::High), run(60, 80, Level::Mid),
                                 run(80, 100, Level::High), run(100, 120, Level::Low)});
        expect("verse between drops", labels(p) == "Intro Build Drop Verse Drop Outro");
        expect("build bounds", p.size() > 1 && p[1].startSec == 20 && p[1].endSec == 40);
    }
    // Too long a pre-drop run is intro, not build.
    {
        const auto p = classify({run(0, 20, Level::Low), run(20, 100, Level::Mid),
                                 run(100, 160, Level::High)});
        expect("long pre-drop", labels(p) == "Intro Drop");
    }
    // No high energy at all: mid becomes the peak.
    {
        const auto p = classify({run(0, 30, Level::Low), run(30, 120, Level::Mid),
                                 run(120, 150, Level::Low)});
        expect("mid peak", labels(p) == "Intro Drop Outro");
    }
    // Flat / quiet material: verses only.
    {
        const auto p = classify({run(0, 60, Level::Low), run(60, 70, Level::Silence)});
        expect("flat", labels(p) == "Verse Silence");
    }
    // Unsorted input, empty runs dropped.
    {
        const auto p = classify({run(60, 90, Level::Low), run(0, 30, Level::Low),
                                 run(30, 60, Level::High), run(5, 5, Level::High)});
        expect("unsorted", labels(p) == "Intro Drop Outro");
    }
    {
        const auto p = classify({run(0, 10, Level::Low), run(10, 20, Level::Mid),
                                 run(20, 30, Level::Low), run(30, 40, Level::Mid),
                                 run(40, 80, Level::High)});
        expect("merge intro runs", labels(p) == "Intro Build Drop" && p[0].endSec == 30);
    }
    // Track opening and closing on full energy (DJ tool intro/outro beats).
    {
        const auto p = classify({run(0, 70, Level::High), run(70, 84, Level::Low),
                                 run(84, 167, Level::High), run(167, 222, Level::Low),
                                 run(222, 292, Level::High)});
        expect("peak edges", labels(p) == "Intro Breakdown Drop Breakdown Outro");
    }
    // Compressed master: everything loud, yet relative dips are breakdowns.
    {
        const auto lv = assignLevels({{0.82f, 0.80f}, {0.97f, 0.98f}, {0.80f, 0.70f},
                                      {1.0f, 1.0f}, {0.85f, 0.82f}, {0.02f, 0.0f, false}});
        expect("compressed levels", lv.size() == 6 && lv[1] == Level::High && lv[3] == Level::High
               && lv[2] == Level::Low && lv[5] == Level::Silence);
    }
    {
        const auto lv = assignLevels({{0.9f, 0.9f}, {0.91f, 0.9f}, {0.9f, 0.92f}});
        expect("flat levels", lv[0] == Level::Mid && lv[1] == Level::Mid && lv[2] == Level::Mid);
        const auto nobass = assignLevels({{0.9f, 0.9f}, {0.6f, 0.05f}});
        expect("no bass is low", nobass[1] == Level::Low);
    }
    expect("empty", classify({}).empty());

    Level l = Level::Silence;
    expect("legacy high", levelFromEnergyLabel("HighEnergy", l) && l == Level::High);
    expect("legacy mid", levelFromEnergyLabel("MediumEnergy", l) && l == Level::Mid);
    expect("legacy low", levelFromEnergyLabel("LowEnergy", l) && l == Level::Low);
    expect("legacy silence", levelFromEnergyLabel("SilenceOrOutro", l) && l == Level::Silence);
    expect("not legacy", !levelFromEnergyLabel("Drop", l));
    expect("is part", isSongPartLabel("Breakdown") && !isSongPartLabel("HighEnergy"));
    expect("colors", colorFor("Drop") != colorFor("Intro"));

    if (failures) {
        std::fprintf(stderr, "%d song structure test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("song structure tests passed");
    return EXIT_SUCCESS;
}
