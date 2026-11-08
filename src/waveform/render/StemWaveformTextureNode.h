#pragma once

#include "stems/StemWaveformOverlay.h"
#include "waveform/render/WaveformRenderMath.h"

#include <QColor>
#include <QSGSimpleTextureNode>

class QSGGeometryNode;

namespace waveform_render {

// The original texture never changes when gains change. Per-column stem energy
// is prepared once; the GPU contracts the coloured waveform over a grey ghost.
class StemWaveformTextureNode final : public QSGNode {
public:
    explicit StemWaveformTextureNode(bool accelerated);
    ~StemWaveformTextureNode() override;
    [[nodiscard]] QSGTexture* texture() const noexcept { return m_texture; }
    void setTexture(QSGTexture* texture);
    void setOwnsTexture(bool owns) noexcept { m_ownsTexture = owns; }
    void setFiltering(QSGTexture::Filtering filtering);
    void setSourceRect(const QRectF& rect);
    void setRect(const QRectF& rect);
    void configure(const RenderTileSpan& span, double rasterScale, double linesPerSecond);
    void setStems(const std::shared_ptr<const brockdj::stems::StemWaveformOverlay>& overlay,
                  const QColor& background);
    [[nodiscard]] bool accelerated() const noexcept { return m_shaderNode != nullptr; }

private:
    void rebuildGeometry();

    QSGGeometryNode* m_shaderNode = nullptr;
    QSGSimpleTextureNode* m_plainNode = nullptr;
    QSGTexture* m_texture = nullptr;
    bool m_ownsTexture = true;
    QRectF m_sourceRect;
    QRectF m_rect;
    RenderTileSpan m_span;
    double m_rasterScale = 1.0;
    double m_linesPerSecond = 1200.0;
    std::shared_ptr<const brockdj::stems::StemTrackData> m_data;
    std::uint64_t m_epoch = 0;
    std::vector<std::array<float, brockdj::stems::kStemCount>> m_energy;
};

} // namespace waveform_render
