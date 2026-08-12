#include "ScrollingWaveformItem.h"

#include "TrackData.h"
#include "app/WaveformZoomController.h"
#include "WaveformRenderMath.h"
#include "WaveformTileRasterizer.h"
#include "waveform/WaveformLineStore.h"

#include <QColor>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QImage>
#include <QMatrix4x4>
#include <QMetaObject>
#include <QPainter>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

namespace {

constexpr std::size_t kWaveformNodePoolSize = 24;
constexpr std::size_t kCueLabelNodePoolSize = 9;
constexpr std::size_t kDownbeatLabelNodePoolSize = 48;
constexpr int kVerticesPerFeatheredLine = 18;

QSGGeometryNode* makeLineNode(QSGNode* parent)
{
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    // Backend line primitives can be rasterised as one or two pixels depending
    // on the graphics API and DPR. Explicit triangles give us an exact
    // one-physical-pixel rectangle on every backend.
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);

    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending, true);

    auto* node = new QSGGeometryNode();
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);
    parent->appendChildNode(node);
    return node;
}

QSGGeometryNode* makeTriangleNode(QSGNode* parent)
{
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);

    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending, true);

    auto* node = new QSGGeometryNode();
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);
    parent->appendChildNode(node);
    return node;
}

QSGSimpleTextureNode* makeWaveformTextureNode()
{
    auto* node = new QSGSimpleTextureNode();
    node->setOwnsTexture(true);
    // The image contains intentional one-physical-pixel gaps. Linear sampling
    // blends neighbouring strokes back into a solid slab while zooming.
    node->setFiltering(QSGTexture::Nearest);
    return node;
}

QSGSimpleTextureNode* assignTextureNode(QSGSimpleTextureNode*& node,
                                        QSGTexture* texture,
                                        QSGNode* parent,
                                        QSGNode* insertBefore)
{
    if (node) {
        auto* previousTexture = node->texture();
        node->setOwnsTexture(false);
        node->setTexture(texture);
        delete previousTexture;
        node->setOwnsTexture(true);
        return node;
    }
    node = makeWaveformTextureNode();
    // QSGSimpleTextureNode must have a valid texture before it becomes part of
    // the scene graph. Vulkan's opaque-material batcher dereferences it while
    // sorting, even when the target rectangle is empty.
    node->setTexture(texture);
    if (insertBefore)
        parent->insertChildNodeBefore(node, insertBefore);
    else
        parent->appendChildNode(node);
    return node;
}

void clearGeometry(QSGGeometryNode* node)
{
    if (node->geometry()->vertexCount() == 0)
        return;
    node->geometry()->allocate(0);
    node->markDirty(QSGNode::DirtyGeometry);
}

void clearWaveformTexture(QSGSimpleTextureNode* node)
{
    if (node)
        node->setRect({});
}

void destroyTextureNode(QSGNode* parent, QSGSimpleTextureNode*& node)
{
    if (!node)
        return;
    parent->removeChildNode(node);
    delete node;
    node = nullptr;
}

struct MarkerLine {
    double linePosition = 0.0;
    QColor color;
    float top = 0.0f;
    float bottom = 0.0f;
    float physicalWidth = 1.0f;
};

struct CueLabel {
    double linePosition = 0.0;
    QColor color;
    QString text;
};

struct PreparedTileSlot {
    std::size_t poolIndex = 0;
    waveform_render::RenderTileSpan span;
    waveform_render::RenderTileKey key;
    std::shared_ptr<const waveform_render::RasterizedRenderTile> ready;
    bool alreadyDisplayed = false;
};

struct WaveformSceneNode final : QSGClipNode {
    WaveformSceneNode()
    {
        setIsRectangular(true);
        timeline = new QSGTransformNode();
        appendChildNode(timeline);

        // Translucent overlays sit below the audio lines.
        loopFill = makeTriangleNode(timeline);
        // Created lazily once an immediate overview is ready. It remains below
        // every high-resolution tile and prevents transparent loading holes.
        regularBeats = makeLineNode(timeline);
        downbeats = makeLineNode(timeline);
        loopEdges = makeLineNode(timeline);
        cueLines = makeLineNode(timeline);
    }

    void clearAllGeometry()
    {
        clearGeometry(loopFill);
        destroyTextureNode(timeline, fallbackNode);
        fallbackKey.reset();
        fallbackRequestedKey.reset();
        fallbackVisible = false;
        fallbackTextureBytes = 0;
        viewKey.reset();
        for (std::size_t index = 0; index < waveformNodes.size(); ++index) {
            destroyTextureNode(timeline, waveformNodes[index]);
            waveformTileKeys[index].reset();
            waveformRenderedLineCounts[index] = 0;
            waveformTextureBytes[index] = 0;
        }
        clearGeometry(regularBeats);
        clearGeometry(downbeats);
        for (auto& label : downbeatLabels)
            destroyTextureNode(timeline, label);
        downbeatLabelTexts.fill(QString());
        downbeatLabelColors.fill(QColor());
        clearGeometry(loopEdges);
        clearGeometry(cueLines);
        for (auto* label : cueLabels) {
            if (label)
                label->setRect({});
        }
        // The hidden node keeps its old texture until reused; clear the
        // content cache too, or a coincidental text/colour match on the next
        // track would skip repainting and reveal stale content.
        cueLabelTexts.fill(QString());
        cueLabelColors.fill(QColor());
        loopVisible = false;
        loopInLine = std::numeric_limits<double>::quiet_NaN();
        loopOutLine = std::numeric_limits<double>::quiet_NaN();
        loopAppearance = -1;
    }

    QSGTransformNode* timeline = nullptr;
    QSGGeometryNode* loopFill = nullptr;
    QSGSimpleTextureNode* fallbackNode = nullptr;
    std::array<QSGSimpleTextureNode*, kWaveformNodePoolSize> waveformNodes{};
    QSGGeometryNode* regularBeats = nullptr;
    QSGGeometryNode* downbeats = nullptr;
    std::array<QSGSimpleTextureNode*, kDownbeatLabelNodePoolSize> downbeatLabels{};
    // Last-painted content per label slot, so a rebase that leaves a badge's
    // text/colour unchanged can reposition it instead of repainting and
    // re-uploading its GPU texture (see replaceCueLabelNode /
    // updateDownbeatCounterNode).
    std::array<QString, kDownbeatLabelNodePoolSize> downbeatLabelTexts{};
    std::array<QColor, kDownbeatLabelNodePoolSize> downbeatLabelColors{};
    QSGGeometryNode* loopEdges = nullptr;
    QSGGeometryNode* cueLines = nullptr;
    std::array<QSGSimpleTextureNode*, kCueLabelNodePoolSize> cueLabels{};
    std::array<QString, kCueLabelNodePoolSize> cueLabelTexts{};
    std::array<QColor, kCueLabelNodePoolSize> cueLabelColors{};
    std::array<std::optional<waveform_render::RenderTileKey>,
               kWaveformNodePoolSize> waveformTileKeys{};
    std::array<std::uint64_t, kWaveformNodePoolSize> waveformRenderedLineCounts{};
    std::array<std::uint64_t, kWaveformNodePoolSize> waveformTextureBytes{};
    std::optional<waveform_render::OverviewRenderKey> fallbackKey;
    std::optional<waveform_render::OverviewRenderKey> fallbackRequestedKey;
    std::optional<waveform_render::WaveformViewKey> viewKey;
    // Whether the coarse fallback currently occupies a non-empty rect. It is
    // hidden whenever the zoom would magnify it past the point of being
    // honest context (see kMaximumFallbackOverviewMagnification), so the
    // transition back to visible has to reposition it for the current origin.
    bool fallbackVisible = false;
    std::uint64_t fallbackTextureBytes = 0;
    std::uint64_t viewGeneration = 0;

    bool hasWindow = false;
    std::uint64_t trackGeneration = 0;
    std::uint64_t dataGeneration = 0;
    std::int64_t windowStartLine = 0;
    std::int64_t windowEndLine = 0;
    double innerStartLine = 0.0;
    double innerEndLine = 0.0;
    double renderOriginLine = 0.0;
    double pixelsPerLine = 0.0;
    double devicePixelRatio = 1.0;
    QSizeF renderedSize;
    QRectF clipBounds;
    bool loopVisible = false;
    double loopInLine = std::numeric_limits<double>::quiet_NaN();
    double loopOutLine = std::numeric_limits<double>::quiet_NaN();
    int loopAppearance = -1;
};

void setPremultipliedVertex(QSGGeometry::ColoredPoint2D& vertex,
                            float x,
                            float y,
                            uchar red,
                            uchar green,
                            uchar blue,
                            uchar alpha)
{
    const auto premultiply = [alpha](uchar channel) {
        return static_cast<uchar>((static_cast<unsigned int>(channel) * alpha + 127u) / 255u);
    };
    vertex.set(x, y, premultiply(red), premultiply(green), premultiply(blue), alpha);
}

void writeSolidQuad(QSGGeometry::ColoredPoint2D* vertices,
                    int& out,
                    float left,
                    float right,
                    float top,
                    float bottom,
                    uchar red,
                    uchar green,
                    uchar blue,
                    uchar leftAlpha,
                    uchar rightAlpha)
{
    setPremultipliedVertex(vertices[out++], left, top, red, green, blue, leftAlpha);
    setPremultipliedVertex(vertices[out++], right, top, red, green, blue, rightAlpha);
    setPremultipliedVertex(vertices[out++], left, bottom, red, green, blue, leftAlpha);
    setPremultipliedVertex(vertices[out++], left, bottom, red, green, blue, leftAlpha);
    setPremultipliedVertex(vertices[out++], right, top, red, green, blue, rightAlpha);
    setPremultipliedVertex(vertices[out++], right, bottom, red, green, blue, rightAlpha);
}

void writeFeatheredVerticalLine(QSGGeometry::ColoredPoint2D* vertices,
                                int& out,
                                float x,
                                float top,
                                float bottom,
                                float corePhysicalWidth,
                                double devicePixelRatio,
                                uchar red,
                                uchar green,
                                uchar blue,
                                uchar alpha)
{
    const float inverseDpr = static_cast<float>(1.0 / std::max(1.0, devicePixelRatio));
    const float halfCore = std::max(0.5f, corePhysicalWidth) * inverseDpr * 0.5f;
    const float feather = inverseDpr * 0.5f;
    const float coreLeft = x - halfCore;
    const float coreRight = x + halfCore;
    writeSolidQuad(vertices, out, coreLeft - feather, coreLeft, top, bottom,
                   red, green, blue, 0, alpha);
    writeSolidQuad(vertices, out, coreLeft, coreRight, top, bottom,
                   red, green, blue, alpha, alpha);
    writeSolidQuad(vertices, out, coreRight, coreRight + feather, top, bottom,
                   red, green, blue, alpha, 0);
}

void writeMarkerGeometry(QSGGeometryNode* node,
                         const MarkerLine* lines,
                         std::size_t lineCount,
                         double originLine,
                         double pixelsPerLine,
                         double devicePixelRatio)
{
    auto* geometry = node->geometry();
    geometry->allocate(static_cast<int>(lineCount * kVerticesPerFeatheredLine));
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    int out = 0;
    for (std::size_t lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        const auto& marker = lines[lineIndex];
        const float x = static_cast<float>(waveform_render::snappedTimelineX(
            marker.linePosition, originLine, pixelsPerLine, devicePixelRatio));
        const uchar r = static_cast<uchar>(marker.color.red());
        const uchar g = static_cast<uchar>(marker.color.green());
        const uchar b = static_cast<uchar>(marker.color.blue());
        const uchar a = static_cast<uchar>(marker.color.alpha());
        writeFeatheredVerticalLine(vertices, out, x, marker.top, marker.bottom,
                                   marker.physicalWidth, devicePixelRatio,
                                   r, g, b, a);
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

void writeMarkerGeometry(QSGGeometryNode* node,
                         const std::vector<MarkerLine>& lines,
                         double originLine,
                         double pixelsPerLine,
                         double devicePixelRatio)
{
    writeMarkerGeometry(node, lines.data(), lines.size(), originLine,
                        pixelsPerLine, devicePixelRatio);
}

QSGSimpleTextureNode* replaceCueLabelNode(QSGSimpleTextureNode* previous,
                                          QSGNode* parent,
                                          const CueLabel& label,
                                          QQuickWindow* window,
                                          double originLine,
                                          double pixelsPerLine,
                                          double devicePixelRatio,
                                          QString& cachedText,
                                          QColor& cachedColor)
{
    if (!window) {
        if (previous)
            previous->setRect({});
        return previous;
    }

    constexpr qreal badgeHeight = 16.0;
    constexpr int maximumTextWidth = 86;
    QFont font;
    font.setBold(true);
    font.setPixelSize(9);
    const QFontMetrics metrics(font);
    const QString text = metrics.elidedText(
        label.text.isEmpty() ? QStringLiteral("CUE") : label.text,
        Qt::ElideRight, maximumTextWidth);
    const qreal badgeWidth = std::clamp<qreal>(
        static_cast<qreal>(metrics.horizontalAdvance(text) + 10), 22.0, 96.0);
    const double x = waveform_render::snappedTimelineX(
        label.linePosition, originLine, pixelsPerLine, devicePixelRatio);

    // Repositioning is one setRect() call; repainting is a QImage fill plus
    // a fresh GPU texture upload. Hot cue badges keep the same text/colour
    // across most guard-window rebases (the cues themselves don't move), so
    // skip the repaint — and the texture churn that comes with it — when
    // nothing about the badge's appearance actually changed.
    if (previous && cachedText == text && cachedColor == label.color) {
        previous->setRect(QRectF(x - badgeWidth * 0.5, 0.0, badgeWidth, badgeHeight));
        return previous;
    }

    const qreal dpr = std::max(1.0, devicePixelRatio);
    QImage image(QSize(static_cast<int>(std::ceil(badgeWidth * dpr)),
                       static_cast<int>(std::ceil(badgeHeight * dpr))),
                 QImage::Format_RGBA8888_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor fill = label.color.isValid() ? label.color : QColor(224, 64, 64);
    fill.setAlpha(245);
    painter.setBrush(fill);
    painter.setPen(QPen(QColor(0, 0, 0, 180), 1.0));
    painter.drawRoundedRect(QRectF(0.5, 0.5, badgeWidth - 1.0, badgeHeight - 1.0), 2.0, 2.0);
    const int brightness = (fill.red() * 299 + fill.green() * 587 + fill.blue() * 114) / 1000;
    painter.setPen(brightness < 145 ? QColor(248, 248, 248) : QColor(17, 17, 17));
    painter.setFont(font);
    painter.drawText(QRectF(2.0, 0.0, badgeWidth - 4.0, badgeHeight),
                     Qt::AlignCenter, text);
    painter.end();

    auto* texture = window->createTextureFromImage(image);
    if (!texture) {
        clearWaveformTexture(previous);
        return previous;
    }
    auto* node = new QSGSimpleTextureNode();
    node->setTexture(texture);
    node->setOwnsTexture(true);
    node->setFiltering(QSGTexture::Linear);
    node->setRect(QRectF(x - badgeWidth * 0.5, 0.0, badgeWidth, badgeHeight));
    if (previous) {
        parent->removeChildNode(previous);
        delete previous;
    }
    parent->appendChildNode(node);
    cachedText = text;
    cachedColor = label.color;
    return node;
}

void updateDownbeatCounterNode(QSGSimpleTextureNode*& node,
                               const CueLabel& label,
                               QQuickWindow* window,
                               QSGNode* parent,
                               QSGNode* insertBefore,
                               double originLine,
                               double pixelsPerLine,
                               double devicePixelRatio,
                               qreal top,
                               QString& cachedText,
                               QColor& cachedColor)
{
    if (!window) {
        clearWaveformTexture(node);
        return;
    }

    constexpr qreal badgeHeight = 14.0;
    QFont font;
    font.setBold(true);
    font.setPixelSize(8);
    const QFontMetrics metrics(font);
    const qreal badgeWidth = std::clamp<qreal>(
        static_cast<qreal>(metrics.horizontalAdvance(label.text) + 8), 16.0, 38.0);
    const qreal dpr = std::max(1.0, devicePixelRatio);
    const double x = waveform_render::snappedTimelineX(
        label.linePosition, originLine, pixelsPerLine, devicePixelRatio);
    // Keep the downbeat itself unobstructed: the counter follows immediately
    // to the right of the red marker instead of sitting on top of it.
    const qreal markerGap = 3.0 / dpr;

    // Same reasoning as replaceCueLabelNode: a rebase often leaves every
    // visible bar number unchanged (e.g. a hot-cue edit invalidates
    // geometry without moving the window), so skip the repaint/texture
    // upload when this slot's bar number and colour are still the same.
    if (node && cachedText == label.text && cachedColor == label.color) {
        node->setRect(QRectF(x + markerGap, top, badgeWidth, badgeHeight));
        return;
    }

    QImage image(QSize(static_cast<int>(std::ceil(badgeWidth * dpr)),
                       static_cast<int>(std::ceil(badgeHeight * dpr))),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor accent = label.color.isValid() ? label.color : QColor(242, 62, 72);
    accent.setAlpha(215);
    painter.setBrush(QColor(13, 16, 22, 218));
    painter.setPen(QPen(accent, 1.0));
    painter.drawRoundedRect(QRectF(0.5, 0.5, badgeWidth - 1.0, badgeHeight - 1.0),
                            2.5, 2.5);
    painter.setFont(font);
    painter.setPen(QColor(246, 247, 250, 238));
    painter.drawText(QRectF(1.5, 0.0, badgeWidth - 3.0, badgeHeight),
                     Qt::AlignCenter, label.text);
    painter.end();

    auto* texture = window->createTextureFromImage(image);
    if (!texture) {
        clearWaveformTexture(node);
        return;
    }
    texture->setFiltering(QSGTexture::Linear);
    auto* textureNode = assignTextureNode(node, texture, parent, insertBefore);
    textureNode->setOwnsTexture(true);
    textureNode->setFiltering(QSGTexture::Linear);
    textureNode->setRect(QRectF(x + markerGap, top, badgeWidth, badgeHeight));
    cachedText = label.text;
    cachedColor = label.color;
}

void positionWaveformTile(QSGSimpleTextureNode* node,
                          const waveform_render::RenderTileSpan& span,
                          double renderOriginLine,
                          double pixelsPerLine,
                          double devicePixelRatio,
                          float height)
{
    if (!node)
        return;
    const double dpr = std::max(1.0, devicePixelRatio);
    const auto originPhysical = static_cast<std::int64_t>(std::llround(
        renderOriginLine * pixelsPerLine * dpr));
    const double left = static_cast<double>(span.physicalBegin - originPhysical) / dpr;
    const double logicalWidth = static_cast<double>(span.physicalWidth()) / dpr;
    node->setRect(QRectF(left, 0.0, logicalWidth, height));
}

struct TextureUpload final {
    std::uint64_t renderedColumns = 0;
    std::uint64_t bytes = 0;
    bool replaced = false;
};

std::optional<TextureUpload> uploadWaveformTile(
    QSGSimpleTextureNode*& node,
    const waveform_render::RasterizedRenderTile& tile,
    double renderOriginLine,
    double pixelsPerLine,
    double devicePixelRatio,
    float height,
    QQuickWindow* window,
    QSGNode* parent,
    QSGNode* insertBefore)
{
    if (!window || tile.image.isNull()) {
        clearWaveformTexture(node);
        return std::nullopt;
    }
    const bool replaced = node && node->texture();
    auto* texture = window->createTextureFromImage(tile.image);
    if (!texture) {
        clearWaveformTexture(node);
        return std::nullopt;
    }
    texture->setFiltering(QSGTexture::Nearest);
    auto* textureNode = assignTextureNode(node, texture, parent, insertBefore);
    textureNode->setOwnsTexture(true);
    textureNode->setFiltering(QSGTexture::Nearest);
    positionWaveformTile(textureNode, tile.span, renderOriginLine,
                         pixelsPerLine, devicePixelRatio, height);
    return TextureUpload{
        tile.renderedColumns,
        static_cast<std::uint64_t>(tile.image.sizeInBytes()),
        replaced};
}

void positionFallbackOverview(QSGSimpleTextureNode* node,
                              std::uint32_t sourceBegin,
                              std::uint32_t sourceEnd,
                              double renderOriginLine,
                              double pixelsPerLine,
                              float height)
{
    if (!node)
        return;
    node->setRect(QRectF(
        (static_cast<double>(sourceBegin) - renderOriginLine) * pixelsPerLine,
        0.0,
        static_cast<double>(sourceEnd - sourceBegin) * pixelsPerLine,
        height));
}

std::optional<TextureUpload> uploadFallbackOverview(
    QSGSimpleTextureNode*& node,
    const waveform_render::RasterizedOverview& overview,
    double renderOriginLine,
    double pixelsPerLine,
    float height,
    QQuickWindow* window,
    QSGNode* parent,
    QSGNode* insertBefore)
{
    if (!window || overview.image.isNull()) {
        clearWaveformTexture(node);
        return std::nullopt;
    }
    const bool replaced = node && node->texture();
    auto* texture = window->createTextureFromImage(overview.image);
    if (!texture) {
        clearWaveformTexture(node);
        return std::nullopt;
    }
    texture->setFiltering(QSGTexture::Linear);
    auto* textureNode = assignTextureNode(node, texture, parent, insertBefore);
    textureNode->setOwnsTexture(true);
    textureNode->setFiltering(QSGTexture::Linear);
    positionFallbackOverview(textureNode, overview.key.sourceBegin,
                             overview.key.sourceEnd, renderOriginLine,
                             pixelsPerLine, height);
    return TextureUpload{
        0,
        static_cast<std::uint64_t>(overview.image.sizeInBytes()),
        replaced};
}

std::vector<TrackData::BeatMarker> visibleBeatGrid(const TrackData& trackData,
                                                   double leftSec,
                                                   double rightSec)
{
    const auto grid = trackData.getBeatGrid();
    if (!grid.empty()) {
        const auto first = std::lower_bound(grid.cbegin(), grid.cend(), leftSec,
            [](const TrackData::BeatMarker& marker, double second) {
                return marker.positionSec < second;
            });
        const auto last = std::upper_bound(first, grid.cend(), rightSec,
            [](double second, const TrackData::BeatMarker& marker) {
                return second < marker.positionSec;
            });
        return {first, last};
    }

    std::vector<TrackData::BeatMarker> fallback;
    const double bpm = trackData.getBpm();
    const double sampleRate = trackData.getSampleRate();
    if (bpm <= 0.0 || sampleRate <= 0.0)
        return fallback;

    const double firstBeatSec = static_cast<double>(trackData.getFirstBeatSample()) / sampleRate;
    const double beatPeriod = 60.0 / bpm;
    const int firstIndex = std::max(-10000,
        static_cast<int>(std::floor((leftSec - firstBeatSec) / beatPeriod)));
    const int lastIndex = std::min(10000000,
        static_cast<int>(std::ceil((rightSec - firstBeatSec) / beatPeriod)));
    fallback.reserve(static_cast<std::size_t>(std::max(0, lastIndex - firstIndex + 1)));
    for (int index = firstIndex; index <= lastIndex; ++index) {
        const int beatInBar = ((index % 4) + 4) % 4;
        TrackData::BeatMarker marker;
        marker.positionSec = firstBeatSec + static_cast<double>(index) * beatPeriod;
        marker.isDownbeat = beatInBar == 0;
        marker.beatInBar = beatInBar + 1;
        marker.barIndex = static_cast<int>(std::floor(static_cast<double>(index) / 4.0));
        marker.barNumber = marker.barIndex + 1;
        fallback.push_back(marker);
    }
    return fallback;
}

void updateWorst(std::atomic<std::uint64_t>& target, std::uint64_t value)
{
    auto previous = target.load(std::memory_order_relaxed);
    while (previous < value
           && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

} // namespace

float ScrollingWaveformItem::clampZoom(float pixelsPerPoint)
{
    if (!std::isfinite(pixelsPerPoint))
        return 0.22f;
    return std::clamp(pixelsPerPoint, kMinimumZoom, kMaximumZoom);
}

ScrollingWaveformItem::ScrollingWaveformItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_tileRasterizer = std::make_unique<waveform_render::WaveformTileRasterizer>(
        [this] {
            m_tilesReady.store(true, std::memory_order_release);
            QMetaObject::invokeMethod(this, [this] { update(); },
                                      Qt::QueuedConnection);
        });
    m_dataUpdateThrottle = new QTimer(this);
    m_dataUpdateThrottle->setSingleShot(true);
    // The playhead itself is a VSync transform and stays at full frame rate.
    // Progressive analysis only changes the underlying geometry; rebuilding it
    // at 60 Hz competes with that transform and produces visible frame drops.
    // Progressive textures are informational while analysis is running. 15 Hz
    // is visually continuous for their fill-in, while playback/scratch motion
    // remains a full-rate scene-graph transform and never waits for raster work.
    m_dataUpdateThrottle->setInterval(66);
    connect(m_dataUpdateThrottle, &QTimer::timeout, this, [this]() {
        // A progressive chunk changes only the audio-line nodes. Rebuilding
        // beat/cue geometry here made the one-pixel beatgrid lines blink while
        // the worker was publishing chunks.
        if (!m_pendingDataUpdate)
            return;
        m_pendingDataUpdate = false;
        update();
        // Keep coalescing for as long as chunks keep streaming in.
        m_dataUpdateThrottle->start();
    });
}

ScrollingWaveformItem::~ScrollingWaveformItem()
{
    // Join the raster worker while this QObject is still alive, so no worker
    // can enqueue a scene update against a partially destroyed item.
    m_tileRasterizer.reset();
}

DjEngine* ScrollingWaveformItem::engine() const
{
    return m_engine;
}

void ScrollingWaveformItem::setEngine(DjEngine* engine)
{
    if (m_engine == engine)
        return;
    if (m_engine)
        disconnect(m_engine, nullptr, this, nullptr);

    m_engine = engine;
    if (m_engine) {
        connect(m_engine, &DjEngine::trackLoaded,
                this, &ScrollingWaveformItem::onTrackLoaded, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::trackEjected,
                this, &ScrollingWaveformItem::onTrackEjected, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::loopChanged,
                this, &ScrollingWaveformItem::onLoopUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::hotCuesChanged,
                this, &ScrollingWaveformItem::onOverlayUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::mainCueChanged,
                this, &ScrollingWaveformItem::onOverlayUpdated, Qt::UniqueConnection);
        connect(m_engine, &DjEngine::tempoChanged,
                this, &ScrollingWaveformItem::onTimelineScaleChanged,
                Qt::UniqueConnection);
        connect(m_engine, &DjEngine::playingChanged,
                this, [this] { publishViewportDemand(); });
        connect(m_engine, &DjEngine::scrubbingChanged,
                this, [this] { publishViewportDemand(); });
        connect(m_engine, &DjEngine::reverseChanged,
                this, [this] { publishViewportDemand(); });
        onTrackLoaded();
    }

    emit engineChanged();
    emit effectivePixelsPerSecondChanged();
    invalidateGeometry();
}

void ScrollingWaveformItem::setPixelsPerPoint(float pixelsPerPoint)
{
    pixelsPerPoint = clampZoom(pixelsPerPoint);
    if (qFuzzyCompare(m_pixelsPerPoint, pixelsPerPoint))
        return;
    m_tileRasterizer->cancelPendingTiles();
    m_pixelsPerPoint = pixelsPerPoint;
    emit pixelsPerPointChanged();
    emit effectivePixelsPerSecondChanged();
    publishViewportDemand();
    invalidateGeometry();
}

void ScrollingWaveformItem::setBackgroundColor(const QColor& color)
{
    if (!color.isValid() || m_backgroundColor == color)
        return;
    m_tileRasterizer->cancelPendingTiles();
    m_backgroundColor = color;
    emit backgroundColorChanged();
    invalidateGeometry();
}

double ScrollingWaveformItem::effectivePixelsPerSecond() const noexcept
{
    const auto* currentEngine = m_engine.data();
    return currentEngine
        ? waveform_render::timelinePixelsPerSecond(
              m_pixelsPerPoint, currentEngine->waveformPointsPerSecond(),
              currentEngine->getTempoRatio())
        : 0.0;
}

double ScrollingWaveformItem::screenDeltaToSeconds(double screenDelta) const noexcept
{
    return waveform_render::screenDeltaToTimelineSeconds(
        screenDelta, effectivePixelsPerSecond());
}

double ScrollingWaveformItem::timelineSecondsAtX(
    double screenX, double playheadSeconds) const noexcept
{
    return playheadSeconds + waveform_render::screenDeltaToTimelineSeconds(
        screenX - width() * 0.5, effectivePixelsPerSecond());
}

void ScrollingWaveformItem::requestUpdate()
{
    publishViewportDemand();
    update();
}

void ScrollingWaveformItem::publishViewportDemand()
{
    auto* currentEngine = m_engine.data();
    auto* trackData = currentEngine ? currentEngine->getTrackData() : nullptr;
    const auto snapshot = trackData
        ? trackData->getWaveformLineStoreSnapshot() : nullptr;
    const double pps = effectivePixelsPerSecond();
    if (!currentEngine || !snapshot || snapshot->trackGeneration == 0
        || snapshot->linesPerSecond == 0 || !(pps > 0.0) || width() <= 0.0) {
        m_lastPublishedDemand.reset();
        return;
    }

    const double chunkDuration = static_cast<double>(snapshot->chunkSize)
        / static_cast<double>(snapshot->linesPerSecond);
    const double playhead = std::max(0.0, currentEngine->getVisualPosition());
    const auto demandChunk = chunkDuration > 0.0
        ? static_cast<std::int64_t>(std::floor(playhead / chunkDuration))
        : std::int64_t{0};
    const double physicalPixelsPerLine = pps
        / static_cast<double>(snapshot->linesPerSecond)
        * (window() ? std::max(1.0, window()->effectiveDevicePixelRatio()) : 1.0);
    auto demand = waveform::makeViewportDemand(
        playhead, width(), pps, currentEngine->isPlaying(),
        currentEngine->isReverse(), currentEngine->isScratchVisualActive(),
        WaveformZoomController::lodLevelForPhysicalPixels(
            physicalPixelsPerLine),
        snapshot->trackGeneration);
    if (m_lastPublishedDemand) {
        const auto previousDemandChunk = chunkDuration > 0.0
            ? static_cast<std::int64_t>(std::floor(
                m_lastPublishedDemand->playheadSec / chunkDuration))
            : std::int64_t{0};
        auto configurationOnly = *m_lastPublishedDemand;
        configurationOnly.playheadSec = demand.playheadSec;
        if (previousDemandChunk == demandChunk
            && configurationOnly == demand) {
            return;
        }
    }
    m_lastPublishedDemand = demand;
    currentEngine->updateWaveformDemand(demand);
}

void ScrollingWaveformItem::zoomIn()
{
    setPixelsPerPoint(m_pixelsPerPoint * kZoomFactor);
}

void ScrollingWaveformItem::zoomOut()
{
    setPixelsPerPoint(m_pixelsPerPoint / kZoomFactor);
}

void ScrollingWaveformItem::invalidateGeometry()
{
    m_forceRebuild = true;
    update();
}

void ScrollingWaveformItem::geometryChange(const QRectF& newGeometry,
                                           const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        publishViewportDemand();
        invalidateGeometry();
    }
}

void ScrollingWaveformItem::onTrackLoaded()
{
    if (m_engine && m_engine->getTrackData()) {
        auto* trackData = m_engine->getTrackData();
        connect(trackData, &TrackData::dataUpdated,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::rgbWaveformUpdated,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::peakMipUpdated,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::overviewRgbUpdated,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::dataCleared,
                this, &ScrollingWaveformItem::onDataUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::beatgridChanged,
                this, &ScrollingWaveformItem::onOverlayUpdated, Qt::UniqueConnection);
        connect(trackData, &TrackData::bpmAnalyzed,
                this, &ScrollingWaveformItem::onOverlayUpdated, Qt::UniqueConnection);
    }
    m_lastPublishedDemand.reset();
    publishViewportDemand();
    invalidateGeometry();
}

void ScrollingWaveformItem::onTrackEjected()
{
    if (m_engine && m_engine->getTrackData())
        disconnect(m_engine->getTrackData(), nullptr, this, nullptr);
    m_lastPublishedDemand.reset();
    invalidateGeometry();
}

void ScrollingWaveformItem::onDataUpdated()
{
    // Leading edge: the first result after an idle gap — which is exactly the
    // playhead chunk after a load or seek — paints on the next frame instead
    // of waiting out a throttle interval. Only the follow-up stream of
    // background chunks is coalesced to ~15 Hz, which is what the throttle
    // was actually meant to bound. Previously every publication, including
    // that first one, was delayed by a full interval.
    if (m_dataUpdateThrottle->isActive()) {
        m_pendingDataUpdate = true;
        return;
    }
    m_pendingDataUpdate = false;
    update();
    m_dataUpdateThrottle->start();
}

void ScrollingWaveformItem::onLoopUpdated()
{
    // Loop geometry is maintained independently below updatePaintNode's
    // guarded waveform window. A loop edit must not recreate audio, beatgrid,
    // or cue-label nodes.
    update();
}

void ScrollingWaveformItem::onOverlayUpdated()
{
    invalidateGeometry();
}

void ScrollingWaveformItem::onTimelineScaleChanged()
{
    emit effectivePixelsPerSecondChanged();
    publishViewportDemand();
    invalidateGeometry();
}

QSGNode* ScrollingWaveformItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    auto* scene = static_cast<WaveformSceneNode*>(oldNode);
    if (!scene) {
        scene = new WaveformSceneNode();
        m_sceneGraphNodeCreationCount.fetch_add(
            2 + kWaveformNodePoolSize + kDownbeatLabelNodePoolSize + 5,
            std::memory_order_relaxed);
        m_forceRebuild = true;
    }

    const QRectF bounds = boundingRect();
    if (scene->clipBounds != bounds) {
        scene->setClipRect(bounds);
        scene->clipBounds = bounds;
        scene->markDirty(QSGNode::DirtyGeometry);
    }
    m_frameCount.fetch_add(1, std::memory_order_relaxed);

    DjEngine* engine = m_engine.data();
    TrackData* trackData = engine ? engine->getTrackData() : nullptr;
    const auto snapshot = trackData ? trackData->getWaveformLineStoreSnapshot() : nullptr;
    if (!engine || !trackData || !snapshot || snapshot->totalLineCount == 0
        || !snapshot->chunks || bounds.width() <= 0.0 || bounds.height() <= 0.0) {
        m_tileRasterizer->setActiveTrackGeneration(0);
        if (scene->hasWindow || m_forceRebuild)
            scene->clearAllGeometry();
        scene->hasWindow = false;
        m_forceRebuild = false;
        m_trackGeneration.store(0, std::memory_order_relaxed);
        m_estimatedGpuTextureBytes.store(0, std::memory_order_relaxed);
        return scene;
    }
    m_tileRasterizer->setActiveTrackGeneration(snapshot->trackGeneration);
    m_trackGeneration.store(snapshot->trackGeneration, std::memory_order_relaxed);

    const double pixelsPerSecond = waveform_render::timelinePixelsPerSecond(
        m_pixelsPerPoint, engine->waveformPointsPerSecond(),
        engine->getTempoRatio());
    const double pixelsPerLine = pixelsPerSecond
        / static_cast<double>(snapshot->linesPerSecond);
    const double playheadSec = engine->getVisualPosition();
    const double playheadLine = playheadSec * static_cast<double>(snapshot->linesPerSecond);
    const double dpr = window() ? std::max(1.0, window()->effectiveDevicePixelRatio()) : 1.0;

    m_lastPlayheadSec.store(playheadSec, std::memory_order_relaxed);
    m_lastPixelsPerSecond.store(
        pixelsPerSecond,
        std::memory_order_relaxed);
    m_lastRenderedWidth.store(bounds.width(), std::memory_order_relaxed);

    const bool outsideGuard = !scene->hasWindow
        || playheadLine < scene->innerStartLine
        || playheadLine > scene->innerEndLine;
    const bool staticConfigurationChanged = !scene->hasWindow
        || scene->trackGeneration != snapshot->trackGeneration
        || !qFuzzyCompare(scene->pixelsPerLine, pixelsPerLine)
        || !qFuzzyCompare(scene->devicePixelRatio, dpr)
        || scene->renderedSize != bounds.size();
    const bool configurationChanged = staticConfigurationChanged
        || scene->dataGeneration != snapshot->dataGeneration;
    const bool tilesReady = m_tilesReady.exchange(false, std::memory_order_acq_rel);
    const bool overlayConfigurationChanged = m_forceRebuild
        || staticConfigurationChanged;
    const bool windowConfigurationChanged = outsideGuard
        || staticConfigurationChanged;
    bool renderOriginChanged = false;

    if (m_forceRebuild || outsideGuard || configurationChanged || tilesReady) {
        QElapsedTimer timer;
        timer.start();

        if (windowConfigurationChanged) {
            const double visibleLineCount = bounds.width() / std::max(pixelsPerLine, 1.0e-6);
            const double maximumHalfWindow =
                (static_cast<double>(waveform_render::kRenderTilePhysicalWidth)
                 * static_cast<double>(kWaveformNodePoolSize - 2) * 0.5)
                / std::max(pixelsPerLine * dpr, 1.0e-6);
            const double halfWindow = std::max(visibleLineCount * 0.55,
                std::min(visibleLineCount * 1.25, maximumHalfWindow));
            scene->windowStartLine = static_cast<std::int64_t>(
                std::floor(playheadLine - halfWindow));
            scene->windowEndLine = static_cast<std::int64_t>(
                std::ceil(playheadLine + halfWindow));
            // Keep local scene-graph coordinates small enough for float matrix
            // precision. Choosing an origin on the global physical-pixel grid
            // makes the rebase mathematically invisible to every timeline layer.
            constexpr double maximumPhysicalTranslation = 262'144.0;
            const bool renderOriginNeedsRebase = !scene->hasWindow
                || staticConfigurationChanged
                || std::abs((playheadLine - scene->renderOriginLine)
                            * pixelsPerLine * dpr) > maximumPhysicalTranslation;
            if (renderOriginNeedsRebase) {
                scene->renderOriginLine = waveform_render::pixelAlignedTimelineOrigin(
                    static_cast<double>(scene->windowStartLine), pixelsPerLine, dpr);
                renderOriginChanged = true;
            }
            const double availableGuard = std::max(
                0.0, halfWindow - visibleLineCount * 0.5);
            const double rebuildTravel = std::max(1.0, availableGuard * 0.58);
            scene->innerStartLine = playheadLine - rebuildTravel;
            scene->innerEndLine = playheadLine + rebuildTravel;
        }

        const std::uint32_t sourceBegin = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            scene->windowStartLine, 0, snapshot->totalLineCount));
        const std::uint32_t sourceEnd = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            scene->windowEndLine, 0, snapshot->totalLineCount));
        std::uint64_t renderedLineCount = 0;
        std::uint64_t visibleTileCount = 0;
        std::size_t usedPoolSlots = 0;

        const auto overviewSnapshot = trackData->getOverviewRgbSnapshot();
        constexpr std::uint32_t fallbackWidth = 2048;
        // The whole-track fallback only ever carries fallbackWidth aggregated
        // samples. At normal deck zoom levels stretching it across the full
        // timeline magnifies each sample into a broad smear that looks like
        // real (but entirely fabricated) waveform detail. Suppress it past the
        // honesty budget and let the neutral background stand in until genuine
        // detail tiles arrive.
        const double fallbackMagnification =
            waveform_render::fallbackOverviewMagnification(
                fallbackWidth, snapshot->totalLineCount, pixelsPerLine, dpr);
        const bool fallbackMayRepresentDetail =
            waveform_render::fallbackOverviewMayRepresentDetail(
                fallbackMagnification);
        m_fallbackSuppressedFrameCount.fetch_add(
            fallbackMayRepresentDetail ? 0 : 1, std::memory_order_relaxed);
        if (fallbackMayRepresentDetail && overviewSnapshot
            && !overviewSnapshot->isEmpty()) {
            const auto fallbackHeight = static_cast<std::uint32_t>(std::max(
                1, static_cast<int>(std::ceil(bounds.height() * dpr))));
            const waveform_render::OverviewRenderKey fallbackKey{
                snapshot->trackGeneration,
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                    overviewSnapshot.get())),
                fallbackWidth,
                fallbackHeight,
                0,
                snapshot->totalLineCount,
                snapshot->totalLineCount
            };
            std::shared_ptr<std::vector<waveform_render::OverviewSample>> samples;
            const auto requestOverview = [&](const auto& key) {
                if (!samples) {
                    samples = std::make_shared<std::vector<
                        waveform_render::OverviewSample>>();
                    samples->reserve(static_cast<std::size_t>(
                        overviewSnapshot->size()));
                    for (const auto& frame : *overviewSnapshot) {
                        samples->push_back({frame.rms, frame.low, frame.lowMid,
                                            frame.mid, frame.high});
                    }
                }
                m_tileRasterizer->requestOverview({key, samples});
            };
            const auto firstHighResolutionNode = [&]() -> QSGNode* {
                for (auto* node : scene->waveformNodes) {
                    if (node)
                        return node;
                }
                return scene->loopFill;
            };
            if (const auto ready = m_tileRasterizer->findOverview(fallbackKey)) {
                if (!scene->fallbackKey || *scene->fallbackKey != fallbackKey) {
                    const auto upload = uploadFallbackOverview(
                        scene->fallbackNode, *ready,
                        scene->renderOriginLine, pixelsPerLine,
                        static_cast<float>(bounds.height()), window(),
                        scene->timeline, firstHighResolutionNode());
                    if (upload) {
                        scene->fallbackKey = fallbackKey;
                        scene->fallbackVisible = true;
                        scene->fallbackTextureBytes = upload->bytes;
                        m_textureUploadCount.fetch_add(1, std::memory_order_relaxed);
                        m_textureUploadBytes.fetch_add(upload->bytes,
                                                       std::memory_order_relaxed);
                        if (upload->replaced) {
                            m_textureReplacementCount.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    } else {
                        scene->fallbackKey.reset();
                        scene->fallbackVisible = false;
                        scene->fallbackTextureBytes = 0;
                    }
                } else if (renderOriginChanged || !scene->fallbackVisible) {
                    // Also covers coming back from a suppressed zoom level:
                    // the node still owns its texture but was hidden, so it
                    // needs its rect restored for the current origin.
                    positionFallbackOverview(
                        scene->fallbackNode, 0, snapshot->totalLineCount,
                        scene->renderOriginLine, pixelsPerLine,
                        static_cast<float>(bounds.height()));
                    scene->fallbackVisible = true;
                }
            } else if (!scene->fallbackRequestedKey
                       || *scene->fallbackRequestedKey != fallbackKey) {
                requestOverview(fallbackKey);
                scene->fallbackRequestedKey = fallbackKey;
            }

        } else if (scene->fallbackVisible) {
            clearWaveformTexture(scene->fallbackNode);
            scene->fallbackVisible = false;
        }
        if (scene->fallbackKey
            && scene->fallbackKey->trackGeneration != snapshot->trackGeneration) {
            clearWaveformTexture(scene->fallbackNode);
            scene->fallbackTextureBytes = 0;
            scene->fallbackKey.reset();
            scene->fallbackRequestedKey.reset();
        }
        std::vector<PreparedTileSlot> preparedTiles;
        preparedTiles.reserve(scene->waveformNodes.size());
        if (sourceBegin < sourceEnd) {
            const auto physicalBegin = waveform_render::timelinePhysicalFloor(
                static_cast<double>(sourceBegin), pixelsPerLine, dpr);
            const auto physicalEnd = waveform_render::timelinePhysicalCeil(
                static_cast<double>(sourceEnd), pixelsPerLine, dpr);
            const auto firstTile = waveform_render::firstRenderTile(physicalBegin);
            const auto lastTile = waveform_render::lastRenderTile(physicalEnd);
            for (auto tileIndex = firstTile;
                 tileIndex <= lastTile && usedPoolSlots < scene->waveformNodes.size();
                 ++tileIndex) {
                const std::size_t poolIndex = usedPoolSlots++;
                const auto tileSpan = waveform_render::renderTileSpan(
                    tileIndex, pixelsPerLine * dpr, snapshot->totalLineCount);
                if (!tileSpan.hasSource()) {
                    clearWaveformTexture(scene->waveformNodes[poolIndex]);
                    scene->waveformTileKeys[poolIndex].reset();
                    scene->waveformRenderedLineCounts[poolIndex] = 0;
                    scene->waveformTextureBytes[poolIndex] = 0;
                    continue;
                }
                const int imageHeight = std::max(
                    1, static_cast<int>(std::ceil(bounds.height() * dpr)));
                const auto lodLevel = WaveformZoomController::lodLevelForPhysicalPixels(
                    pixelsPerLine * dpr);
                const auto key = waveform_render::WaveformTileRasterizer::makeKey(
                    *snapshot, tileIndex, tileSpan, pixelsPerLine * dpr,
                    imageHeight, dpr, lodLevel,
                    static_cast<std::uint32_t>(m_backgroundColor.rgba()));
                const auto requiredViewKey = waveform_render::viewKeyFor(key);
                if (!scene->viewKey || *scene->viewKey != requiredViewKey) {
                    // A zoom/DPR/background change invalidates every pool
                    // slot's key, but the textures themselves still depict
                    // valid audio at their remembered source-line range.
                    // Leave them in place — the per-tile loop below keeps
                    // displaying each one, repositioned for the new zoom,
                    // until its replacement for the new view is ready. This
                    // avoids a hard cut to the coarse whole-track fallback on
                    // every zoom step.
                    const bool zoomTransition = scene->viewKey
                        && scene->viewKey->trackGeneration
                            == requiredViewKey.trackGeneration;
                    scene->viewKey = requiredViewKey;
                    ++scene->viewGeneration;
                    m_viewGeneration.store(scene->viewGeneration,
                                           std::memory_order_relaxed);
                    if (zoomTransition)
                        m_zoomTransitionCount.fetch_add(
                            1, std::memory_order_relaxed);
                }
                PreparedTileSlot prepared{poolIndex, tileSpan, key};
                prepared.alreadyDisplayed = waveform_render::currentRenderTileKey(
                    scene->waveformTileKeys[poolIndex], key);
                if (!prepared.alreadyDisplayed)
                    prepared.ready = m_tileRasterizer->find(key);
                if (!prepared.alreadyDisplayed && !prepared.ready) {
                    const double tileCentre = static_cast<double>(
                        tileSpan.physicalBegin + tileSpan.physicalEnd) * 0.5;
                    const double playheadPhysical = playheadLine * pixelsPerLine * dpr;
                    m_tileRasterizer->request({
                        key,
                        tileSpan,
                        snapshot,
                        pixelsPerLine * dpr,
                        bounds.height(),
                        dpr,
                        std::abs(tileCentre - playheadPhysical)
                    });
                }
                preparedTiles.push_back(std::move(prepared));
            }
        }

        std::uint64_t readyVisibleTiles = 0;
        std::uint64_t missingVisibleTiles = 0;

        // Publish each current-generation tile as soon as it is ready. A slot
        // whose replacement isn't ready yet becomes a zero-area node, which
        // exposes the coarse whole-track fallback beneath it.
        for (const auto& prepared : preparedTiles) {
            const auto poolIndex = prepared.poolIndex;
            if (prepared.alreadyDisplayed) {
                ++readyVisibleTiles;
                if (renderOriginChanged) {
                    positionWaveformTile(
                        scene->waveformNodes[poolIndex], prepared.span,
                        scene->renderOriginLine, pixelsPerLine, dpr,
                        static_cast<float>(bounds.height()));
                }
            } else if (prepared.ready
                       && waveform_render::completeDetailMayCoverOverview(
                           prepared.ready->key == prepared.key,
                           prepared.ready->hasAnySourceData,
                           prepared.ready->hasCompleteSourceData)) {
                const auto upload = uploadWaveformTile(
                    scene->waveformNodes[poolIndex], *prepared.ready,
                    scene->renderOriginLine, pixelsPerLine, dpr,
                    static_cast<float>(bounds.height()), window(),
                    scene->timeline, scene->loopFill);
                if (upload) {
                    scene->waveformRenderedLineCounts[poolIndex]
                        = upload->renderedColumns;
                    scene->waveformTextureBytes[poolIndex] = upload->bytes;
                    scene->waveformTileKeys[poolIndex] = prepared.key;
                    ++readyVisibleTiles;
                    m_textureUploadCount.fetch_add(1, std::memory_order_relaxed);
                    m_textureUploadBytes.fetch_add(upload->bytes,
                                                   std::memory_order_relaxed);
                    if (upload->replaced) {
                        m_textureReplacementCount.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                } else {
                    scene->waveformTileKeys[poolIndex].reset();
                    scene->waveformRenderedLineCounts[poolIndex] = 0;
                    scene->waveformTextureBytes[poolIndex] = 0;
                    ++missingVisibleTiles;
                }
            } else {
                // The replacement tile isn't rasterised yet (zoom change,
                // live analysis still settling, or freshly scrolled-into
                // territory). Earlier this held the previous tile on screen,
                // repositioned to the new scale, instead of clearing it —
                // but independently-stale tiles at independently-stretched
                // scales produced a patchwork of mismatched blocky
                // rectangles across the pool, which reads far worse than a
                // brief, uniform flash to the coarse whole-track fallback.
                // Clearing here lets that single coherent fallback texture
                // cover the whole gap instead.
                if (scene->waveformTileKeys[poolIndex]) {
                    m_staleZoomTilesRejected.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (prepared.ready && prepared.ready->hasAnySourceData
                    && !prepared.ready->hasCompleteSourceData) {
                    m_incompleteTileRejectedCount.fetch_add(
                        1, std::memory_order_relaxed);
                }
                clearWaveformTexture(scene->waveformNodes[poolIndex]);
                scene->waveformTileKeys[poolIndex].reset();
                scene->waveformRenderedLineCounts[poolIndex] = 0;
                scene->waveformTextureBytes[poolIndex] = 0;
                ++missingVisibleTiles;
            }
            renderedLineCount += scene->waveformRenderedLineCounts[poolIndex];
            ++visibleTileCount;
        }
        m_readyVisibleTileCount.store(readyVisibleTiles,
                                      std::memory_order_relaxed);
        m_missingVisibleTileCount.store(missingVisibleTiles,
                                        std::memory_order_relaxed);
        m_detailCoveragePermille.store(
            visibleTileCount == 0 ? 0
                : (readyVisibleTiles * 1000) / visibleTileCount,
            std::memory_order_relaxed);
        if (missingVisibleTiles > 0 && scene->fallbackVisible)
            m_overviewFallbackFrameCount.fetch_add(
                1, std::memory_order_relaxed);
        for (std::size_t poolIndex = usedPoolSlots;
            poolIndex < scene->waveformNodes.size(); ++poolIndex) {
            clearWaveformTexture(scene->waveformNodes[poolIndex]);
            scene->waveformTileKeys[poolIndex].reset();
            scene->waveformRenderedLineCounts[poolIndex] = 0;
            scene->waveformTextureBytes[poolIndex] = 0;
        }
        const auto gpuTextureBytes = std::accumulate(
            scene->waveformTextureBytes.cbegin(),
            scene->waveformTextureBytes.cend(),
            scene->fallbackTextureBytes);
        m_estimatedGpuTextureBytes.store(gpuTextureBytes,
                                         std::memory_order_relaxed);

        // Every timeline layer uses the same persistent origin. Replacing an
        // off-screen waveform chunk therefore cannot change the pixel phase of
        // either the audio columns or an overlay at a guarded window boundary.
        if (m_forceRebuild || windowConfigurationChanged || renderOriginChanged) {
            // Keep QSG float coordinates local. Building the beat geometry for
            // an entire hour-long track loses pixel precision at high zoom and
            // manifests as missing or warped grid lines on Vulkan.
            const double gridStartSec = static_cast<double>(scene->windowStartLine)
                / snapshot->linesPerSecond;
            const double gridEndSec = static_cast<double>(scene->windowEndLine)
                / snapshot->linesPerSecond;
            const auto beats = visibleBeatGrid(*trackData, gridStartSec, gridEndSec);
            std::vector<MarkerLine> regularBeats;
            std::vector<MarkerLine> downbeats;
            regularBeats.reserve(beats.size() * 2);
            downbeats.reserve((beats.size() / 4 + 1) * 2);
            const float height = static_cast<float>(bounds.height());
            const auto verticalLayout = waveform_render::verticalMarkerLayout(height);
            const float edgeInset = static_cast<float>(1.0 / dpr);
            for (const auto& beat : beats) {
                const bool isDownbeat = beat.isDownbeat || beat.beatInBar == 1;
                const float tickLength = isDownbeat
                    ? verticalLayout.downbeatTickLength
                    : verticalLayout.regularTickLength;
                const QColor color = isDownbeat
                    ? QColor(242, 62, 72, 220)
                    : QColor(235, 240, 245, 112);
                auto& markers = isDownbeat ? downbeats : regularBeats;
                const double linePosition = beat.positionSec * snapshot->linesPerSecond;
                markers.push_back({linePosition, color,
                                   edgeInset, edgeInset + tickLength});
                markers.push_back({linePosition, color,
                                   height - edgeInset - tickLength, height - edgeInset});
            }
            writeMarkerGeometry(scene->regularBeats, regularBeats,
                                scene->renderOriginLine, pixelsPerLine, dpr);
            writeMarkerGeometry(scene->downbeats, downbeats,
                                scene->renderOriginLine, pixelsPerLine, dpr);

            std::vector<MarkerLine> cueLines;
            std::vector<CueLabel> cueLabels;
            const QVariantList cues = engine->hotCues();
            cueLines.reserve(static_cast<std::size_t>(cues.size() + 1));
            cueLabels.reserve(static_cast<std::size_t>(cues.size() + 1));
            for (const QVariant& cueValue : cues) {
                const QVariantMap cue = cueValue.toMap();
                if (!cue.value(QStringLiteral("set")).toBool())
                    continue;
                QColor color(cue.value(QStringLiteral("color")).toString());
                if (!color.isValid())
                    color = QColor(235, 65, 75);
                color.setAlpha(215);
                const double linePosition = cue.value(QStringLiteral("positionSec")).toDouble()
                    * snapshot->linesPerSecond;
                cueLines.push_back({
                    linePosition, color, 0.0f, static_cast<float>(bounds.height()),
                    verticalLayout.cueLinePhysicalWidth});
                QString label = cue.value(QStringLiteral("label")).toString().trimmed();
                if (label.isEmpty())
                    label = QStringLiteral("HOT CUE %1")
                        .arg(cue.value(QStringLiteral("index")).toInt() + 1);
                cueLabels.push_back({linePosition, color, label});
            }
            if (engine->mainCueSec() >= 0.0) {
                const double linePosition = engine->mainCueSec() * snapshot->linesPerSecond;
                const QColor color(255, 70, 78, 210);
                cueLines.push_back({linePosition, color, 0.0f,
                                    static_cast<float>(bounds.height()),
                                    verticalLayout.cueLinePhysicalWidth});
                cueLabels.push_back({linePosition, color, QStringLiteral("CUE")});
            }
            writeMarkerGeometry(scene->cueLines, cueLines,
                                scene->renderOriginLine, pixelsPerLine, dpr);
            std::size_t labelIndex = 0;
            for (; labelIndex < cueLabels.size()
                   && labelIndex < scene->cueLabels.size(); ++labelIndex) {
                scene->cueLabels[labelIndex] = replaceCueLabelNode(
                    scene->cueLabels[labelIndex], scene->timeline,
                    cueLabels[labelIndex], window(), scene->renderOriginLine,
                    pixelsPerLine, dpr, scene->cueLabelTexts[labelIndex],
                    scene->cueLabelColors[labelIndex]);
                m_sceneGraphNodeCreationCount.fetch_add(1, std::memory_order_relaxed);
            }
            for (; labelIndex < scene->cueLabels.size(); ++labelIndex) {
                scene->cueLabelTexts[labelIndex].clear();
                scene->cueLabelColors[labelIndex] = QColor();
                if (!scene->cueLabels[labelIndex])
                    continue;
                scene->timeline->removeChildNode(scene->cueLabels[labelIndex]);
                delete scene->cueLabels[labelIndex];
                scene->cueLabels[labelIndex] = nullptr;
            }
        }

        if (m_forceRebuild || windowConfigurationChanged || renderOriginChanged) {
            const double labelsStartSec = static_cast<double>(scene->windowStartLine)
                / snapshot->linesPerSecond;
            const double labelsEndSec = static_cast<double>(scene->windowEndLine)
                / snapshot->linesPerSecond;
            const auto visibleBeats = visibleBeatGrid(
                *trackData, labelsStartSec, labelsEndSec);
            std::vector<CueLabel> labels;
            labels.reserve(visibleBeats.size() / 4 + 1);
            for (const auto& beat : visibleBeats) {
                const bool isDownbeat = beat.isDownbeat || beat.beatInBar == 1;
                if (!isDownbeat || beat.barNumber <= 0)
                    continue;
                labels.push_back({
                    beat.positionSec * snapshot->linesPerSecond,
                    QColor(242, 62, 72, 220),
                    QString::number(beat.barNumber)});
            }

            const auto markerLayout = waveform_render::verticalMarkerLayout(
                static_cast<float>(bounds.height()));
            const qreal labelTop = 1.0 / dpr + markerLayout.downbeatTickLength + 2.0;
            std::size_t labelIndex = 0;
            for (; labelIndex < labels.size()
                   && labelIndex < scene->downbeatLabels.size(); ++labelIndex) {
                updateDownbeatCounterNode(
                    scene->downbeatLabels[labelIndex], labels[labelIndex], window(),
                    scene->timeline, scene->loopEdges, scene->renderOriginLine,
                    pixelsPerLine, dpr, labelTop,
                    scene->downbeatLabelTexts[labelIndex],
                    scene->downbeatLabelColors[labelIndex]);
            }
            for (; labelIndex < scene->downbeatLabels.size(); ++labelIndex)
                clearWaveformTexture(scene->downbeatLabels[labelIndex]);
        }

        scene->trackGeneration = snapshot->trackGeneration;
        scene->dataGeneration = snapshot->dataGeneration;
        scene->pixelsPerLine = pixelsPerLine;
        scene->devicePixelRatio = dpr;
        scene->renderedSize = bounds.size();
        scene->hasWindow = true;
        m_forceRebuild = false;

        m_geometryRebuildCount.fetch_add(1, std::memory_order_relaxed);
        m_lastRenderedLineCount.store(renderedLineCount, std::memory_order_relaxed);
        m_lastVisibleChunkCount.store(visibleTileCount, std::memory_order_relaxed);
        updateWorst(m_worstGeometryBuildUsec,
                    static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000));
    }

    const bool loopComplete = engine->loopOutPosition() > engine->loopInPosition();
    const double visualLoopOut = loopComplete
        ? engine->loopOutPosition()
        : engine->loopPreviewOutPosition();
    const bool loopVisible = engine->loopInSet()
        && visualLoopOut > engine->loopInPosition();
    const int loopAppearance = engine->loopActive() && loopComplete
        ? 2
        : (loopComplete ? 0 : 1);
    const double loopInLine = engine->loopInPosition() * snapshot->linesPerSecond;
    const double loopOutLine = visualLoopOut * snapshot->linesPerSecond;
    const bool loopGeometryChanged = overlayConfigurationChanged
        || renderOriginChanged
        || scene->loopVisible != loopVisible
        || scene->loopAppearance != loopAppearance
        || !qFuzzyCompare(scene->loopInLine, loopInLine)
        || !qFuzzyCompare(scene->loopOutLine, loopOutLine);

    if (loopGeometryChanged) {
        auto* loopGeometry = scene->loopFill->geometry();
        std::array<MarkerLine, 2> loopEdges {};
        std::size_t loopEdgeCount = 0;
        if (loopVisible) {
            const int fillAlpha = loopAppearance == 2 ? 22 : (loopAppearance == 1 ? 14 : 9);
            const int edgeAlpha = loopAppearance == 2 ? 190 : (loopAppearance == 1 ? 150 : 95);
            const float x0 = static_cast<float>(waveform_render::snappedTimelineX(
                loopInLine, scene->renderOriginLine, pixelsPerLine, dpr));
            const float x1 = static_cast<float>(waveform_render::snappedTimelineX(
                loopOutLine, scene->renderOriginLine, pixelsPerLine, dpr));
            loopGeometry->allocate(4);
            auto* vertices = loopGeometry->vertexDataAsColoredPoint2D();
            const float h = static_cast<float>(bounds.height());
            setPremultipliedVertex(vertices[0], x0, 0.0f, 70, 190, 255, fillAlpha);
            setPremultipliedVertex(vertices[1], x0, h, 70, 190, 255, fillAlpha);
            setPremultipliedVertex(vertices[2], x1, 0.0f, 70, 190, 255, fillAlpha);
            setPremultipliedVertex(vertices[3], x1, h, 70, 190, 255, fillAlpha);
            loopEdges[loopEdgeCount++] = {
                loopInLine, QColor(70, 190, 255, edgeAlpha), 0.0f, h};
            loopEdges[loopEdgeCount++] = {
                loopOutLine, QColor(70, 190, 255, edgeAlpha), 0.0f, h};
        } else {
            loopGeometry->allocate(0);
        }
        scene->loopFill->markDirty(QSGNode::DirtyGeometry);
        writeMarkerGeometry(scene->loopEdges, loopEdges.data(), loopEdgeCount,
                            scene->renderOriginLine, pixelsPerLine, dpr);
        scene->loopVisible = loopVisible;
        scene->loopInLine = loopInLine;
        scene->loopOutLine = loopOutLine;
        scene->loopAppearance = loopAppearance;
    }

    // The visual clock is continuous. Keeping its translation continuous as
    // well avoids the 1-pixel stop/start cadence that made dense waveforms and
    // beat ticks shimmer during playback. Feathered line edges provide stable
    // coverage while the shared timeline moves between physical pixels.
    const double timelineTranslation = waveform_render::smoothTimelineTranslation(
        bounds.width(), playheadLine, scene->renderOriginLine, pixelsPerLine);
    QMatrix4x4 transform;
    transform.translate(static_cast<float>(timelineTranslation), 0.0f);
    scene->timeline->setMatrix(transform);
    scene->timeline->markDirty(QSGNode::DirtyMatrix);
    m_transformUpdateCount.fetch_add(1, std::memory_order_relaxed);
    return scene;
}

QVariantList ScrollingWaveformItem::beatLabels() const
{
    QVariantList result;
    if (!m_engine || !m_engine->getTrackData())
        return result;

    const double width = m_lastRenderedWidth.load(std::memory_order_relaxed);
    const double pixelsPerSecond = m_lastPixelsPerSecond.load(std::memory_order_relaxed);
    const double playheadSec = m_lastPlayheadSec.load(std::memory_order_relaxed);
    if (width <= 0.0 || pixelsPerSecond <= 0.0)
        return result;

    const double halfVisibleSec = width * 0.5 / pixelsPerSecond;
    const auto beats = visibleBeatGrid(*m_engine->getTrackData(),
                                       playheadSec - halfVisibleSec,
                                       playheadSec + halfVisibleSec);
    double previousX = -std::numeric_limits<double>::infinity();
    for (const auto& beat : beats) {
        const double x = width * 0.5 + (beat.positionSec - playheadSec) * pixelsPerSecond;
        if (x - previousX < 20.0)
            continue;
        previousX = x;
        QVariantMap marker;
        marker.insert(QStringLiteral("x"), x);
        marker.insert(QStringLiteral("text"),
                      beat.isDownbeat ? QString::number(beat.barNumber)
                                      : QString::number(beat.beatInBar));
        marker.insert(QStringLiteral("isDownbeat"), beat.isDownbeat);
        result.push_back(marker);
    }
    return result;
}

QVariantMap ScrollingWaveformItem::renderStats() const
{
    QVariantMap stats;
    const auto tileStats = m_tileRasterizer->stats();
    stats.insert(QStringLiteral("frames"),
                 QVariant::fromValue<qulonglong>(m_frameCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("frameCount"),
                 QVariant::fromValue<qulonglong>(m_frameCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("geometryRebuilds"),
                 QVariant::fromValue<qulonglong>(m_geometryRebuildCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("geometryRebuildCount"),
                 QVariant::fromValue<qulonglong>(m_geometryRebuildCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("transformUpdates"),
                 QVariant::fromValue<qulonglong>(m_transformUpdateCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("transformUpdateCount"),
                 QVariant::fromValue<qulonglong>(m_transformUpdateCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("sceneGraphNodeCreations"),
                 QVariant::fromValue<qulonglong>(m_sceneGraphNodeCreationCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("renderedLines"),
                 QVariant::fromValue<qulonglong>(m_lastRenderedLineCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("visibleChunks"),
                 QVariant::fromValue<qulonglong>(m_lastVisibleChunkCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("visibleTiles"),
                 QVariant::fromValue<qulonglong>(m_lastVisibleChunkCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("readyVisibleTiles"),
                 QVariant::fromValue<qulonglong>(m_readyVisibleTileCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("missingVisibleTiles"),
                 QVariant::fromValue<qulonglong>(m_missingVisibleTileCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("detailCoveragePercent"),
                 static_cast<double>(m_detailCoveragePermille.load(std::memory_order_relaxed)) / 10.0);
    stats.insert(QStringLiteral("overviewFallbackFrames"),
                 QVariant::fromValue<qulonglong>(m_overviewFallbackFrameCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("fallbackSuppressedFrames"),
                 QVariant::fromValue<qulonglong>(m_fallbackSuppressedFrameCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("viewGeneration"),
                 QVariant::fromValue<qulonglong>(m_viewGeneration.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("trackGeneration"),
                 QVariant::fromValue<qulonglong>(m_trackGeneration.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("zoomTransitions"),
                 QVariant::fromValue<qulonglong>(m_zoomTransitionCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("staleZoomTilesRejected"),
                 QVariant::fromValue<qulonglong>(m_staleZoomTilesRejected.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("textureUploads"),
                 QVariant::fromValue<qulonglong>(m_textureUploadCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("textureReplacements"),
                 QVariant::fromValue<qulonglong>(m_textureReplacementCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("textureUploadBytes"),
                 QVariant::fromValue<qulonglong>(m_textureUploadBytes.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("tilePoolCapacity"),
                 static_cast<qulonglong>(kWaveformNodePoolSize));
    stats.insert(QStringLiteral("tilePhysicalWidth"),
                 static_cast<qlonglong>(waveform_render::kRenderTilePhysicalWidth));
    stats.insert(QStringLiteral("tileCacheHits"),
                 QVariant::fromValue<qulonglong>(tileStats.cacheHits));
    stats.insert(QStringLiteral("tileCacheMisses"),
                 QVariant::fromValue<qulonglong>(tileStats.cacheMisses));
    stats.insert(QStringLiteral("tileCacheBytes"),
                 QVariant::fromValue<qulonglong>(tileStats.cacheBytes));
    stats.insert(QStringLiteral("tileCacheBudgetBytes"),
                 QVariant::fromValue<qulonglong>(
                     waveform_render::WaveformTileRasterizer::kMaximumCacheBytes));
    stats.insert(QStringLiteral("estimatedGpuTextureBytes"),
                 QVariant::fromValue<qulonglong>(
                     m_estimatedGpuTextureBytes.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("tileCacheEntries"),
                 QVariant::fromValue<qulonglong>(tileStats.cacheEntries));
    stats.insert(QStringLiteral("pendingTileRequests"),
                 QVariant::fromValue<qulonglong>(tileStats.pendingRequests));
    stats.insert(QStringLiteral("rasterizedTiles"),
                 QVariant::fromValue<qulonglong>(tileStats.rasterizedTiles));
    stats.insert(QStringLiteral("discardedStaleTiles"),
                 QVariant::fromValue<qulonglong>(tileStats.discardedTiles));
    stats.insert(QStringLiteral("incompleteTileRejectedCount"),
                 QVariant::fromValue<qulonglong>(
                     m_incompleteTileRejectedCount.load(std::memory_order_relaxed)));
    stats.insert(QStringLiteral("worstTileRasterUsec"),
                 QVariant::fromValue<qulonglong>(tileStats.worstRasterUsec));
    stats.insert(QStringLiteral("worstRasterUsec"),
                 QVariant::fromValue<qulonglong>(tileStats.worstRasterUsec));
    stats.insert(QStringLiteral("worstGeometryBuildUsec"),
                 QVariant::fromValue<qulonglong>(m_worstGeometryBuildUsec.load(std::memory_order_relaxed)));
    return stats;
}

void ScrollingWaveformItem::resetRenderStats()
{
    m_tileRasterizer->resetStats();
    m_frameCount.store(0, std::memory_order_relaxed);
    m_geometryRebuildCount.store(0, std::memory_order_relaxed);
    m_transformUpdateCount.store(0, std::memory_order_relaxed);
    m_sceneGraphNodeCreationCount.store(0, std::memory_order_relaxed);
    m_lastRenderedLineCount.store(0, std::memory_order_relaxed);
    m_lastVisibleChunkCount.store(0, std::memory_order_relaxed);
    m_readyVisibleTileCount.store(0, std::memory_order_relaxed);
    m_missingVisibleTileCount.store(0, std::memory_order_relaxed);
    m_detailCoveragePermille.store(0, std::memory_order_relaxed);
    m_overviewFallbackFrameCount.store(0, std::memory_order_relaxed);
    m_fallbackSuppressedFrameCount.store(0, std::memory_order_relaxed);
    m_viewGeneration.store(0, std::memory_order_relaxed);
    m_trackGeneration.store(0, std::memory_order_relaxed);
    m_zoomTransitionCount.store(0, std::memory_order_relaxed);
    m_staleZoomTilesRejected.store(0, std::memory_order_relaxed);
    m_textureUploadCount.store(0, std::memory_order_relaxed);
    m_textureReplacementCount.store(0, std::memory_order_relaxed);
    m_textureUploadBytes.store(0, std::memory_order_relaxed);
    m_estimatedGpuTextureBytes.store(0, std::memory_order_relaxed);
    m_incompleteTileRejectedCount.store(0, std::memory_order_relaxed);
    m_worstGeometryBuildUsec.store(0, std::memory_order_relaxed);
}
