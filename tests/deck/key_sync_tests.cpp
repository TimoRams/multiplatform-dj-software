#include "deck/KeySync.h"

#include <cmath>
#include <iostream>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

bool parsesTo(const char* text, int pitchClass, bool minor)
{
    const auto key = brockdj::keysync::parseKey(text);
    return key && key->pitchClass == pitchClass && key->minor == minor;
}

} // namespace

int main()
{
    using namespace brockdj::keysync;
    bool ok = true;

    ok &= require(parsesTo("8A", 9, true), "8A is A minor");
    ok &= require(parsesTo("8B", 0, false), "8B is C major");
    ok &= require(parsesTo("08a", 9, true), "zero-padded lowercase Camelot parses");
    ok &= require(parsesTo("9A", 4, true), "9A is E minor");
    ok &= require(parsesTo("12B", 4, false), "12B is E major");
    ok &= require(parsesTo("1A", 8, true), "1A is G# minor");
    ok &= require(parsesTo("Am", 9, true), "Am parses");
    ok &= require(parsesTo("F#m", 6, true), "F#m parses");
    ok &= require(parsesTo("Bb", 10, false), "Bb parses");
    ok &= require(parsesTo("Eb major", 3, false), "long major notation parses");
    ok &= require(parsesTo("C minor", 0, true), "long minor notation parses");
    ok &= require(parsesTo("Cb", 11, false), "Cb wraps to B");
    for (const char* bad : { "", "13A", "0B", "8C", "H", "Xm", "C##", "8" })
        ok &= require(!parseKey(bad), "invalid keys are rejected");

    ok &= require(near(audiblePitchSemitones(2.0, 1.06, true), 2.0),
                  "keylock removes the varispeed component");
    ok &= require(near(audiblePitchSemitones(0.0, 2.0, false), 12.0),
                  "double speed without keylock is one octave");
    ok &= require(near(audiblePitchSemitones(1.0, 0.0, false), 1.0),
                  "invalid tempo ratios contribute nothing");

    ok &= require(near(wrapSemitones(7.0), -5.0) && near(wrapSemitones(-7.0), 5.0)
                      && near(wrapSemitones(6.0), -6.0) && near(wrapSemitones(0.0), 0.0),
                  "semitone distances wrap into [-6, 6)");

    // Same key needs no shift.
    auto offset = keySyncOffset("8A", "Am", 0.0);
    ok &= require(offset && near(*offset, 0.0), "identical keys stay put");

    // Camelot neighbours are already compatible: a fifth away needs no shift.
    offset = keySyncOffset("9A", "8A", 0.0);
    ok &= require(offset && near(*offset, 0.0), "Camelot +1 is already compatible");

    // Relative major/minor counts as a match.
    offset = keySyncOffset("8B", "8A", 0.0);
    ok &= require(offset && near(*offset, 0.0), "relative major matches its minor");

    // A minor vs. B minor: two semitones apart, no fifth is closer.
    offset = keySyncOffset("Bm", "Am", 0.0);
    ok &= require(offset && near(*offset, -2.0), "B minor drops two semitones to A minor");

    // Target pitched up one semitone (Key Shift or varispeed) moves the goal.
    offset = keySyncOffset("Am", "Am", 1.0);
    ok &= require(offset && near(*offset, 1.0), "the target's audible pitch is honoured");

    // Fractional varispeed of the target is matched exactly.
    const double targetPitch = audiblePitchSemitones(0.0, 1.03, false);
    offset = keySyncOffset("Am", "Am", targetPitch);
    ok &= require(offset && near(*offset, targetPitch), "fractional target pitch is matched");

    // Smallest movement wins: C major against F# major (tritone) — a fifth
    // relation reaches compatibility with one semitone.
    offset = keySyncOffset("C", "F#", 0.0);
    ok &= require(offset && std::abs(*offset) <= 1.0 + 1e-9,
                  "a fifth relation keeps the shift small");

    for (int own = 1; own <= 12; ++own) {
        for (int target = 1; target <= 12; ++target) {
            const std::string a = std::to_string(own) + "A";
            const std::string b = std::to_string(target) + "B";
            const auto o = keySyncOffset(a, b, 0.0);
            ok &= require(o && std::abs(*o) <= 3.5 + 1e-9,
                          "every key pair is reachable within a small shift");
        }
    }

    ok &= require(!keySyncOffset("", "8A", 0.0), "unknown own key is rejected");
    ok &= require(!keySyncOffset("8A", "??", 0.0), "unknown target key is rejected");
    ok &= require(!keySyncOffset("8A", "8A", std::nan("")), "non-finite pitch is rejected");

    if (!ok)
        return 1;
    std::cout << "key sync tests passed\n";
    return 0;
}
