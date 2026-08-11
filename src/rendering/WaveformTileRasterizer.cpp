#include "WaveformTileRasterizer.h"

#include "WaveformMarkerLayout.h"
#include "waveform/WaveformLineStore.h"
#include "waveform/WaveformLodPyramid.h"
#include "waveform/WaveformVisualStyle.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace waveform_render {
namespace {

void updateWorst(std::atomic<std::uint64_t>& target, std::uint64_t value)
{
    auto previous = target.load(std::memory_order_relaxed);
    while (previous < value
           && !target.compare_exchange_weak(previous, value,
                                            std::memory_order_relaxed)) {
    }
}

std::size_t hashCombine(std::size_t seed, std::size_t value) noexcept
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

void writeAntialiasedVerticalStroke(QImage& image,
                                    int physicalX,
                                    double top,
                                    double bottom,
                                    int red,
                                    int green,
                                    int blue,
                                    int maximumAlpha)
{
    const int height = image.height();
    if (physicalX < 0 || physicalX >= image.width() || height <= 0)
        return;
    top = std::clamp(top, 0.0, static_cast<double>(height));
    bottom = std::clamp(bottom, top, static_cast<double>(height));
    if (bottom <= top)
        return;
    const int first = std::clamp(static_cast<int>(std::floor(top)), 0, height - 1);
    const int last = std::clamp(static_cast<int>(std::ceil(bottom)) - 1,
                                first, height - 1);
    for (int physicalY = first; physicalY <= last; ++physicalY) {
        const float coverage = waveform_visual::verticalPixelCoverage(
            top, bottom, physicalY);
        const int alpha = std::clamp(static_cast<int>(std::lround(
            static_cast<double>(maximumAlpha) * coverage)), 0, 255);
        if (alpha == 0)
            continue;
        reinterpret_cast<QRgb*>(image.scanLine(physicalY))[physicalX]
            = qPremultiply(qRgba(red, green, blue, alpha));
    }
}

} // namespace

std::size_t RenderTileKeyHash::operator()(const RenderTileKey& key) const noexcept
{
    std::size_t value = std::hash<std::uint64_t>{}(key.trackGeneration);
    value = hashCombine(value, std::hash<std::uint64_t>{}(key.sourceRevision));
    value = hashCombine(value, std::hash<std::int64_t>{}(key.tileIndex));
    value = hashCombine(value,
                        std::hash<std::uint64_t>{}(key.physicalPixelsPerLineMicros));
    value = hashCombine(value, std::hash<std::uint32_t>{}(key.imageHeight));
    value = hashCombine(value, std::hash<std::uint16_t>{}(key.devicePixelRatioMillis));
    return hashCombine(value, std::hash<std::uint8_t>{}(key.lodLevel));
}

WaveformTileRasterizer::WaveformTileRasterizer(
    std::function<void()> tileReadyCallback)
    : m_tileReadyCallback(std::move(tileReadyCallback)),
      m_worker([this](std::stop_token stopToken) { run(stopToken); })
{
}

WaveformTileRasterizer::~WaveformTileRasterizer()
{
    m_worker.request_stop();
    m_condition.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

RenderTileKey WaveformTileRasterizer::makeKey(
    const WaveformLineStoreSnapshot& snapshot,
    std::int64_t tileIndex,
    const RenderTileSpan& span,
    double physicalPixelsPerLine,
    int imageHeight,
    double devicePixelRatio,
    std::uint8_t lodLevel) noexcept
{
    return {
        snapshot.trackGeneration,
        waveform::WaveformLodPyramid::sourceRevision(
            snapshot, lodLevel, span.sourceBegin, span.sourceEnd),
        tileIndex,
        static_cast<std::uint64_t>(std::max(0.0,
            std::round(physicalPixelsPerLine * 1'000'000.0))),
        static_cast<std::uint32_t>(std::max(0, imageHeight)),
        static_cast<std::uint16_t>(std::clamp(
            std::lround(devicePixelRatio * 1000.0), 1L, 65'535L)),
        lodLevel
    };
}

std::shared_ptr<const RasterizedRenderTile>
WaveformTileRasterizer::find(const RenderTileKey& key)
{
    std::lock_guard lock(m_mutex);
    const auto found = m_cache.find(key);
    if (found == m_cache.end()) {
        m_cacheMisses.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    m_lru.splice(m_lru.begin(), m_lru, found->second.lruPosition);
    m_cacheHits.fetch_add(1, std::memory_order_relaxed);
    return found->second.tile;
}

void WaveformTileRasterizer::request(RenderTileRequest request)
{
    if (!request.snapshot || !request.span.hasSource())
        return;

    {
        std::lock_guard lock(m_mutex);
        if (request.key.trackGeneration != m_activeTrackGeneration
            || m_cache.contains(request.key)
            || m_pendingKeys.contains(request.key)) {
            return;
        }

        if (m_pending.size() >= kMaximumPendingRequests) {
            const auto worst = std::max_element(
                m_pending.begin(), m_pending.end(),
                [](const auto& left, const auto& right) {
                    return left.priority < right.priority;
                });
            if (worst != m_pending.end() && worst->priority > request.priority) {
                m_pendingKeys.erase(worst->key);
                m_pending.erase(worst);
            } else {
                return;
            }
        }
        m_pendingKeys.insert(request.key);
        m_pending.push_back(std::move(request));
    }
    m_condition.notify_one();
}

std::shared_ptr<const RasterizedOverview>
WaveformTileRasterizer::findOverview(const OverviewRenderKey& key) const
{
    std::lock_guard lock(m_mutex);
    return m_overview && m_overview->key == key ? m_overview : nullptr;
}

void WaveformTileRasterizer::requestOverview(OverviewRenderRequest request)
{
    if (!request.samples || request.samples->empty())
        return;
    {
        std::lock_guard lock(m_mutex);
        if (request.key.trackGeneration != m_activeTrackGeneration
            || (m_overview && m_overview->key == request.key)
            || (m_pendingOverview && m_pendingOverview->key == request.key)) {
            return;
        }
        // There is only one useful fallback: the newest visual generation.
        m_pendingOverview = std::move(request);
    }
    m_condition.notify_one();
}

void WaveformTileRasterizer::cancelPendingTiles()
{
    std::lock_guard lock(m_mutex);
    m_pending.clear();
    m_pendingKeys.clear();
}

void WaveformTileRasterizer::setActiveTrackGeneration(std::uint64_t generation)
{
    std::lock_guard lock(m_mutex);
    if (m_activeTrackGeneration == generation)
        return;
    m_activeTrackGeneration = generation;
    m_pending.clear();
    m_pendingOverview.reset();
    m_pendingKeys.clear();
    m_cache.clear();
    m_lru.clear();
    m_cacheBytes = 0;
    m_overview.reset();
}

void WaveformTileRasterizer::clear()
{
    std::lock_guard lock(m_mutex);
    m_pending.clear();
    m_pendingOverview.reset();
    m_pendingKeys.clear();
    m_cache.clear();
    m_lru.clear();
    m_cacheBytes = 0;
    m_activeTrackGeneration = 0;
    m_overview.reset();
}

WaveformTileRasterizer::Stats WaveformTileRasterizer::stats() const
{
    Stats result;
    result.cacheHits = m_cacheHits.load(std::memory_order_relaxed);
    result.cacheMisses = m_cacheMisses.load(std::memory_order_relaxed);
    result.rasterizedTiles = m_rasterizedTiles.load(std::memory_order_relaxed);
    result.discardedTiles = m_discardedTiles.load(std::memory_order_relaxed);
    result.worstRasterUsec = m_worstRasterUsec.load(std::memory_order_relaxed);
    std::lock_guard lock(m_mutex);
    result.cacheBytes = m_cacheBytes;
    result.cacheEntries = m_cache.size();
    result.pendingRequests = m_pending.size() + (m_pendingOverview ? 1u : 0u);
    return result;
}

void WaveformTileRasterizer::resetStats()
{
    m_cacheHits.store(0, std::memory_order_relaxed);
    m_cacheMisses.store(0, std::memory_order_relaxed);
    m_rasterizedTiles.store(0, std::memory_order_relaxed);
    m_discardedTiles.store(0, std::memory_order_relaxed);
    m_worstRasterUsec.store(0, std::memory_order_relaxed);
}

void WaveformTileRasterizer::run(std::stop_token stopToken)
{
    while (!stopToken.stop_requested()) {
        std::optional<RenderTileRequest> request;
        std::optional<OverviewRenderRequest> overviewRequest;
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, stopToken, [this] {
                return m_pendingOverview.has_value() || !m_pending.empty();
            });
            if (stopToken.stop_requested())
                break;
            if (m_pendingOverview) {
                overviewRequest.emplace(std::move(*m_pendingOverview));
                m_pendingOverview.reset();
            } else {
                const auto best = std::min_element(
                    m_pending.begin(), m_pending.end(),
                    [](const auto& left, const auto& right) {
                        return left.priority < right.priority;
                    });
                request.emplace(std::move(*best));
                m_pendingKeys.erase(request->key);
                m_pending.erase(best);
            }
        }

        QElapsedTimer timer;
        timer.start();
        if (overviewRequest) {
            auto overview = rasterizeOverview(*overviewRequest);
            bool accepted = false;
            {
                std::lock_guard lock(m_mutex);
                if (overview
                    && overview->key.trackGeneration == m_activeTrackGeneration) {
                    m_overview = std::move(overview);
                    accepted = true;
                }
            }
            if (accepted && m_tileReadyCallback)
                m_tileReadyCallback();
            continue;
        }
        auto tile = rasterize(*request);
        updateWorst(m_worstRasterUsec,
                    static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000));
        m_rasterizedTiles.fetch_add(1, std::memory_order_relaxed);
        insert(std::move(tile));
    }
}

std::shared_ptr<const RasterizedOverview>
WaveformTileRasterizer::rasterizeOverview(const OverviewRenderRequest& request)
{
    auto result = std::make_shared<RasterizedOverview>();
    result->key = request.key;
    if (!request.samples || request.samples->empty()
        || request.key.imageWidth == 0 || request.key.imageHeight == 0) {
        return result;
    }

    const int width = static_cast<int>(request.key.imageWidth);
    const int height = static_cast<int>(request.key.imageHeight);
    result->image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    result->image.fill(Qt::transparent);
    const double centre = static_cast<double>(height) * 0.5;
    const double halfHeight = std::max(1.0, centre - 2.0);

    for (int x = 0; x < width; ++x) {
        const auto begin = static_cast<std::size_t>(
            static_cast<std::uint64_t>(x) * request.samples->size()
            / static_cast<std::uint64_t>(width));
        const auto end = std::max(begin + 1, static_cast<std::size_t>(
            static_cast<std::uint64_t>(x + 1) * request.samples->size()
            / static_cast<std::uint64_t>(width)));
        float peak = 0.0f;
        float rmsSum = 0.0f;
        float low = 0.0f;
        float lowMid = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
        float colorWeight = 0.0f;
        for (auto index = begin;
             index < std::min(end, request.samples->size()); ++index) {
            const auto& sample = (*request.samples)[index];
            peak = std::max(peak, sample.rms);
            rmsSum += sample.rms;
            const float weight = std::max(0.01f, sample.rms);
            low += sample.low * weight;
            lowMid += sample.lowMid * weight;
            mid += sample.mid * weight;
            high += sample.high * weight;
            colorWeight += weight;
        }
        if (colorWeight <= 0.0f)
            continue;
        const float mean = rmsSum / static_cast<float>(std::max<std::size_t>(1, end - begin));
        const float energy = waveform_visual::foldedEnergy(mean, peak);
        if (energy <= 0.001f)
            continue;
        const float inverseWeight = 1.0f / colorWeight;
        const auto color = waveform_visual::color({
            low * inverseWeight, lowMid * inverseWeight,
            mid * inverseWeight, high * inverseWeight, energy});
        const double amplitude = waveform_visual::logarithmicAmplitude(energy);
        double top = centre - amplitude * halfHeight;
        double bottom = centre + amplitude * halfHeight;
        if (bottom - top < 1.0) {
            const double middle = (top + bottom) * 0.5;
            top = middle - 0.5;
            bottom = middle + 0.5;
        }
        writeAntialiasedVerticalStroke(
            result->image, x, top, bottom,
            color[0], color[1], color[2], 155);
    }
    return result;
}

void WaveformTileRasterizer::insert(
    std::shared_ptr<const RasterizedRenderTile> tile)
{
    if (!tile)
        return;
    bool accepted = false;
    {
        std::lock_guard lock(m_mutex);
        if (tile->key.trackGeneration == m_activeTrackGeneration) {
            const auto bytes = static_cast<std::size_t>(tile->image.sizeInBytes());
            m_lru.push_front(tile->key);
            auto [position, inserted] = m_cache.emplace(
                tile->key, CacheEntry{tile, bytes, m_lru.begin()});
            if (inserted) {
                m_cacheBytes += bytes;
                evictToBudgetLocked();
                accepted = true;
            } else {
                m_lru.pop_front();
            }
        }
    }

    if (!accepted) {
        m_discardedTiles.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (m_tileReadyCallback)
        m_tileReadyCallback();
}

void WaveformTileRasterizer::evictToBudgetLocked()
{
    while ((!m_lru.empty())
           && (m_cacheBytes > kMaximumCacheBytes
               || m_cache.size() > kMaximumCacheEntries)) {
        const auto key = m_lru.back();
        const auto found = m_cache.find(key);
        if (found != m_cache.end()) {
            m_cacheBytes -= found->second.bytes;
            m_cache.erase(found);
        }
        m_lru.pop_back();
    }
}

std::shared_ptr<const RasterizedRenderTile>
WaveformTileRasterizer::rasterize(const RenderTileRequest& request)
{
    auto result = std::make_shared<RasterizedRenderTile>();
    result->key = request.key;
    result->span = request.span;
    if (!request.snapshot || !request.snapshot->chunks
        || !request.span.hasSource()) {
        return result;
    }

    const int imageWidth = request.span.physicalWidth();
    const int imageHeight = static_cast<int>(request.key.imageHeight);
    if (imageWidth <= 0 || imageWidth > kRenderTilePhysicalWidth
        || imageHeight <= 0) {
        return result;
    }

    result->image = QImage(imageWidth, imageHeight,
                           QImage::Format_ARGB32_Premultiplied);
    result->image.fill(Qt::transparent);

    const auto verticalLayout = verticalMarkerLayout(
        static_cast<float>(request.logicalHeight));
    const double centerY = static_cast<double>(imageHeight) * 0.5;
    const double halfHeight = std::max(
        1.0, (request.logicalHeight - verticalLayout.waveformInset * 2.0)
            * request.devicePixelRatio * 0.5);
    bool allSourcePresent = true;

    for (int physicalX = 0; physicalX < imageWidth; ++physicalX) {
        const auto globalPhysicalX = request.span.physicalBegin + physicalX;
        if (!isWaveformStrokeColumn(globalPhysicalX))
            continue;
        if (!physicalStrokeIntersectsTrack(
                globalPhysicalX, request.physicalPixelsPerLine,
                request.snapshot->totalLineCount)) {
            continue;
        }

        const double sourceBeginExact = std::max(0.0,
            static_cast<double>(globalPhysicalX)
                / request.physicalPixelsPerLine);
        const double sourceEndExact = std::min(
            static_cast<double>(request.snapshot->totalLineCount),
            static_cast<double>(globalPhysicalX
                + kWaveformStrokePitchPhysicalPixels)
                / request.physicalPixelsPerLine);
        const auto globalBegin = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(std::floor(sourceBeginExact)),
            request.span.sourceBegin, request.span.sourceEnd - 1);
        const auto globalEnd = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil(sourceEndExact)),
            globalBegin + 1, request.span.sourceEnd);

        std::int16_t minimum = 0;
        std::int16_t maximum = 0;
        std::uint64_t red = 0;
        std::uint64_t green = 0;
        std::uint64_t blue = 0;
        std::uint64_t weight = 0;
        const auto stride = static_cast<std::uint32_t>(
            waveform::WaveformLodPyramid::level(
                request.key.lodLevel).canonicalLineStride);
        const auto lodBegin = globalBegin / stride;
        const auto lodEnd = (globalEnd + stride - 1) / stride;
        for (auto lodIndex = lodBegin; lodIndex < lodEnd; ++lodIndex) {
            const auto lodSample = waveform::WaveformLodPyramid::sample(
                *request.snapshot, request.key.lodLevel, lodIndex);
            if (!lodSample.complete)
                allSourcePresent = false;
            if (!lodSample.hasData)
                continue;
            const auto& line = lodSample.line;
            minimum = std::min(minimum, line.minimum);
            maximum = std::max(maximum, line.maximum);
            const auto magnitude = static_cast<std::uint32_t>(std::max(
                std::abs(static_cast<int>(line.minimum)),
                std::abs(static_cast<int>(line.maximum))));
            const auto lineWeight = std::max(1u, magnitude);
            red += static_cast<std::uint64_t>(line.red) * lineWeight;
            green += static_cast<std::uint64_t>(line.green) * lineWeight;
            blue += static_cast<std::uint64_t>(line.blue) * lineWeight;
            weight += lineWeight;
        }
        if (weight == 0)
            continue;

        result->hasAnySourceData = true;
        double top = centerY - (static_cast<double>(maximum) / 32767.0) * halfHeight;
        double bottom = centerY - (static_cast<double>(minimum) / 32767.0) * halfHeight;
        if ((minimum != 0 || maximum != 0) && bottom - top < 1.0) {
            const double middle = (top + bottom) * 0.5;
            top = middle - 0.5;
            bottom = middle + 0.5;
        }

        const int r = static_cast<int>(red / weight);
        const int g = static_cast<int>(green / weight);
        const int b = static_cast<int>(blue / weight);
        writeAntialiasedVerticalStroke(
            result->image, physicalX, top, bottom, r, g, b, 248);
        ++result->renderedColumns;
    }
    result->hasCompleteSourceData = allSourcePresent && result->hasAnySourceData;
    return result;
}

} // namespace waveform_render
