#include "StemWaveformTextureNode.h"

#include <QSGGeometryNode>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QVector4D>

#include <cstring>
#include <numeric>

namespace waveform_render {
namespace {

class StemMaterial final : public QSGMaterial {
public:
    StemMaterial()
    {
        setFlag(Blending, true);
        // Position baking must not reinterpret our extra per-column attributes.
        setFlag(RequiresFullMatrix, true);
    }
    QSGMaterialType* type() const override
    {
        static QSGMaterialType type;
        return &type;
    }
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override
    {
        const auto* material = static_cast<const StemMaterial*>(other);
        const auto a = texture->comparisonKey();
        const auto b = material->texture->comparisonKey();
        if (a != b)
            return a < b ? -1 : 1;
        for (int i = 0; i < 4; ++i) {
            if (gains[i] != material->gains[i])
                return gains[i] < material->gains[i] ? -1 : 1;
            if (background[i] != material->background[i])
                return background[i] < material->background[i] ? -1 : 1;
        }
        return 0;
    }

    QSGTexture* texture = nullptr;
    QVector4D gains {1, 1, 1, 1};
    QVector4D background;
};

class StemShader final : public QSGMaterialShader {
public:
    StemShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/stemwaveform.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/stemwaveform.frag.qsb"));
    }
    bool updateUniformData(RenderState& state, QSGMaterial* next, QSGMaterial*) override
    {
        auto* bytes = state.uniformData();
        const auto* material = static_cast<StemMaterial*>(next);
        if (state.isMatrixDirty()) {
            const auto matrix = state.combinedMatrix();
            std::memcpy(bytes->data(), matrix.constData(), 64);
        }
        if (state.isOpacityDirty()) {
            const float opacity = state.opacity();
            std::memcpy(bytes->data() + 64, &opacity, 4);
        }
        std::memcpy(bytes->data() + 80, &material->gains, 16);
        std::memcpy(bytes->data() + 96, &material->background, 16);
        return true;
    }
    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture,
                            QSGMaterial* next, QSGMaterial*) override
    {
        if (binding == 1) {
            *texture = static_cast<StemMaterial*>(next)->texture;
            (*texture)->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        }
    }
};

QSGMaterialShader* StemMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new StemShader;
}

struct Vertex {
    float x, y, u, v;
    std::array<float, brockdj::stems::kStemCount> energy;
};

const QSGGeometry::AttributeSet& attributes()
{
    static const QSGGeometry::Attribute values[] {
        QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),
        QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType),
        QSGGeometry::Attribute::create(2, 4, QSGGeometry::FloatType)
    };
    static const QSGGeometry::AttributeSet set {3, sizeof(Vertex), values};
    return set;
}

} // namespace

StemWaveformTextureNode::StemWaveformTextureNode(bool accelerated)
{
    if (accelerated) {
        m_shaderNode = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(attributes(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        m_shaderNode->setGeometry(geometry);
        m_shaderNode->setFlag(QSGNode::OwnsGeometry);
        m_shaderNode->setMaterial(new StemMaterial);
        m_shaderNode->setFlag(QSGNode::OwnsMaterial);
        appendChildNode(m_shaderNode);
    } else {
        m_plainNode = new QSGSimpleTextureNode;
        m_plainNode->setOwnsTexture(false);
        m_plainNode->setFiltering(QSGTexture::Linear);
        appendChildNode(m_plainNode);
    }
}

StemWaveformTextureNode::~StemWaveformTextureNode()
{
    if (m_ownsTexture)
        delete m_texture;
}

void StemWaveformTextureNode::setTexture(QSGTexture* texture)
{
    m_texture = texture;
    if (m_plainNode)
        m_plainNode->setTexture(texture);
    if (m_shaderNode) {
        static_cast<StemMaterial*>(m_shaderNode->material())->texture = texture;
        m_shaderNode->markDirty(QSGNode::DirtyMaterial);
    }
}

void StemWaveformTextureNode::setFiltering(QSGTexture::Filtering filtering)
{
    if (m_texture)
        m_texture->setFiltering(filtering);
    if (m_plainNode)
        m_plainNode->setFiltering(filtering);
}

void StemWaveformTextureNode::setSourceRect(const QRectF& rect)
{
    m_sourceRect = rect;
    if (m_plainNode)
        m_plainNode->setSourceRect(rect);
}

void StemWaveformTextureNode::setRect(const QRectF& rect)
{
    if (!accelerated()) {
        m_plainNode->setRect(rect);
        return;
    }
    if (m_rect == rect)
        return;
    m_rect = rect;
    rebuildGeometry();
}

void StemWaveformTextureNode::configure(const RenderTileSpan& span, double rasterScale,
                                       double linesPerSecond)
{
    m_span = span;
    m_rasterScale = rasterScale;
    m_linesPerSecond = linesPerSecond;
    m_energy.clear();
    m_data.reset();
    if (accelerated()) {
        static_cast<StemMaterial*>(m_shaderNode->material())->texture = texture();
        rebuildGeometry();
    }
}

void StemWaveformTextureNode::setStems(
    const std::shared_ptr<const brockdj::stems::StemWaveformOverlay>& overlay,
    const QColor& background)
{
    if (!accelerated())
        return;
    const auto data = overlay ? overlay->data : nullptr;
    const auto epoch = data ? data->envelopeEpoch() : 0;
    if (data != m_data || epoch != m_epoch || m_energy.empty()) {
        m_data = data;
        m_epoch = epoch;
        m_energy.assign(static_cast<std::size_t>(m_span.physicalWidth()), {});
        if (data && m_rasterScale > 0.0 && m_linesPerSecond > 0.0) {
            const double samplesPerPixel = data->sampleRate() / (m_linesPerSecond * m_rasterScale);
            for (std::size_t column = 0; column < m_energy.size(); ++column) {
                const auto pixel = m_span.physicalBegin + static_cast<std::int64_t>(column);
                auto& energy = m_energy[column];
                if (!data->envelopeEnergy(
                        static_cast<std::int64_t>(std::floor(pixel * samplesPerPixel)),
                        static_cast<std::int64_t>(std::ceil((pixel + 1) * samplesPerPixel)), energy))
                    continue;
                const float total = std::accumulate(energy.begin(), energy.end(), 0.0f);
                if (total > 1e-12f)
                    for (auto& value : energy)
                        value /= total;
                else
                    energy.fill(0.0f);
            }
        }
        rebuildGeometry();
    }
    auto* material = static_cast<StemMaterial*>(m_shaderNode->material());
    const auto gains = overlay ? overlay->gains : brockdj::stems::StemWaveformTransition::Gains {1, 1, 1, 1};
    const QVector4D nextGains(gains[0], gains[1], gains[2], gains[3]);
    const QVector4D nextBackground(background.redF(), background.greenF(), background.blueF(), background.alphaF());
    if (material->gains != nextGains || material->background != nextBackground) {
        material->gains = nextGains;
        material->background = nextBackground;
        m_shaderNode->markDirty(QSGNode::DirtyMaterial);
    }
}

void StemWaveformTextureNode::rebuildGeometry()
{
    if (!m_shaderNode)
        return;
    auto* geometry = m_shaderNode->geometry();
    const int columns = m_rect.isEmpty() ? 0 : static_cast<int>(m_span.physicalWidth());
    geometry->allocate(columns * 6);
    auto* vertices = static_cast<Vertex*>(geometry->vertexData());
    if (columns > 0 && texture()) {
        const auto source = m_sourceRect;
        const auto size = texture()->textureSize();
        const float top = static_cast<float>(m_rect.top());
        const float bottom = static_cast<float>(m_rect.bottom());
        for (int column = 0; column < columns; ++column) {
            const float left = static_cast<float>(m_rect.left() + m_rect.width() * column / columns);
            const float right = static_cast<float>(m_rect.left() + m_rect.width() * (column + 1) / columns);
            const float u0 = static_cast<float>((source.left() + source.width() * column / columns) / size.width());
            const float u1 = static_cast<float>((source.left() + source.width() * (column + 1) / columns) / size.width());
            const auto energy = column < static_cast<int>(m_energy.size())
                ? m_energy[column] : std::array<float, brockdj::stems::kStemCount> {};
            const Vertex a {left, top, u0, 0.0f, energy};
            const Vertex b {right, top, u1, 0.0f, energy};
            const Vertex c {left, bottom, u0, 1.0f, energy};
            const Vertex d {right, bottom, u1, 1.0f, energy};
            const int offset = column * 6;
            vertices[offset] = a;
            vertices[offset + 1] = b;
            vertices[offset + 2] = c;
            vertices[offset + 3] = c;
            vertices[offset + 4] = b;
            vertices[offset + 5] = d;
        }
    }
    m_shaderNode->markDirty(QSGNode::DirtyGeometry);
}

} // namespace waveform_render
