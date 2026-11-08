#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace brockdj::stems {

// Geometry of the overlapping inference windows in *source* samples.
// Window i covers [start(i), start(i) + window). Its "owned" range is
// [start(i), start(i+1)) (the last window owns up to the track end). The first
// `overlap` samples of an owned range (i > 0) are shared with window i−1's
// tail and become final only once both neighbours are separated.
struct StemChunkPlan {
    std::int64_t length = 0;
    std::int64_t window = 0;
    std::int64_t overlap = 0;

    [[nodiscard]] std::int64_t stride() const noexcept { return window - overlap; }

    [[nodiscard]] int count() const noexcept
    {
        if (length <= 0 || window <= 0 || stride() <= 0)
            return 0;
        if (length <= window)
            return 1;
        const auto remaining = length - window;
        const auto windows = 1 + remaining / stride() + (remaining % stride() != 0 ? 1 : 0);
        return windows > std::numeric_limits<int>::max() ? 0 : static_cast<int>(windows);
    }
    [[nodiscard]] std::int64_t start(int i) const noexcept { return static_cast<std::int64_t>(i) * stride(); }
    [[nodiscard]] std::int64_t ownedEnd(int i) const noexcept
    {
        return i + 1 >= count() ? length : std::min(length, start(i + 1));
    }
    // Shared head region of window i (empty for i == 0).
    [[nodiscard]] std::int64_t sharedEnd(int i) const noexcept
    {
        return i == 0 ? start(i) : std::min(ownedEnd(i), start(i) + overlap);
    }
    [[nodiscard]] int chunkForSample(std::int64_t sample) const noexcept
    {
        const int n = count();
        if (n <= 0)
            return -1;
        if (sample <= 0)
            return 0;
        return static_cast<int>(std::min<std::int64_t>(sample / stride(), n - 1));
    }

    // Crossfade weight of the incoming (later) window at offset k of an
    // overlap region; the earlier window gets 1 − weight.
    [[nodiscard]] float incomingWeight(std::int64_t k) const noexcept
    {
        return overlap <= 0 ? 1.0f
                            : static_cast<float>((static_cast<double>(k) + 0.5) / static_cast<double>(overlap));
    }
};

[[nodiscard]] inline StemChunkPlan makeStemChunkPlan(std::int64_t lengthSamples, double sampleRate,
                                                     std::int64_t modelWindow = 343980,
                                                     double modelRate = 44100.0) noexcept
{
    StemChunkPlan plan;
    plan.length = std::max<std::int64_t>(0, lengthSamples);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(modelRate) || modelRate <= 0.0 || modelWindow <= 0)
        return plan;
    const double windowSamples = static_cast<double>(modelWindow) * (sampleRate / modelRate);
    if (!std::isfinite(windowSamples)
        || windowSamples >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return plan;
    // Slightly shorter than the model window so the resampled read never
    // overruns the model input.
    plan.window = std::max<std::int64_t>(8, static_cast<std::int64_t>(windowSamples) - 4);
    plan.overlap = plan.window / 8;
    return plan;
}

// Picks the next window to separate: first unfinished window at or after the
// playhead (so audio ahead of the DJ is ready first), otherwise the closest
// unfinished window behind it.
[[nodiscard]] inline std::optional<int> nextStemChunk(const std::vector<bool>& done, int playheadChunk) noexcept
{
    const int n = static_cast<int>(done.size());
    if (n == 0)
        return std::nullopt;
    playheadChunk = std::clamp(playheadChunk, 0, n - 1);
    for (int i = playheadChunk; i < n; ++i)
        if (!done[static_cast<std::size_t>(i)])
            return i;
    for (int i = playheadChunk - 1; i >= 0; --i)
        if (!done[static_cast<std::size_t>(i)])
            return i;
    return std::nullopt;
}

// A playhead in an overlap needs both neighbouring windows before those
// samples can be published. Finish the preceding window before processing
// unrelated audio farther ahead.
[[nodiscard]] inline std::optional<int> nextStemChunk(const StemChunkPlan& plan,
                                                       const std::vector<bool>& done,
                                                       std::int64_t playheadSample) noexcept
{
    const int chunk = plan.chunkForSample(playheadSample);
    if (chunk > 0 && chunk < static_cast<int>(done.size())
        && playheadSample >= plan.start(chunk) && playheadSample < plan.sharedEnd(chunk)) {
        if (!done[static_cast<std::size_t>(chunk)])
            return chunk;
        if (!done[static_cast<std::size_t>(chunk - 1)])
            return chunk - 1;
    }
    return nextStemChunk(done, chunk);
}

} // namespace brockdj::stems
