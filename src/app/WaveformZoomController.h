#pragma once

#include <QObject>
#include <algorithm>
#include <cmath>
#include "waveform/WaveformLodPyramid.h"

class SettingsManager;

class WaveformZoomController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)

public:
    // CDJ-style long overview: roughly twelve more zoom-out detents below the
    // previous 0.030075 limit. At 1280 px this exposes about three minutes of
    // timeline while the normal 0.22 detail view remains unchanged.
    static constexpr double kMinimum = 0.0056;
    static constexpr double kMaximum = 10.0;
    static constexpr double kDefault = 0.22;
    static constexpr double kFactor = 1.15;

    explicit WaveformZoomController(SettingsManager* settings = nullptr, QObject* parent = nullptr);

    [[nodiscard]] double zoom() const noexcept { return m_zoom; }
    [[nodiscard]] static double validatedZoom(double value) noexcept
    {
        if (!std::isfinite(value))
            return kDefault;
        return std::clamp(value, kMinimum, kMaximum);
    }
    [[nodiscard]] static double increasedZoom(double value) noexcept
    {
        return validatedZoom(validatedZoom(value) * kFactor);
    }
    [[nodiscard]] static double decreasedZoom(double value) noexcept
    {
        return validatedZoom(validatedZoom(value) / kFactor);
    }
    [[nodiscard]] static std::uint8_t lodLevelForPhysicalPixels(
        double physicalPixelsPerCanonicalLine) noexcept
    {
        return waveform::WaveformLodPyramid::selectLevel(
            physicalPixelsPerCanonicalLine);
    }

    Q_INVOKABLE void setZoom(double value);
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void reset();

signals:
    void zoomChanged();

private:
    void persist();

    SettingsManager* m_settings = nullptr;
    double m_zoom = kDefault;
};
