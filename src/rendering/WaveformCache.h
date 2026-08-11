#pragma once

#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "TrackData.h"
#include "waveform/WaveformLineBuilder.h"

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
        std::shared_ptr<const waveform::PreparedWaveformLines> preparedLines;
    };

    struct RenderInfo {
        int pointsPerSecond = 0;
        int totalLines = 0;
        QVector<TrackData::RgbWaveformFrame> overview;
    };

    using RenderChunkCallback = std::function<void(
        int firstLine, int totalLines,
        std::shared_ptr<const std::vector<WaveformLine>> lines)>;

    static QString cachePathFor(const QString& filePath, int pointsPerSecond);
    static QString renderCachePathFor(const QString& filePath, int pointsPerSecond);
    static bool loadForFile(const QString& filePath, int pointsPerSecond, Payload* out);
    static bool inspectRenderCache(const QString& filePath, int pointsPerSecond,
                                   RenderInfo* out);
    static bool streamRenderCache(
        const QString& filePath, int pointsPerSecond,
        const std::function<bool()>& shouldCancel,
        const std::function<double()>& seekHintSeconds,
        const RenderChunkCallback& publishChunk);
    static bool saveForFile(const QString& filePath, const Payload& payload);
};
