#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>

namespace waveform_render {

// One aggregated envelope column per physical display pixel. The former
// one-column-on / one-column-off mask formed a 1 px spatial carrier. Moving or
// scaling that carrier through fractional pixel phases produces a visible
// moire pattern: the same peak alternates between thin, thick and half-bright.
// A full-resolution envelope has no artificial carrier to alias and lets the
// GPU interpolate neighbouring *audio* columns during smooth scrolling.
inline constexpr std::int64_t kWaveformStrokePitchPhysicalPixels = 1;
inline constexpr std::size_t kWaveformTilePoolSize = 24;

inline bool isWaveformStrokeColumn(std::int64_t globalPhysicalColumn) noexcept
{
    return globalPhysicalColumn % kWaveformStrokePitchPhysicalPixels == 0;
}

inline std::int64_t timelinePhysicalFloor(double linePosition,
                                          double pixelsPerLine,
                                          double devicePixelRatio) noexcept
{
    const double dpr = std::max(1.0, devicePixelRatio);
    return static_cast<std::int64_t>(
        std::floor(linePosition * pixelsPerLine * dpr));
}

inline std::int64_t timelinePhysicalCeil(double linePosition,
                                         double pixelsPerLine,
                                         double devicePixelRatio) noexcept
{
    const double dpr = std::max(1.0, devicePixelRatio);
    return static_cast<std::int64_t>(
        std::ceil(linePosition * pixelsPerLine * dpr));
}

struct VerticalMarkerLayout {
    float waveformInset = 0.0f;
    float regularTickLength = 0.0f;
    float downbeatTickLength = 0.0f;
    float cueLinePhysicalWidth = 3.0f;
};

inline VerticalMarkerLayout verticalMarkerLayout(float height) noexcept
{
    const float h = std::max(0.0f, height);
    const float regular = std::min(
        std::clamp(h * 0.06f, 4.0f, 8.0f), h * 0.18f);
    const float downbeat = std::min(regular + 2.0f, h * 0.24f);
    const float inset = std::min(
        std::max(downbeat + 2.0f, h * 0.075f),
        std::min(14.0f, h * 0.32f));
    return {inset, regular, downbeat, 3.0f};
}

inline double snappedTimelineX(double linePosition,
                               double originLine,
                               double pixelsPerLine,
                               double devicePixelRatio) noexcept
{
    (void)devicePixelRatio;
    return (linePosition - originLine) * pixelsPerLine;
}

inline double pixelAlignedTimelineOrigin(double linePosition,
                                         double pixelsPerLine,
                                         double devicePixelRatio) noexcept
{
    const double dpr = std::max(1.0, devicePixelRatio);
    const double physicalPixelsPerLine = pixelsPerLine * dpr;
    if (physicalPixelsPerLine <= 0.0)
        return linePosition;
    return std::round(linePosition * physicalPixelsPerLine)
        / physicalPixelsPerLine;
}

inline double physicalPixelCenter(double logicalPosition,
                                  double devicePixelRatio) noexcept
{
    const double dpr = std::max(1.0, devicePixelRatio);
    if (!std::isfinite(logicalPosition))
        return 0.0;
    return (std::floor(logicalPosition * dpr) + 0.5) / dpr;
}

inline double viewportPhysicalPixelCenter(double logicalWidth,
                                          double devicePixelRatio) noexcept
{
    return physicalPixelCenter(std::max(0.0, logicalWidth) * 0.5,
                               devicePixelRatio);
}

inline double smoothTimelineTranslation(double width,
                                        double playheadLine,
                                        double originLine,
                                        double pixelsPerLine,
                                        double devicePixelRatio) noexcept
{
    return viewportPhysicalPixelCenter(width, devicePixelRatio)
        - (playheadLine - originLine) * pixelsPerLine;
}

// Render tiles live on one track-wide physical-pixel grid. Their texture size
// is deliberately independent of analysis chunk boundaries, zoom and DPR.
inline constexpr std::int64_t kRenderTilePhysicalWidth = 1024;
// Linear filtering needs one real neighbour on either side of a tile. Without
// this gutter, the sampler clamps at each texture edge and the waveform makes
// a small brightness/shape jump whenever a feature crosses a tile boundary.
// Only the 1024 core columns are displayed; the two extra columns are filter
// support and do not change the track-wide physical-pixel grid.
inline constexpr int kRenderTileFilterGutterPhysicalPixels = 1;
inline constexpr int kRenderTileTexturePhysicalWidth
    = static_cast<int>(kRenderTilePhysicalWidth)
    + 2 * kRenderTileFilterGutterPhysicalPixels;

// Rasterizing at exactly the on-screen scale is correct but brittle: moving the
// tempo fader retunes pixels-per-line on every frame, and because the scale is
// part of every tile key, the whole tile cache is invalidated at frame rate --
// the waveform blanks for as long as the fader moves. The beatgrid stays smooth
// through the same gesture only because it is pure geometry, with no cached
// pixels keyed on scale.
//
// So snap the *rasterization* scale to a geometric ladder and let the
// destination rectangle carry the exact scale instead. Tiles then survive an
// entire tempo sweep and are merely repositioned each frame, exactly like the
// grid, while a replacement is rasterized in the background only when a ladder
// step is actually crossed. A sixteenth-octave ladder keeps a texture within
// 4.5% of its natural size, which is below the threshold where the stretch
// reads as blur.
inline constexpr double kRasterScaleLadderStepsPerOctave = 16.0;

inline double quantizedRasterScale(double physicalPixelsPerLine) noexcept
{
    if (!(physicalPixelsPerLine > 0.0)
        || !std::isfinite(physicalPixelsPerLine)) {
        return physicalPixelsPerLine;
    }
    const double step = std::round(std::log2(physicalPixelsPerLine)
                                   * kRasterScaleLadderStepsPerOctave);
    return std::exp2(step / kRasterScaleLadderStepsPerOctave);
}

inline bool rasterScalesWithin(double rasterScale,
                               double displayScale,
                               double relativeTolerance) noexcept
{
    return std::abs(rasterScale - displayScale)
        <= std::max(displayScale, 1.0e-9) * relativeTolerance;
}

inline bool rasterScaleMatchesDisplay(double rasterScale,
                                      double displayScale) noexcept
{
    return rasterScalesWithin(rasterScale, displayScale, 1.0e-6);
}

// The ladder is the right answer only *while* the scale is moving. At rest it
// leaves every texture permanently stretched by up to ~2%, so roughly every
// 45th physical column is resampled from two texels while its neighbours are
// not. That is a stationary thick/thin ripple standing across the view -- the
// exact artefact the ladder was never meant to introduce.
//
// So treat the ladder as a gesture fallback: once the display scale has held
// still the tiles are re-cut at the exact scale, where one texel is one
// physical pixel and the destination rectangle is a whole number of pixels
// wide. Re-cutting costs one visible transition, which is acceptable at the
// end of a fader move and unacceptable as a permanent ripple.
inline constexpr int kRasterScaleSettleFrames = 8;
// A re-cut invalidates every tile key, so never re-cut for an error that
// cannot be seen. 0.1% is under one texel across a whole 1024-column tile and
// absorbs the slow tempo drift a running sync coordinator produces.
inline constexpr double kRasterScaleStickyTolerance = 1.0e-3;
// Anything coarser than this counts as the scale genuinely moving this frame.
inline constexpr double kRasterScaleMotionTolerance = 1.0e-9;

// Carried across frames by the render thread; never touched by audio or GUI.
struct RasterScaleTracker final {
    double rasterScale = 0.0;
    double lastDisplayScale = 0.0;
    int stableFrames = 0;
};

inline double selectRasterScale(RasterScaleTracker& tracker,
                                double displayScale) noexcept
{
    if (!(displayScale > 0.0) || !std::isfinite(displayScale)) {
        tracker.stableFrames = 0;
        return displayScale;
    }

    // A first frame has no motion history. Cut it exactly: a track load
    // rasterizes everything anyway, so there is no transition to pay for.
    const bool hasHistory = tracker.lastDisplayScale > 0.0;
    const bool held = hasHistory
        && rasterScalesWithin(displayScale, tracker.lastDisplayScale,
                              kRasterScaleMotionTolerance);
    tracker.lastDisplayScale = displayScale;
    tracker.stableFrames = held
        ? std::min(tracker.stableFrames + 1, kRasterScaleSettleFrames)
        : 0;

    if (tracker.rasterScale > 0.0
        && rasterScalesWithin(displayScale, tracker.rasterScale,
                              kRasterScaleStickyTolerance)) {
        return tracker.rasterScale;
    }

    tracker.rasterScale
        = (!hasHistory || tracker.stableFrames >= kRasterScaleSettleFrames)
        ? displayScale
        : quantizedRasterScale(displayScale);
    return tracker.rasterScale;
}

// A settle only completes if frames keep arriving. A paused deck stops asking
// for frames the moment the fader is released, which would strand the view on
// the ladder cut indefinitely, so the renderer has to request the remaining
// frames itself.
//
// This cannot run away. While the scale holds still, stableFrames climbs and
// the request stops at kRasterScaleSettleFrames. While the scale keeps moving,
// stableFrames resets to 0 every frame, so this asks for exactly one frame per
// scale change -- and a scale change already triggers a frame of its own, so
// the request is redundant rather than additional.
inline bool rasterScaleSettlePending(const RasterScaleTracker& tracker,
                                     double displayScale) noexcept
{
    return tracker.rasterScale > 0.0
        && tracker.stableFrames < kRasterScaleSettleFrames
        && !rasterScalesWithin(displayScale, tracker.rasterScale,
                               kRasterScaleStickyTolerance);
}

// The coarse whole-track fallback overview is rasterized once at a fixed texel
// count for the entire track, so one texel already aggregates many canonical
// lines. Drawing it underneath the detail tiles is only honest while each of
// its texels still maps to roughly one physical pixel. Magnified further it
// stops being "coarse context" and becomes fake detail: a single aggregated
// sample smeared across dozens of pixels, which reads as a broad solid block
// of waveform that was never in the audio. At a typical deck zoom
// (0.22 px/line) a 7-minute track is ~504k lines wide, so a 2048-texel
// overview would be magnified ~54x — exactly the broad blocks seen before
// real detail arrives. Past the budget below the neutral background is the
// truthful placeholder until genuine detail tiles are ready.
inline constexpr double kMaximumFallbackOverviewMagnification = 2.0;

// Physical pixels each fallback-overview texel is stretched across.
inline double fallbackOverviewMagnification(
    std::uint32_t textureTexelWidth,
    std::uint32_t totalLineCount,
    double pixelsPerLine,
    double devicePixelRatio) noexcept
{
    if (textureTexelWidth == 0 || totalLineCount == 0
        || !(pixelsPerLine > 0.0) || !std::isfinite(pixelsPerLine)) {
        return 0.0;
    }
    const double dpr = std::max(1.0, devicePixelRatio);
    const double physicalWidth =
        static_cast<double>(totalLineCount) * pixelsPerLine * dpr;
    return physicalWidth / static_cast<double>(textureTexelWidth);
}

inline bool fallbackOverviewMayRepresentDetail(double magnification) noexcept
{
    return magnification > 0.0
        && magnification <= kMaximumFallbackOverviewMagnification;
}

enum class WaveformCoverage : std::uint8_t {
    Missing,
    Fallback,
    HighResolution
};

inline WaveformCoverage bestAvailableCoverage(bool highResolutionReady,
                                              bool fallbackReady) noexcept
{
    if (highResolutionReady)
        return WaveformCoverage::HighResolution;
    return fallbackReady ? WaveformCoverage::Fallback
                         : WaveformCoverage::Missing;
}

// A detail tile may be shown as soon as it carries any real audio, even if
// part of its source range is still being analysed. Requiring every source
// line to be present meant a tile spanning any not-yet-analysed region was
// discarded whole, so nothing appeared until analysis had swept past it.
// That rule only existed so the coarse whole-track overview could not bleed
// through a partly-drawn tile's gaps — and that overview is now suppressed
// whenever it would be magnified into fake detail, so the reason is gone.
// Columns without data simply stay background; the tile's key carries the
// source revision, so a better version replaces it automatically as chunks
// land.
inline bool detailTileMayBeDisplayed(bool keyIsCurrent,
                                     bool hasAnySourceData) noexcept
{
    return keyIsCurrent && hasAnySourceData;
}


struct RenderTileSpan final {
    std::int64_t tileIndex = 0;
    std::int64_t physicalBegin = 0;
    std::int64_t physicalEnd = 0;
    std::uint32_t sourceBegin = 0;
    std::uint32_t sourceEnd = 0;

    [[nodiscard]] int physicalWidth() const noexcept
    {
        return static_cast<int>(physicalEnd - physicalBegin);
    }

    [[nodiscard]] bool hasSource() const noexcept
    {
        return sourceBegin < sourceEnd;
    }
};

inline std::int64_t floorDiv(std::int64_t numerator,
                             std::int64_t denominator) noexcept
{
    if (denominator <= 0)
        return 0;
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    return remainder < 0 ? quotient - 1 : quotient;
}

inline std::int64_t firstRenderTile(std::int64_t physicalBegin) noexcept
{
    return floorDiv(physicalBegin, kRenderTilePhysicalWidth);
}

inline std::int64_t lastRenderTile(std::int64_t physicalEnd) noexcept
{
    if (physicalEnd <= std::numeric_limits<std::int64_t>::min() + 1)
        return firstRenderTile(physicalEnd);
    return firstRenderTile(physicalEnd - 1);
}

// A global tile always owns the same scene-graph slot. Sequentially assigning
// visible tiles to slots makes every surviving tile move to a different node
// when the guard window advances by one tile, causing a burst of texture
// replacements and sometimes a fallback flash. Modulo ownership keeps all
// overlapping tiles resident; a contiguous window no wider than the pool has
// no collisions, including for negative pre-roll tile indices.
inline std::size_t renderTilePoolSlot(std::int64_t tileIndex,
                                     std::size_t poolSize
                                         = kWaveformTilePoolSize) noexcept
{
    if (poolSize == 0)
        return 0;
    const auto modulus = static_cast<std::int64_t>(poolSize);
    auto slot = tileIndex % modulus;
    if (slot < 0)
        slot += modulus;
    return static_cast<std::size_t>(slot);
}

inline RenderTileSpan renderTileSpan(std::int64_t tileIndex,
                                     double physicalPixelsPerLine,
                                     std::uint32_t totalLineCount) noexcept
{
    RenderTileSpan span;
    span.tileIndex = tileIndex;
    span.physicalBegin = tileIndex * kRenderTilePhysicalWidth;
    span.physicalEnd = span.physicalBegin + kRenderTilePhysicalWidth;
    if (!(physicalPixelsPerLine > 0.0) || !std::isfinite(physicalPixelsPerLine)
        || totalLineCount == 0) {
        return span;
    }

    const double first = std::floor(
        static_cast<double>(span.physicalBegin) / physicalPixelsPerLine);
    const double last = std::ceil(
        static_cast<double>(span.physicalEnd) / physicalPixelsPerLine);
    const auto clampLine = [totalLineCount](double line) {
        if (line <= 0.0)
            return std::uint32_t{0};
        if (line >= static_cast<double>(totalLineCount))
            return totalLineCount;
        return static_cast<std::uint32_t>(line);
    };
    span.sourceBegin = clampLine(first);
    span.sourceEnd = clampLine(last);
    return span;
}

struct RenderTileRasterSourceSpan final {
    std::uint32_t sourceBegin = 0;
    std::uint32_t sourceEnd = 0;
};

// Include the filter gutters in a tile's source revision. Otherwise a newly
// published source line just across a tile boundary could leave the cached
// neighbour texel stale even though the visible core key still looked current.
inline RenderTileRasterSourceSpan renderTileRasterSourceSpan(
    const RenderTileSpan& span,
    double physicalPixelsPerLine,
    std::uint32_t totalLineCount) noexcept
{
    if (!(physicalPixelsPerLine > 0.0)
        || !std::isfinite(physicalPixelsPerLine)
        || totalLineCount == 0) {
        return {};
    }

    const double gutter = static_cast<double>(
        kRenderTileFilterGutterPhysicalPixels);
    const double first = std::floor(
        (static_cast<double>(span.physicalBegin) - gutter)
        / physicalPixelsPerLine);
    const double last = std::ceil(
        (static_cast<double>(span.physicalEnd) + gutter)
        / physicalPixelsPerLine);
    const auto clampLine = [totalLineCount](double line) {
        if (line <= 0.0)
            return std::uint32_t{0};
        if (line >= static_cast<double>(totalLineCount))
            return totalLineCount;
        return static_cast<std::uint32_t>(line);
    };
    return {clampLine(first), clampLine(last)};
}

inline bool physicalStrokeIntersectsTrack(
    std::int64_t physicalColumn,
    double physicalPixelsPerLine,
    std::uint32_t totalLineCount) noexcept
{
    if (!(physicalPixelsPerLine > 0.0)
        || !std::isfinite(physicalPixelsPerLine)
        || totalLineCount == 0) {
        return false;
    }
    const double strokeBegin = static_cast<double>(physicalColumn);
    const double strokeEnd = strokeBegin
        + static_cast<double>(kWaveformStrokePitchPhysicalPixels);
    const double trackEnd = static_cast<double>(totalLineCount)
        * physicalPixelsPerLine;
    return strokeEnd > 0.0 && strokeBegin < trackEnd;
}

inline double timelinePixelsPerSecond(double pixelsPerPoint,
                                      double waveformPointsPerSecond,
                                      double tempoRatio) noexcept
{
    if (!std::isfinite(pixelsPerPoint)
        || !std::isfinite(waveformPointsPerSecond)
        || !std::isfinite(tempoRatio)) {
        return 0.0;
    }
    return std::max(0.0, pixelsPerPoint)
        * std::max(0.0, waveformPointsPerSecond)
        / std::max(0.05, std::abs(tempoRatio));
}

inline double timelineScreenX(double viewportCenter,
                              double timelineSeconds,
                              double playheadSeconds,
                              double pixelsPerSecond) noexcept
{
    if (!std::isfinite(viewportCenter)
        || !std::isfinite(timelineSeconds)
        || !std::isfinite(playheadSeconds)
        || !std::isfinite(pixelsPerSecond)) {
        return viewportCenter;
    }
    return viewportCenter
        + (timelineSeconds - playheadSeconds) * pixelsPerSecond;
}

inline double screenDeltaToTimelineSeconds(double screenDelta,
                                           double pixelsPerSecond) noexcept
{
    if (!std::isfinite(screenDelta) || !(pixelsPerSecond > 0.0)
        || !std::isfinite(pixelsPerSecond)) {
        return 0.0;
    }
    return screenDelta / pixelsPerSecond;
}

} // namespace waveform_render
