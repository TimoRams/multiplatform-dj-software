#include "controllers/flx10/Flx10ControllerIdentity.h"
#include "domain/DeckId.h"
#include "analysis/AnalysisValidation.h"
#include "analysis/AnalysisTypes.h"
#include "audio/internal/HermiteKernel.h"
#include "deck/sync/SyncTypes.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

int runMixerDspSmokeTests();

namespace {

int g_failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void testDeckIndex()
{
    expect(domain::deckFromHardwareNumber(1) == domain::DeckId::A, "deck 1 is deck A");
    expect(domain::deckFromHardwareNumber(4) == domain::DeckId::D, "deck 4 is deck D");
    expect(!domain::deckFromHardwareNumber(0).has_value(), "deck 0 invalid");
    expect(!domain::deckFromHardwareNumber(5).has_value(), "deck 5 invalid");
    expect(domain::toHardwareNumber(domain::DeckId::C) == 3, "deck C is hardware deck 3");
    expect(domain::deckFromChannelId(QStringLiteral("deckB")) == domain::DeckId::B, "deckB parses");
    expect(!domain::deckFromChannelId(QStringLiteral("deckE")).has_value(), "deckE rejected");
    expect(!domain::deckFromChannelId(QStringLiteral("deck")).has_value(), "bare deck rejected");
    expect(domain::toChannelId(domain::DeckId::D) == QLatin1String("deckD"), "deck D channel id");
    expect(flx10::isNativeDeckNumber(2), "controller deck 2 native");
    expect(!flx10::isNativeDeckNumber(3), "controller deck 3 not native");
}

void testHermiteSampleAt()
{
    const std::vector<float> buffer {0.0f, 1.0f, 0.5f, -0.5f, 0.25f};
    const auto samples = std::span<const float>(buffer);

    expect(std::abs(engine::audio::sampleAt(samples, -1, engine::audio::SampleEdgeMode::Clamp) - 0.0f) < 1e-6f,
           "clamp negative index");
    expect(std::abs(engine::audio::sampleAt(samples, 10, engine::audio::SampleEdgeMode::Clamp) - 0.25f) < 1e-6f,
           "clamp past end");

    const float mirrored = engine::audio::sampleAt(samples, -1, engine::audio::SampleEdgeMode::Mirror);
    expect(std::abs(mirrored - 1.0f) < 1e-6f, "mirror negative index reflects to sample 1");

    const float hermite = engine::audio::readHermite(samples, 1.5, engine::audio::SampleEdgeMode::Clamp);
    expect(std::isfinite(hermite), "hermite clamp finite");
}

void testAnalysisValidation()
{
    std::vector<analysis::BeatMarker> beats;
    for (int i = 0; i < 16; ++i) {
        analysis::BeatMarker beat;
        beat.positionSec = static_cast<double>(i) * 0.5;
        beat.beatInBar = (i % 4) + 1;
        beat.isDownbeat = (beat.beatInBar == 1);
        beats.push_back(beat);
    }
    const auto result = analysis::validateBeatGrid(beats, 120.0, 60.0);
    expect(result.ok, "beatgrid validation ok for synthetic grid");
}

void testSyncMaintenancePolicy()
{
    using engine::shouldRunFollowerSyncMaintenance;

    expect(!shouldRunFollowerSyncMaintenance(false, false, false, false),
           "sync maintenance disabled when sync is off");
    expect(!shouldRunFollowerSyncMaintenance(true, true, false, false),
           "sync master does not run follower maintenance");
    expect(!shouldRunFollowerSyncMaintenance(true, false, true, false),
           "sync maintenance disabled while scrubbing");
    expect(!shouldRunFollowerSyncMaintenance(true, false, false, true),
           "sync maintenance disabled during release glide");
    expect(shouldRunFollowerSyncMaintenance(true, false, false, false),
           "sync follower runs maintenance when active");
}

} // namespace

int main()
{
    testDeckIndex();
    testHermiteSampleAt();
    testAnalysisValidation();
    testSyncMaintenancePolicy();
    g_failures += runMixerDspSmokeTests();

    if (g_failures == 0) {
        std::cout << "All smoke tests passed.\n";
        return EXIT_SUCCESS;
    }

    std::cerr << g_failures << " smoke test(s) failed.\n";
    return EXIT_FAILURE;
}
