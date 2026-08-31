#pragma once

#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "TrackData.h"
#include "waveform/WaveformLineBuilder.h"
#include "waveform/WaveformTypes.h"
#include "waveform/WaveformDemand.h"

class WaveformCache
{
public:
    // Version of the rendered-line cache this build writes and accepts. Owned
    // here because this class is the only thing that reads or writes those
    // files; a bump invalidates them without touching the analysis results.
    static constexpr int kRenderCacheVersion = 3;

    struct Payload {
        int pointsPerSecond = 0;
        int spectralPointsPerSecond = TrackData::SPECTRAL_POINTS_PER_SECOND;
        int totalExpected = 0;
        float globalMaxPeak = 0.001f;
        QVector<TrackData::WaveformBin> waveform;
        QVector<TrackData::SpectralWaveformPoint> spectral;
        QVector<TrackData::RgbWaveformFrame> overview;
        QVector<TrackData::PeakFrame> peakMip;  // high-res signed min/max peaks
        std::shared_ptr<const waveform::PreparedWaveformLines> preparedLines;
    };

    struct RenderInfo {
        int pointsPerSecond = 0;
        int totalLines = 0;
        int cacheVersion = 0;
        int lodLevelCount = 0;
        QVector<TrackData::RgbWaveformFrame> overview;
    };

    using LodTile = WaveformLodBlock;

    using RenderChunkCallback = std::function<void(
        int totalLines, WaveformLineBatch chunks)>;

    static QString cachePathFor(const QString& filePath, int pointsPerSecond);
    static QString renderCachePathFor(const QString& filePath, int pointsPerSecond);
    static bool loadForFile(const QString& filePath, int pointsPerSecond, Payload* out);
    static bool inspectRenderCache(const QString& filePath, int pointsPerSecond,
                                   RenderInfo* out);
    static bool streamRenderCache(
        const QString& filePath, int pointsPerSecond,
        const std::function<bool()>& shouldCancel,
        const std::function<waveform::WaveformDemand()>& demandSnapshot,
        const RenderChunkCallback& publishChunk);
    static bool streamRenderLodCache(
        const QString& filePath, int pointsPerSecond, int level,
        const std::function<bool()>& shouldCancel,
        const std::function<void(LodTile)>& publishTile);
    static bool saveForFile(const QString& filePath, const Payload& payload);
};
