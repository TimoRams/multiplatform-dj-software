#pragma once

#include <QObject>
#include <QString>
#include <algorithm>
#include <cmath>
#include "waveform/WaveformLodPyramid.h"

class SettingsManager;

class WaveformZoomController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    // Human-readable zoom, relative to the default detail view: "1.0x" is that
    // default, larger is zoomed in. The raw value is a pixels-per-line scale and
    // means nothing to anyone reading it off a button.
    Q_PROPERTY(QString zoomLabel READ zoomLabel NOTIFY zoomChanged)
    // Position within the zoom range, 0 fully out to 1 fully in. The detents are
    // geometric, so this is measured on a log scale — a linear one would sit
    // near zero across most of the useful range.
    Q_PROPERTY(double zoomFraction READ zoomFraction NOTIFY zoomChanged)

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
    [[nodiscard]] QString zoomLabel() const { return zoomLabelFor(m_zoom); }
    [[nodiscard]] double zoomFraction() const noexcept { return zoomFractionFor(m_zoom); }

    [[nodiscard]] static QString zoomLabelFor(double value)
    {
        const double relative = validatedZoom(value) / kDefault;
        // Below 1x the steps are small in absolute terms, so a single decimal
        // would show the same number for several detents.
        return QStringLiteral("%1x").arg(relative,
                                         0,
                                         'f',
                                         relative < 1.0 ? 2 : 1);
    }
    [[nodiscard]] static double zoomFractionFor(double value) noexcept
    {
        const double span = std::log(kMaximum / kMinimum);
        if (!(span > 0.0))
            return 0.0;
        return std::clamp(std::log(validatedZoom(value) / kMinimum) / span, 0.0, 1.0);
    }

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
