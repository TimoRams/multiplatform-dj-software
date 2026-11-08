#include "waveform/render/StemWaveformAnimator.h"
#include "waveform/render/StemWaveformTextureNode.h"

#include <QGuiApplication>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QTest>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#endif

#include <iostream>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

class WaveformProbe final : public QQuickItem {
public:
    explicit WaveformProbe(QQuickItem* parent) : QQuickItem(parent)
    {
        setFlag(ItemHasContents);
        setSize({512, 128});
        data = std::make_shared<brockdj::stems::StemTrackData>(1, 76800, 512 * 64);
        for (std::int64_t sample = 0; sample < data->length(); ++sample)
            data->accumulateEnvelope(sample, sample < data->length() / 2
                ? std::array<float, 4> {1, 0, 0, 0} : std::array<float, 4> {0, 0, 0, 1});
        animator = new StemWaveformAnimator(this, [this] {
            overlay = animator->frame();
            update();
        });
        target({1, 1, 1, 1});
    }

    void target(const std::array<float, 4>& gains)
    {
        auto next = std::make_shared<brockdj::stems::StemWaveformOverlay>();
        next->data = data;
        next->gains = gains;
        next->revision = brockdj::stems::stemWaveformOverlayRevision(1, gains, data->envelopeEpoch());
        animator->setTarget(next);
    }

    int uploads = 0;
    int energyGeometryChanges = 0;

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override
    {
        auto* node = static_cast<waveform_render::StemWaveformTextureNode*>(oldNode);
        if (!node) {
            node = new waveform_render::StemWaveformTextureNode(true);
            QImage image(516, 128, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.fillRect(QRect(0, 20, 516, 88), QColor(235, 75, 40));
            painter.end();
            node->setTexture(window()->createTextureFromImage(image));
            node->setSourceRect({2, 0, 512, 128});
            node->configure({0, 0, 512, 0, 512}, 1, 1200);
            node->setRect(boundingRect());
            ++uploads;
        }
        node->setStems(overlay, QColor(16, 17, 20));
        auto* geometry = static_cast<QSGGeometryNode*>(node->firstChild())->geometry();
        const QByteArray vertices(static_cast<const char*>(geometry->vertexData()),
                                  geometry->vertexCount() * geometry->sizeOfVertex());
        if (vertices != previousVertices) {
            ++energyGeometryChanges;
            previousVertices = vertices;
        }
        return node;
    }

private:
    std::shared_ptr<brockdj::stems::StemTrackData> data;
    StemWaveformAnimator* animator = nullptr;
    std::shared_ptr<const brockdj::stems::StemWaveformOverlay> overlay;
    QByteArray previousVertices;
};

struct Pixels {
    int coloured = 0;
    int grey = 0;
};

Pixels classify(const QImage& image, int begin, int end)
{
    Pixels result;
    for (int y = 4; y < image.height() - 4; ++y)
        for (int x = begin; x < std::min(end, image.width()); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.red() > 150 && c.red() - c.blue() > 80)
                ++result.coloured;
            if (c.red() > 45 && c.red() < 110 && std::abs(c.red() - c.blue()) < 20)
                ++result.grey;
        }
    return result;
}
}

int main(int argc, char** argv)
{
    qputenv("QSG_RENDER_LOOP", "basic");
    QGuiApplication app(argc, argv);
    if (qEnvironmentVariable("QT_QUICK_BACKEND") == "software"
        || QGuiApplication::platformName() == "offscreen"
        || QGuiApplication::platformName() == "minimal") {
        std::cout << "GPU shader test requires an RHI backend\n";
        return 77;
    }
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    QVulkanInstance vulkan;
#else
    if (qEnvironmentVariable("QSG_RHI_BACKEND") == "vulkan") {
        std::cout << "Vulkan test requires Vulkan SDK headers\n";
        return 77;
    }
#endif
    QQuickWindow window;
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    if (qEnvironmentVariable("QSG_RHI_BACKEND") == "vulkan") {
        if (!vulkan.create()) {
            std::cout << "Vulkan renderer is unavailable\n";
            return 77;
        }
        window.setVulkanInstance(&vulkan);
    }
#endif
    window.setColor(QColor(16, 17, 20));
    window.resize(512, 128);
    auto* probe = new WaveformProbe(window.contentItem());
    window.show();
    QTest::qWait(100);
    const QImage full = window.grabWindow();
    if (window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        std::cout << "GPU shader test requires an RHI backend\n";
        return 77;
    }
    bool ok = require(!full.isNull(), "GPU waveform capture is available");
    if (full.isNull())
        return 1;
    const int split = full.width() / 2;
    const auto fullLeft = classify(full, 8, split - 8);
    const auto fullRight = classify(full, split + 8, full.width() - 8);
    ok &= require(fullLeft.coloured > 1000 && fullRight.coloured > 1000,
                  "unity gains preserve the full coloured mix");
    probe->target({0, 1, 1, 1});
    QTest::qWait(40);
    const QImage intermediate = window.grabWindow();
    const auto middle = classify(intermediate, 8, split - 8);
    ok &= require(middle.coloured > 0 && middle.coloured < fullLeft.coloured && middle.grey > 0,
                  "the first transition frames contract colour and smoothly reveal grey");
    QTest::qWait(200);
    const QImage muted = window.grabWindow();
    const auto mutedLeft = classify(muted, 8, split - 8);
    const auto mutedRight = classify(muted, split + 8, full.width() - 8);
    ok &= require(mutedLeft.coloured == 0 && mutedLeft.grey > 1000
                      && mutedRight.coloured == fullRight.coloured,
                  "only muted vocals become grey; unmuted drums keep their colour");
    for (int toggle = 0; toggle < 40; ++toggle) {
        probe->target(toggle % 2 == 0 ? std::array<float, 4> {1, 1, 1, 1}
                                      : std::array<float, 4> {0, 1, 1, 1});
        QTest::qWait(12);
        window.grabWindow();
    }
    probe->target({1, 1, 1, 1});
    QTest::qWait(220);
    const QImage restored = window.grabWindow();
    ok &= require(restored == full, "rapid toggles finish at the exact latest target without visual residue");
    ok &= require(probe->uploads == 1 && probe->energyGeometryChanges == 1,
                  "gain animation does not rerasterize, upload textures or rebuild energy geometry");
    if (ok)
        std::cout << "stem waveform GPU animation passed: 40 rapid toggles, one texture upload\n";
    return ok ? 0 : 1;
}
