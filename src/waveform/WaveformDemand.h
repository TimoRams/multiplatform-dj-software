#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace waveform {

enum class WaveformPriority : std::uint8_t {
    Visible = 0,
    PlaybackDirection = 1,
    ScratchOrReverseGuard = 2,
    RecentlyPassed = 3,
    BackgroundNear = 4,
    BackgroundRest = 5
};

struct WaveformDemand final {
    double playheadSec = 0.0;
    double visibleBeforeSec = 0.0;
    double visibleAfterSec = 0.0;
    double guardBeforeSec = 0.0;
    double guardAfterSec = 0.0;
    bool playing = false;
    bool reverse = false;
    bool scratching = false;
    std::uint8_t lodLevel = 0;
    std::uint64_t generation = 0;

    // Slip/seek previews have a second visible timeline. Keep it in the same
    // demand so progressive publication can service it without replacing the
    // audible transport's viewport. A negative playhead means no preview.
    double previewPlayheadSec = -1.0;
    double previewVisibleBeforeSec = 0.0;
    double previewVisibleAfterSec = 0.0;
    double previewGuardBeforeSec = 0.0;
    double previewGuardAfterSec = 0.0;
    bool previewReverse = false;
    bool previewScratching = false;

    [[nodiscard]] bool valid() const noexcept
    {
        return generation != 0 && std::isfinite(playheadSec)
            && visibleBeforeSec >= 0.0 && visibleAfterSec >= 0.0
            && guardBeforeSec >= visibleBeforeSec
            && guardAfterSec >= visibleAfterSec;
    }

    [[nodiscard]] bool hasPreviewViewport() const noexcept
    {
        return previewPlayheadSec >= 0.0 && std::isfinite(previewPlayheadSec)
            && previewVisibleBeforeSec >= 0.0 && previewVisibleAfterSec >= 0.0
            && previewGuardBeforeSec >= previewVisibleBeforeSec
            && previewGuardAfterSec >= previewVisibleAfterSec;
    }

    bool operator==(const WaveformDemand&) const noexcept = default;
};

inline void setPreviewViewport(WaveformDemand& destination,
                               const WaveformDemand& preview) noexcept
{
    destination.previewPlayheadSec = preview.playheadSec;
    destination.previewVisibleBeforeSec = preview.visibleBeforeSec;
    destination.previewVisibleAfterSec = preview.visibleAfterSec;
    destination.previewGuardBeforeSec = preview.guardBeforeSec;
    destination.previewGuardAfterSec = preview.guardAfterSec;
    destination.previewReverse = preview.reverse;
    destination.previewScratching = preview.scratching;
}

inline void clearPreviewViewport(WaveformDemand& demand) noexcept
{
    demand.previewPlayheadSec = -1.0;
    demand.previewVisibleBeforeSec = 0.0;
    demand.previewVisibleAfterSec = 0.0;
    demand.previewGuardBeforeSec = 0.0;
    demand.previewGuardAfterSec = 0.0;
    demand.previewReverse = false;
    demand.previewScratching = false;
}

struct WaveformPriorityScore final {
    WaveformPriority priority = WaveformPriority::BackgroundRest;
    double distanceSec = 0.0;
    // Strict publication order around the transport cursor.  This is kept
    // separate from the semantic priority so the complete visible viewport
    // can remain P0 while its chunks still expand deterministically from the
    // playhead in the current playback direction.
    std::uint8_t expansionRank = 5;
};

inline bool higherPriority(const WaveformPriorityScore& left,
                           const WaveformPriorityScore& right) noexcept
{
    if (left.expansionRank != right.expansionRank)
        return left.expansionRank < right.expansionRank;
    if (left.priority != right.priority)
        return left.priority < right.priority;
    return left.distanceSec < right.distanceSec;
}

inline bool rangesIntersect(double leftBegin, double leftEnd,
                            double rightBegin, double rightEnd) noexcept
{
    return leftEnd > rightBegin && leftBegin < rightEnd;
}

inline WaveformPriorityScore priorityForRange(
    const WaveformDemand& demand, double beginSec, double endSec) noexcept
{
    if (!demand.valid() || !std::isfinite(beginSec) || !std::isfinite(endSec)
        || endSec <= beginSec) {
        return {};
    }

    const auto scoreViewport = [beginSec, endSec](double playheadSec,
                                                   double visibleBeforeSec,
                                                   double visibleAfterSec,
                                                   double guardBeforeSec,
                                                   double guardAfterSec,
                                                   bool reverse,
                                                   bool scratching) noexcept {
        const double visibleBegin = playheadSec - visibleBeforeSec;
        const double visibleEnd = playheadSec + visibleAfterSec;
        const double guardBegin = playheadSec - guardBeforeSec;
        const double guardEnd = playheadSec + guardAfterSec;
        const double centre = (beginSec + endSec) * 0.5;
        const double distance = std::abs(centre - playheadSec);
        if (beginSec <= playheadSec && playheadSec < endSec)
            return WaveformPriorityScore{WaveformPriority::Visible, 0.0, 0};

        const bool afterPlayhead = beginSec >= playheadSec;
        const bool beforePlayhead = endSec <= playheadSec;
        const bool preferredDirection = scratching
            || (!reverse && afterPlayhead)
            || (reverse && beforePlayhead);
        const std::uint8_t guardRank = preferredDirection ? 1 : 2;
        if (rangesIntersect(beginSec, endSec, visibleBegin, visibleEnd))
            return WaveformPriorityScore{WaveformPriority::Visible, distance, guardRank};
        if (scratching && rangesIntersect(beginSec, endSec, guardBegin, guardEnd))
            return WaveformPriorityScore{WaveformPriority::ScratchOrReverseGuard, distance, 1};

        const bool inForwardGuard = rangesIntersect(beginSec, endSec, visibleEnd, guardEnd);
        const bool inReverseGuard = rangesIntersect(beginSec, endSec, guardBegin, visibleBegin);
        if ((!reverse && inForwardGuard) || (reverse && inReverseGuard))
            return WaveformPriorityScore{WaveformPriority::PlaybackDirection, distance, 1};
        if (inForwardGuard || inReverseGuard)
            return WaveformPriorityScore{WaveformPriority::RecentlyPassed, distance, 2};

        const double nearRadius = std::max(guardBeforeSec + guardAfterSec, 1.0);
        if (distance <= nearRadius * 2.0)
            return WaveformPriorityScore{WaveformPriority::BackgroundNear, distance, 3};
        return WaveformPriorityScore{WaveformPriority::BackgroundRest, distance, 4};
    };

    const auto primary = scoreViewport(
        demand.playheadSec, demand.visibleBeforeSec, demand.visibleAfterSec,
        demand.guardBeforeSec, demand.guardAfterSec, demand.reverse, demand.scratching);
    if (!demand.hasPreviewViewport())
        return primary;

    auto preview = scoreViewport(
        demand.previewPlayheadSec, demand.previewVisibleBeforeSec,
        demand.previewVisibleAfterSec, demand.previewGuardBeforeSec,
        demand.previewGuardAfterSec, demand.previewReverse, demand.previewScratching);
    // Audible transport comes first; the grey preview follows immediately,
    // ahead of normal background publication.
    preview.expansionRank = std::min<std::uint8_t>(
        static_cast<std::uint8_t>(preview.expansionRank + 1), 5);
    return higherPriority(preview, primary) ? preview : primary;
}

inline WaveformDemand makeViewportDemand(
    double playheadSec, double viewportWidth, double pixelsPerSecond,
    bool playing, bool reverse, bool scratching, std::uint8_t lodLevel,
    std::uint64_t generation) noexcept
{
    WaveformDemand demand;
    demand.playheadSec = std::max(0.0, playheadSec);
    const double visibleDuration = pixelsPerSecond > 0.0
        ? std::max(0.0, viewportWidth / pixelsPerSecond) : 0.0;
    demand.visibleBeforeSec = visibleDuration * 0.5;
    demand.visibleAfterSec = visibleDuration * 0.5;
    demand.playing = playing;
    demand.reverse = reverse;
    demand.scratching = scratching;
    demand.lodLevel = lodLevel;
    demand.generation = generation;

    if (scratching) {
        demand.guardBeforeSec = visibleDuration * 1.5;
        demand.guardAfterSec = visibleDuration * 1.5;
    } else if (reverse) {
        demand.guardBeforeSec = visibleDuration * 2.5;
        demand.guardAfterSec = visibleDuration;
    } else {
        demand.guardBeforeSec = visibleDuration;
        demand.guardAfterSec = visibleDuration * 2.5;
    }
    demand.guardBeforeSec = std::max(
        demand.guardBeforeSec, demand.visibleBeforeSec);
    demand.guardAfterSec = std::max(
        demand.guardAfterSec, demand.visibleAfterSec);
    return demand;
}

} // namespace waveform
