#pragma once

#include <QString>
#include <QVector>

#include "TrackData.h"

class WaveformCache
{
public:
    struct Payload {
        int pointsPerSecond = 0;
        int totalExpected = 0;
        float globalMaxPeak = 0.001f;
        QVector<TrackData::WaveformBin> waveform;
        QVector<TrackData::RgbWaveformFrame> rgb;
        QVector<TrackData::PeakFrame> peakMip;  // high-res signed min/max peaks
    };

    static QString cachePathFor(const QString& filePath, int pointsPerSecond);
    static bool loadForFile(const QString& filePath, int pointsPerSecond, Payload* out);
    static bool saveForFile(const QString& filePath, const Payload& payload);
};
