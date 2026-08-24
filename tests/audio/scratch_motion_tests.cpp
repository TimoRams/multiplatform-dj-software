// Reproducible scratch motion scenarios.
//
// The rig replays scripted hand movement through the same call the audio
// callback makes, so tracking quality is measured rather than guessed. Every
// scenario feeds a continuous velocity profile, emits controller events at a
// realistic cadence, and consumes fixed-size audio blocks in between — which is
// exactly the situation where a tracker that treats the last received target as
// stationary for the whole block starts to stutter.

#include "audio/cache/AudioPageCache.h"
#include "audio/internal/ScratchResampler.h"
#include "deck/scratch/ScratchController.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using engine::audio::ScratchResampler;

bool g_verbose = false;
// Position-trim bandwidth used for the velocity-led arm of the A/B. Overridable
// so the trade-off can be swept without a rebuild.
double g_correctionHz = 6.0;
int g_failures = 0;

bool require(bool value, const char* message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
    return value;
}

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr double kBlockSeconds = kBlockSize / kSampleRate;
// Typical jog event cadence of a USB controller.
constexpr double kEventIntervalSec = 0.002;
constexpr int kFixtureSamples = 48000 * 12;

bool writeSineFixture(const QString& path, double frequencyHz = 1000.0)
{
    juce::WavAudioFormat format;
    auto fs = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!fs->openedOk())
        return false;
    std::unique_ptr<juce::OutputStream> stream = std::move(fs);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
        .withSampleRate(kSampleRate).withNumChannels(2).withBitsPerSample(24));
    if (!writer)
        return false;
    juce::AudioBuffer<float> data(2, kFixtureSamples);
    for (int i = 0; i < kFixtureSamples; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const auto value = static_cast<float>(
            0.5 * std::sin(2.0 * M_PI * frequencyHz * t));
        data.setSample(0, i, value);
        data.setSample(1, i, value);
    }
    return writer->writeFromAudioSampleBuffer(data, 0, kFixtureSamples);
}

bool waitResident(AudioPageCache& cache, const AudioCacheHandle& handle)
{
    const auto last = handle.pageCount() - 1;
    cache.requestRange(handle, 0, last, AudioCachePriority::ScratchNearPlayhead);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (std::int64_t page = 0; page <= last; ++page)
            ready = static_cast<bool>(cache.tryGetPage(handle, page)) && ready;
        if (ready)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

double renderSteadyScratchRms(AudioPageCache& cache,
                              const AudioCacheHandle& handle,
                              double rate)
{
    ScratchResampler scratch;
    scratch.prepare(2, kBlockSize, kSampleRate);
    scratch.setTrackLengthSamples(static_cast<double>(handle.lengthInSamples()));
    scratch.setTrackCacheSource(&cache, handle);
    scratch.reset(2.0 * kSampleRate);
    scratch.prefetchAround(scratch.readPosition());

    juce::AudioBuffer<float> out(2, kBlockSize);
    // Clear the rate ramp and starvation fade before measuring the steady state.
    for (int block = 0; block < 6; ++block)
        scratch.processBlock(rate, {&out, 0, kBlockSize});

    double sumSquares = 0.0;
    for (int ch = 0; ch < out.getNumChannels(); ++ch)
        for (int i = 0; i < out.getNumSamples(); ++i) {
            const double sample = out.getSample(ch, i);
            sumSquares += sample * sample;
        }
    return std::sqrt(sumSquares
        / static_cast<double>(out.getNumChannels() * out.getNumSamples()));
}

// The first blocks of every run are a genuine standing start: the hand begins
// at rest and the tracker has not seen a single event yet, so one block of pure
// stall is unavoidable in any causal system. Steady-state quality is measured
// after that, and the startup catch-up is reported on its own.
constexpr int kSettleBlocks = 4;

struct MotionResult {
    double maxRateStepX = 0.0;     // largest per-sample rate jump, in playback rates
    double maxErrSamples = 0.0;    // worst |read head - true hand position|
    // The same error expressed as time at the scenario's peak hand speed: how
    // stale the audio under the finger is. Normalising by the peak rather than
    // by the instantaneous speed keeps the figure meaningful through a zero
    // crossing, where the hand is momentarily still and the quotient against an
    // instantaneous speed diverges regardless of how well the head is tracking.
    double maxLagMs = 0.0;
    double startupErrSamples = 0.0;  // worst error during the standing start
    double startupLagMs = 0.0;
    double meanRateX = 0.0;        // achieved mean rate over the run
    double travelRatio = 0.0;      // achieved travel / commanded travel
    bool travelMeaningful = false; // false when the movement nets out to zero
    std::uint64_t leadLimitedBlocks = 0;
    int blocks = 0;
    bool finite = true;
};

// velocityAt returns the hand speed in playback rates (1.0 = 1x forward).
// eventIntervalSec is the cadence the input actually arrives at: hardware jog
// rings report every couple of milliseconds, while an on-screen drag can only
// produce one event per UI frame.
MotionResult runMotion(AudioPageCache& cache,
                       const AudioCacheHandle& handle,
                       const char* name,
                       double startSample,
                       double durationSec,
                       const std::function<double(double)>& velocityAt,
                       double eventIntervalSec = kEventIntervalSec,
                       engine::audio::ScratchTrackerMode mode =
                           engine::audio::ScratchTrackerMode::PositionServo,
                       double correctionHz = 6.0)
{
    ScratchResampler scratch;
    scratch.setTrackerMode(mode);
    scratch.setVelocityLedCorrectionHz(correctionHz);
    scratch.prepare(2, kBlockSize, kSampleRate);
    scratch.setTrackLengthSamples(static_cast<double>(kFixtureSamples));
    scratch.setTrackCacheSource(&cache, handle);
    scratch.reset(startSample);
    scratch.prefetchAround(startSample);

    juce::AudioBuffer<float> out(2, kBlockSize);
    MotionResult result;

    double peakSpeed = 0.0;
    double pathLength = 0.0;   // total distance travelled, direction ignored
    {
        constexpr double kStepSec = 0.00025;
        for (double t = 0.0; t < durationSec; t += kStepSec) {
            const double speed = std::abs(velocityAt(t));
            peakSpeed = std::max(peakSpeed, speed);
            pathLength += speed * kSampleRate * kStepSec;
        }
    }
    peakSpeed = std::max(peakSpeed, 0.05);

    // The hand is integrated on a fine grid; the tracker only ever sees the
    // sampled event stream, which is what the controller bridge delivers.
    double trueHandSample = startSample;
    double lastEventSample = startSample;
    double lastEventRate = 0.0;
    double lastEventTime = 0.0;
    double eventClock = 0.0;
    double handClock = 0.0;
    double time = 0.0;

    // Hand position at an arbitrary instant, integrated from rest on a fine
    // grid. Used to answer what an event arriving at that instant would report.
    const auto handSampleAt = [&velocityAt, startSample](double until) {
        constexpr double kStepSec = 0.00025;
        double position = startSample;
        for (double t = 0.0; t + kStepSec <= until; t += kStepSec)
            position += velocityAt(t + kStepSec * 0.5) * kSampleRate * kStepSec;
        return position;
    };

    // The audio thread does not read the raw event velocity; it reads
    // ScratchController::commandedHandSpeed(), which holds the last command for
    // a moment and then decays it so a lifted hand does not run on forever. How
    // much of it survives to the next event is therefore a function of the input
    // cadence, which is exactly what differs between a jog ring and a drag.
    const engine::scratch::ScratchControllerConfig cfg;
    const double intervalMs = eventIntervalSec * 1000.0;
    const double holdMs = std::max(cfg.commandVelocityHoldMs, intervalMs * 1.25);
    const double tauMs = std::max({1.0, cfg.commandVelocityDecayTauMs,
                                   intervalMs * 0.5});
    const auto commandedAt = [holdMs, tauMs](double rate, double ageSec) {
        const double ageMs = ageSec * 1000.0;
        return ageMs <= holdMs ? rate
                               : rate * std::exp(-(ageMs - holdMs) / tauMs);
    };

    while (time < durationSec) {
        // Causal order: the block is rendered from what the controller had
        // already delivered by the time it started. Events that land while it
        // plays are only visible to the next block, exactly as in the callback.
        const double blockEnd = time + kBlockSeconds;

        const double before = scratch.readPosition();
        scratch.processScratchTracking(lastEventSample,
                                       commandedAt(lastEventRate, time - lastEventTime),
                                       8.0,
                                       {&out, 0, kBlockSize},
                                       std::max(0.0, time - lastEventTime));
        const double after = scratch.readPosition();

        // The hand moves continuously whether or not an event happens to be due,
        // so it is integrated on its own fine grid. Comparing the read head to
        // the last *event* instead would charge the tracker for staleness the
        // input never told it about — and at a UI cadence some blocks receive no
        // event at all.
        constexpr double kHandStepSec = 0.00025;
        while (handClock + kHandStepSec <= blockEnd) {
            trueHandSample += velocityAt(handClock + kHandStepSec * 0.5)
                * kSampleRate * kHandStepSec;
            handClock += kHandStepSec;
        }

        while (eventClock + eventIntervalSec <= blockEnd) {
            eventClock += eventIntervalSec;
            // The event reports the hand where it actually was at that instant.
            lastEventSample = handSampleAt(eventClock);
            lastEventRate = velocityAt(eventClock - eventIntervalSec * 0.5);
            lastEventTime = eventClock;
        }

        for (int ch = 0; ch < 2 && result.finite; ++ch)
            for (int i = 0; i < kBlockSize; ++i)
                if (!std::isfinite(out.getSample(ch, i))) {
                    result.finite = false;
                    break;
                }

        const auto stats = scratch.motionStats();
        const double errSamples = std::abs(after - trueHandSample);
        if (result.blocks < kSettleBlocks) {
            result.startupErrSamples = std::max(result.startupErrSamples, errSamples);
        } else {
            result.maxRateStepX = std::max(result.maxRateStepX, stats.maxRateStep);
            result.maxErrSamples = std::max(result.maxErrSamples, errSamples);
        }
        result.meanRateX += (after - before);
        ++result.blocks;
        time = blockEnd;
    }

    const double toMs = 1000.0 / (peakSpeed * kSampleRate);
    result.maxLagMs = result.maxErrSamples * toMs;
    result.startupLagMs = result.startupErrSamples * toMs;

    // Only meaningful when the movement actually goes somewhere. A scratch that
    // returns to where it started has a net travel near zero, so this ratio
    // divides two small numbers and says nothing about tracking quality; those
    // scenarios are judged on rate continuity and position error instead.
    const double commandedTravel = trueHandSample - startSample;
    const double achievedTravel = result.meanRateX;
    result.travelMeaningful = std::abs(commandedTravel) > 0.2 * pathLength;
    result.travelRatio = (result.travelMeaningful && std::abs(commandedTravel) > 1.0)
        ? achievedTravel / commandedTravel : 1.0;
    result.meanRateX /= std::max(1, result.blocks) * static_cast<double>(kBlockSize);

    // leadLimitedBlocks is a running counter inside the resampler, so the last
    // read already holds the total for this run.
    result.leadLimitedBlocks = scratch.motionStats().leadLimitedBlocks;

    if (g_verbose) {
        std::cout << "  " << name
                  << "  meanRate=" << result.meanRateX
                  << "x travelRatio=" << result.travelRatio
                  << " maxRateStep=" << result.maxRateStepX
                  << "x maxLag=" << result.maxLagMs
                  << "ms maxErr=" << result.maxErrSamples
                  << "smp startupLag=" << result.startupLagMs
                  << "ms leadLimited=" << result.leadLimitedBlocks
                  << "/" << result.blocks << '\n';
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    g_verbose = qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_VERBOSE");
    if (qEnvironmentVariableIsSet("BROCKDJ_SCRATCH_CORRECTION_HZ")) {
        bool okHz = false;
        const double hz = qEnvironmentVariable("BROCKDJ_SCRATCH_CORRECTION_HZ").toDouble(&okHz);
        if (okHz && hz > 0.0) g_correctionHz = hz;
    }

    QTemporaryDir dir;
    const QString fixture = dir.filePath("scratch-motion.wav");
    if (!require(writeSineFixture(fixture), "sine fixture written"))
        return 1;

    AudioPageCache cache;
    auto handle = cache.openTrack({fixture});
    if (!require(handle.isValid(), "fixture handle valid"))
        return 1;
    if (!require(waitResident(cache, handle), "whole fixture resident in cache"))
        return 1;

    // A 10 kHz source played at 4x would fold back into the audible band with
    // polynomial interpolation. The scratch reader must reject it before it is
    // decimated; an output low-pass is too late once that alias exists.
    const QString aliasFixture = dir.filePath("scratch-alias.wav");
    if (!require(writeSineFixture(aliasFixture, 10000.0),
                 "alias fixture written"))
        return 1;
    auto aliasHandle = cache.openTrack({aliasFixture});
    if (!require(aliasHandle.isValid(), "alias fixture handle valid"))
        return 1;
    if (!require(waitResident(cache, aliasHandle),
                 "alias fixture resident in cache"))
        return 1;

    const double oneXRms = renderSteadyScratchRms(cache, aliasHandle, 1.0);
    const double fourXRms = renderSteadyScratchRms(cache, aliasHandle, 4.0);
    if (g_verbose)
        std::cout << "scratch anti-alias: 1x RMS=" << oneXRms
                  << " 4x RMS=" << fourXRms << '\n';
    require(oneXRms > 0.2, "scratch resampler preserves in-band detail at 1x");
    require(fourXRms < oneXRms * 0.15,
            "scratch resampler rejects fold-back alias before 4x decimation");

    if (g_verbose)
        std::cout << "scratch motion scenarios:\n";

    const double start = 4.0 * kSampleRate;

    struct Scenario {
        const char* name;
        double durationSec;
        std::function<double(double)> velocity;
        double expectedRate;      // mean rate the run should achieve
        double rateTolerance;
        // A movement whose direction changes faster than a UI frame rate can
        // convey. Nothing downstream can recover detail the input never carried,
        // so these are only checked for stability at that cadence.
        bool needsHighInputRate = false;
        // Worst tolerated position error, as time at the peak hand speed.
        double lagBudgetMs = 12.0;
        // The movement actually turns around. A backspin decays to rest without
        // ever changing sign, so it is not a reversal case however slow it ends.
        bool reverses = false;
    };

    const std::vector<Scenario> scenarios {
        {"slow forward drag 0.1x", 0.5, [](double) { return 0.1; }, 0.1, 0.03},
        {"slow reverse drag 0.1x", 0.5, [](double) { return -0.1; }, -0.1, 0.03},
        {"nominal forward 1x", 0.5, [](double) { return 1.0; }, 1.0, 0.12},
        {"nominal reverse 1x", 0.5, [](double) { return -1.0; }, -1.0, 0.12},
        {"fast forward throw 4x", 0.4, [](double) { return 4.0; }, 4.0, 0.45},
        {"fast reverse throw 4x", 0.4, [](double) { return -4.0; }, -4.0, 0.45},
        // A baby scratch: back and forth at roughly 5 Hz.
        {"baby scratch", 0.8,
         [](double t) { return 1.6 * std::sin(2.0 * M_PI * 5.0 * t); }, 0.0, 0.25,
         false, 12.0, true},
        // Alternating ±1x every 60 ms — the direction-change stress case. The
        // reversal is shaped over a few milliseconds because a hand cannot flip
        // a platter instantaneously; a true step would only measure how fast the
        // tracker follows an unphysical infinite acceleration.
        {"alternating +-1x", 0.8,
         [](double t) { return std::tanh(3.0 * std::sin(2.0 * M_PI * t / 0.12)); },
         // Reversals cost more than steady motion: the 52 Hz tracker settles in
         // roughly 19 ms while a hand turns a platter in about 6 ms, so it
         // rounds the corner rather than following it. This is the engine's
         // known residual, held here so it cannot quietly grow.
         0.0, 0.25, true, 15.0, true},
        // Backspin: a hard reverse throw decaying back to rest.
        {"backspin", 0.6,
         [](double t) { return -6.0 * std::exp(-t / 0.25); }, 0.0, 3.0},
    };

    for (const auto& scenario : scenarios) {
        const auto result = runMotion(cache, handle, scenario.name, start,
                                      scenario.durationSec, scenario.velocity);

        require(result.finite,
                (std::string(scenario.name) + ": output finite").c_str());

        // Every scenario must actually move the read head the distance the hand
        // asked for. A tracker that stalls mid-block shows up here first.
        if (result.travelMeaningful) {
            require(std::abs(result.travelRatio - 1.0) < 0.12,
                    (std::string(scenario.name) + ": travels the commanded distance").c_str());
        }

        // The read head must stay glued to the hand. Anything beyond a few
        // milliseconds is audible as the scratch feeling detached.
        require(result.maxLagMs < scenario.lagBudgetMs,
                (std::string(scenario.name) + ": read head stays close to the hand").c_str());

        // Per-sample rate must stay continuous. A frozen-then-released read head
        // produces a large step here, which is what clicks and sounds digital.
        require(result.maxRateStepX < 0.05,
                (std::string(scenario.name) + ": rate stays continuous between samples").c_str());
    }

    // The same movements dragged on screen. A pointer or touch drag can only
    // produce one event per UI frame, so the audio thread sees the hand roughly
    // eight times less often than it does from a jog ring. Tracking quality must
    // not depend on which of the two is driving.
    constexpr double kUiFrameSec = 1.0 / 60.0;
    for (const auto& scenario : scenarios) {
        const std::string name = std::string(scenario.name) + " [screen drag]";
        const auto result = runMotion(cache, handle, name.c_str(), start,
                                      scenario.durationSec, scenario.velocity,
                                      kUiFrameSec);

        require(result.finite, (name + ": output finite").c_str());

        if (scenario.needsHighInputRate) {
            // The input itself cannot describe this movement at one event per
            // frame, so only stability is required. Reproducing it faithfully
            // needs a jog ring, and no amount of downstream work changes that.
            require(result.maxRateStepX < 1.0,
                    (name + ": stays stable even when the input undersamples").c_str());
            continue;
        }

        if (result.travelMeaningful) {
            require(std::abs(result.travelRatio - 1.0) < 0.12,
                    (name + ": travels the commanded distance").c_str());
        }
        // Budget: half a frame of event age plus one audio block, which is the
        // most the head can know about a hand reporting this rarely.
        require(result.maxLagMs < 18.0,
                (name + ": read head stays close to the hand").c_str());
        require(result.maxRateStepX < 0.05,
                (name + ": rate stays continuous between samples").c_str());
    }

    // Steady drags must never need the lead limiter: it exists for stale
    // commands, not for normal movement.
    for (const double rate : {0.25, 1.0, 2.0, 4.0}) {
        const auto forward = runMotion(cache, handle, "steady", start, 0.4,
                                       [rate](double) { return rate; });
        require(forward.leadLimitedBlocks == 0,
                "steady forward drag never hits the lead limiter");
        const auto reverse = runMotion(cache, handle, "steady reverse", 8.0 * kSampleRate,
                                       0.4, [rate](double) { return -rate; });
        require(reverse.leadLimitedBlocks == 0,
                "steady reverse drag never hits the lead limiter");
    }

    // A sign change must never arrive as a rate step. These traces cross zero at
    // a range of speeds, including one that crosses inside a single block, so
    // the crossing cannot hide in a block boundary.
    for (const double peak : {0.5, 2.0, 6.0}) {
        for (const double turnSec : {0.20, 0.05, 0.008}) {
            const std::string name = "sign change +-" + std::to_string(peak).substr(0, 3)
                + "x over " + std::to_string(turnSec).substr(0, 5) + "s";
            const auto result = runMotion(cache, handle, name.c_str(), start, 0.4,
                                          [peak, turnSec](double t) {
                                              // Linear sweep down through zero,
                                              // held either side of the turn.
                                              const double turnStart = 0.2 - turnSec * 0.5;
                                              if (t <= turnStart) return peak;
                                              if (t >= turnStart + turnSec) return -peak;
                                              return peak * (1.0 - 2.0 * (t - turnStart) / turnSec);
                                          });
            require(result.finite, (name + ": output finite").c_str());
            // The turn is a real acceleration, so the rate must change — the
            // question is whether it changes by more than the hand asked for.
            // The budget is what the hand itself sweeps through in one sample,
            // with headroom for the tracker's own settling. A snap shows as a
            // step far above this; the 6x-in-8ms case is already past what a
            // hand can do to a platter and is included as a stress bound.
            const double handStepPerSample = 2.0 * peak / turnSec / kSampleRate;
            require(result.maxRateStepX < 0.05 + 4.0 * handStepPerSample,
                    (name + ": rate crosses zero without a step").c_str());
            require(result.leadLimitedBlocks == 0,
                    (name + ": zero crossing never snaps the read head").c_str());
        }
    }

    // A/B: position servo vs velocity-led tracker on identical motion traces,
    // identical cache and identical event cadence. The point is to decide with
    // numbers whether pulling the head at the hand position every sample buys
    // anything over letting the hand speed drive it with a slow position trim.
    {
        using engine::audio::ScratchTrackerMode;
        if (g_verbose)
            std::cout << "tracker A/B (servo -> velocity-led):\n";

        for (const double cadence : {kEventIntervalSec, kUiFrameSec}) {
            const bool drag = cadence > kEventIntervalSec;
            for (const auto& scenario : scenarios) {
                const auto servo = runMotion(cache, handle, "", start,
                                             scenario.durationSec, scenario.velocity,
                                             cadence, ScratchTrackerMode::PositionServo);
                const auto led = runMotion(cache, handle, "", start,
                                           scenario.durationSec, scenario.velocity,
                                           cadence, ScratchTrackerMode::VelocityLed,
                                           g_correctionHz);

                require(led.finite,
                        (std::string(scenario.name) + ": velocity-led output finite").c_str());

                // The measured verdict, pinned so it cannot invert unnoticed.
                // Feeding the position error straight into the rate makes every
                // jump in that error a jump in the rate, which is precisely a
                // direction change. The servo feeds it into acceleration, so the
                // rate is an integral and stays continuous by construction. If
                // this ever fails the velocity-led arm has genuinely improved
                // and the choice of default is worth revisiting — it is not a
                // bug to be silenced.
                if (!drag && scenario.reverses) {
                    require(servo.maxRateStepX <= led.maxRateStepX,
                            (std::string(scenario.name)
                             + ": servo keeps rate more continuous through reversals"
                               " than the velocity-led tracker").c_str());
                }

                if (g_verbose) {
                    std::cout << "  " << scenario.name << (drag ? " [screen drag]" : "")
                              << "  rateStep " << servo.maxRateStepX << " -> "
                              << led.maxRateStepX
                              << "  lag " << servo.maxLagMs << "ms -> "
                              << led.maxLagMs << "ms"
                              << "  travel " << servo.travelRatio << " -> "
                              << led.travelRatio
                              << "  leadLimited " << servo.leadLimitedBlocks << " -> "
                              << led.leadLimitedBlocks << '\n';
                }
            }
        }
    }

    cache.releaseTrack(aliasHandle);
    cache.releaseTrack(handle);
    return g_failures == 0 ? 0 : 1;
}
