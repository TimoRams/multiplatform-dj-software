#pragma once

#include "analysis/SongStructure.h"
#include "domain/DomainTypes.h"

#include <vector>

namespace analysis {

// Maps energy-level segments (fresh or legacy stored ones) to song parts
// (Intro/Verse/Build/Drop/Breakdown/Outro). Input that is not purely made of
// energy labels (already song parts, user data) is returned unchanged.
[[nodiscard]] inline std::vector<TrackSegment> toSongParts(const std::vector<TrackSegment>& segments)
{
    namespace st = brockdj::structure;
    std::vector<st::LevelRun> runs;
    runs.reserve(segments.size());
    for (const auto& segment : segments) {
        st::Level level = st::Level::Mid;
        if (!st::levelFromEnergyLabel(segment.label.toStdString(), level))
            return segments;
        runs.push_back({segment.startTime, segment.endTime, level, segment.confidence});
    }

    std::vector<TrackSegment> parts;
    for (const auto& part : st::classify(std::move(runs))) {
        const auto color = st::colorFor(part.label);
        parts.push_back({QString::fromStdString(part.label),
                         static_cast<float>(part.startSec),
                         static_cast<float>(part.endSec),
                         QString::fromLatin1(color.data(), static_cast<qsizetype>(color.size())),
                         part.confidence});
    }
    return parts;
}

} // namespace analysis
