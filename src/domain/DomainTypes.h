#pragma once

#include <QString>

namespace TransportLimits {

// Transport preroll length in seconds. Independent of beatgrid placement.
constexpr double kPreRollSeconds = 32.0;

} // namespace TransportLimits

struct TrackSegment {
    QString label;
    float startTime = 0.0f;
    float endTime = 0.0f;
    QString colorHex;
    float confidence = 0.0f;
};
