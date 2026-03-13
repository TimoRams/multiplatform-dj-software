#pragma once

#include <QString>

struct TrackSegment {
    QString label;
    float startTime = 0.0f;
    float endTime = 0.0f;
    QString colorHex;
};
