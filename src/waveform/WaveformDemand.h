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

    [[nodiscard]] bool valid() const noexcept
    {
        return generation != 0 && std::isfinite(playheadSec)
            && visibleBeforeSec >= 0.0 && visibleAfterSec >= 0.0
            && guardBeforeSec >= visibleBeforeSec
            && guardAfterSec >= visibleAfterSec;
    }

    bool operator==(const WaveformDemand&) const noexcept = default;
};

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

    const double visibleBegin = demand.playheadSec - demand.visibleBeforeSec;
    const double visibleEnd = demand.playheadSec + demand.visibleAfterSec;
    const double guardBegin = demand.playheadSec - demand.guardBeforeSec;
    const double guardEnd = demand.playheadSec + demand.guardAfterSec;
    const double centre = (beginSec + endSec) * 0.5;
    const double distance = std::abs(centre - demand.playheadSec);
    const bool containsPlayhead = beginSec <= demand.playheadSec
        && demand.playheadSec < endSec;
    if (containsPlayhead)
        return {WaveformPriority::Visible, 0.0, 0};

    const bool afterPlayhead = beginSec >= demand.playheadSec;
    const bool beforePlayhead = endSec <= demand.playheadSec;
    const bool preferredDirection = demand.scratching
        || (!demand.reverse && afterPlayhead)
        || (demand.reverse && beforePlayhead);
    const std::uint8_t guardRank = preferredDirection ? 1 : 2;

    if (rangesIntersect(beginSec, endSec, visibleBegin, visibleEnd))
        return {WaveformPriority::Visible, distance, guardRank};

    if (demand.scratching
        && rangesIntersect(beginSec, endSec, guardBegin, guardEnd)) {
        return {WaveformPriority::ScratchOrReverseGuard, distance, 1};
    }

    const bool inForwardGuard = rangesIntersect(
        beginSec, endSec, visibleEnd, guardEnd);
    const bool inReverseGuard = rangesIntersect(
        beginSec, endSec, guardBegin, visibleBegin);
    if ((!demand.reverse && inForwardGuard)
        || (demand.reverse && inReverseGuard)) {
        return {WaveformPriority::PlaybackDirection, distance, 1};
    }
    if (inForwardGuard || inReverseGuard)
        return {WaveformPriority::RecentlyPassed, distance, 2};

    const double nearRadius = std::max(
        demand.guardBeforeSec + demand.guardAfterSec, 1.0);
    if (distance <= nearRadius * 2.0)
        return {WaveformPriority::BackgroundNear, distance, 3};
    return {WaveformPriority::BackgroundRest, distance, 4};
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
